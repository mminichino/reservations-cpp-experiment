#include "seatmap/seed.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

#include <cstdio>

#include "domain/pricing.hpp"
#include "platform/document_store.hpp"
#include "pricing/engine.hpp"
#include "schedule/network.hpp"
#include "seatmap/service.hpp"
#include "seatmap/synthetic.hpp"

namespace reservations::seatmap {
namespace {

constexpr int kMaxAttempts = 8;
constexpr int kPricePageSize = 500;

template <typename Store, typename Doc>
Result<void> UpsertWithRetry(Store& store, std::string_view key, const Doc& doc) {
  Error last = Error::Make(ErrorCode::InternalError, "upsert retry exhausted");
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    auto upserted = store.Upsert(key, doc);
    if (upserted) {
      return Result<void>::Success();
    }
    last = upserted.error();
    if (!IsRetryable(last.code) && last.code != ErrorCode::InternalError &&
        last.code != ErrorCode::ConnectionError) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25 * attempt * attempt));
  }
  return Result<void>::Failure(last);
}

Result<void> AppendOdIndex(platform::CouchbaseClient& client, std::string_view origin,
                           std::string_view destination, std::string_view date,
                           const std::string& flight_id, std::uint64_t epoch) {
  platform::DocumentStore<domain::OdFlightIndexDocument> store(client, schedule::kOdIndexCollection);
  const std::string key = schedule::OdDateIndexKey(origin, destination, date);
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    auto loaded = store.Get(key);
    if (!loaded) {
      return Result<void>::Failure(loaded.error());
    }
    domain::OdFlightIndexDocument doc;
    std::uint64_t cas = 0;
    if (loaded.value().has_value()) {
      doc = loaded.value()->document;
      cas = loaded.value()->cas;
      if (std::find(doc.flightIds.begin(), doc.flightIds.end(), flight_id) != doc.flightIds.end()) {
        return Result<void>::Success();
      }
      doc.flightIds.push_back(flight_id);
      auto replaced = store.Replace(key, doc, cas);
      if (replaced) {
        return Result<void>::Success();
      }
      if (replaced.error().code != ErrorCode::VersionConflict) {
        return Result<void>::Failure(replaced.error());
      }
      continue;
    }
    doc.type = "od_index";
    doc.origin = std::string(origin);
    doc.destination = std::string(destination);
    doc.departureDate = std::string(date);
    doc.flightIds = {flight_id};
    doc.resetEpoch = epoch;
    auto inserted = store.Insert(key, doc);
    if (inserted) {
      return Result<void>::Success();
    }
    if (inserted.error().code != ErrorCode::AlreadyExists) {
      return Result<void>::Failure(inserted.error());
    }
  }
  return Result<void>::Failure(Error::Make(ErrorCode::InternalError, "od_index CAS retries exhausted"));
}

void SeedFlightRange(const CouchbaseConfig& config, const schedule::CatalogMeta& meta, std::uint64_t begin,
                     std::uint64_t end, std::atomic<std::uint64_t>& done,
                     std::atomic<std::uint64_t>& errors) {
  const std::uint64_t span = end > begin ? end - begin : 0;
  platform::CouchbaseClient client(config);
  if (auto connected = client.Connect(); !connected) {
    std::cerr << "seed worker connect failed: " << connected.error().message << '\n';
    errors.fetch_add(span, std::memory_order_relaxed);
    done.fetch_add(span, std::memory_order_relaxed);
    return;
  }
  if (auto opened = client.OpenBucket(); !opened) {
    std::cerr << "seed worker open bucket failed: " << opened.error().message << '\n';
    errors.fetch_add(span, std::memory_order_relaxed);
    done.fetch_add(span, std::memory_order_relaxed);
    return;
  }

  platform::DocumentStore<domain::Flight> flights(client, kFlightsCollection);
  platform::DocumentStore<domain::SeatMapDocument> seatmaps(client, kSeatmapsCollection);
  platform::DocumentStore<domain::InventoryDocument> inventory(client, kInventoryCollection);
  platform::DocumentStore<domain::FlightIndexDocument> flight_index(client, kFlightIndexCollection);

  for (std::uint64_t i = begin; i < end; ++i) {
    const auto ref = schedule::FlightRef::FromGlobalIndex(i);
    auto bundle = BuildCatalogFlight(ref, meta.resetEpoch);
    if (meta.physicalCapacityPerFlight > 0) {
      bundle.inventory.physicalCapacity = meta.physicalCapacityPerFlight;
    }
    bundle.inventory.oversellPercent = meta.oversellPercent;
    bundle.inventory.resetEpoch = meta.resetEpoch;
    bundle.seatmap.resetEpoch = meta.resetEpoch;

    const std::string& flight_id = bundle.flight.flightId;
    bool ok = true;
    if (!UpsertWithRetry(inventory, InventoryKey(flight_id), bundle.inventory)) {
      ok = false;
    }
    if (ok && !UpsertWithRetry(seatmaps, SeatMapKey(flight_id), bundle.seatmap)) {
      ok = false;
    }
    if (ok && !UpsertWithRetry(flights, FlightKey(flight_id), bundle.flight)) {
      ok = false;
    }
    if (ok) {
      const auto idx_key =
          FlightIndexKey(bundle.index.airlineCode, bundle.index.flightNumber, bundle.index.departureDate);
      if (!UpsertWithRetry(flight_index, idx_key, bundle.index)) {
        ok = false;
      }
    }
    if (ok) {
      auto od = AppendOdIndex(client, bundle.flight.origin, bundle.flight.destination,
                              bundle.index.departureDate, flight_id, meta.resetEpoch);
      if (!od) {
        ok = false;
      }
    }
    if (!ok) {
      errors.fetch_add(1, std::memory_order_relaxed);
    }
    done.fetch_add(1, std::memory_order_relaxed);
  }
}

Result<void> SeedMarkets(const CouchbaseConfig& config, const schedule::CatalogMeta& meta, int threads) {
  std::set<std::pair<std::string, std::string>> markets;
  for (const auto& route : schedule::Routes()) {
    markets.emplace(std::string(route.origin), std::string(route.destination));
  }
  std::vector<std::pair<std::string, std::string>> list(markets.begin(), markets.end());
  std::atomic<std::uint64_t> done{0};
  std::atomic<std::uint64_t> errors{0};
  const std::uint64_t total = list.size();
  const int workers = std::max(1, threads);
  const std::uint64_t chunk = (total + static_cast<std::uint64_t>(workers) - 1) / static_cast<std::uint64_t>(workers);

  std::vector<std::thread> pool;
  for (int t = 0; t < workers; ++t) {
    const std::uint64_t begin = static_cast<std::uint64_t>(t) * chunk;
    if (begin >= total) {
      break;
    }
    const std::uint64_t end = std::min(total, begin + chunk);
    pool.emplace_back([&, begin, end] {
      platform::CouchbaseClient client(config);
      if (!client.Connect() || !client.OpenBucket()) {
        errors.fetch_add(end - begin, std::memory_order_relaxed);
        done.fetch_add(end - begin, std::memory_order_relaxed);
        return;
      }
      platform::DocumentStore<domain::MarketMetaDocument> meta_store(client, schedule::kMarketsCollection);
      platform::DocumentStore<domain::MarketPricePageDocument> page_store(client, schedule::kPricesCollection);

      for (std::uint64_t i = begin; i < end; ++i) {
        const auto& [origin, destination] = list[static_cast<std::size_t>(i)];
        // Carrier hint: first airline that flies this O&D.
        std::string carrier = "AA";
        for (const auto& route : schedule::Routes()) {
          if (route.origin == origin && route.destination == destination) {
            carrier = std::string(schedule::Airline(route.airline_index).code);
            break;
          }
        }
        const std::string snapshot = meta.priceSnapshotId.empty() ? "snap-1" : meta.priceSnapshotId;
        auto records = pricing::GenerateMarketPrices(origin, destination, carrier,
                                                     meta.priceRecordsPerMarket, snapshot);
        const int page_count =
            std::max(1, (static_cast<int>(records.size()) + kPricePageSize - 1) / kPricePageSize);
        bool ok = true;
        for (int p = 0; p < page_count; ++p) {
          domain::MarketPricePageDocument page;
          page.type = "market_prices";
          page.marketOrigin = origin;
          page.marketDestination = destination;
          page.snapshotId = snapshot;
          page.pageNumber = p + 1;
          page.pageCount = page_count;
          page.resetEpoch = meta.resetEpoch;
          const auto from = static_cast<std::size_t>(p * kPricePageSize);
          const auto to = std::min(records.size(), from + static_cast<std::size_t>(kPricePageSize));
          page.records.assign(records.begin() + static_cast<std::ptrdiff_t>(from),
                              records.begin() + static_cast<std::ptrdiff_t>(to));
          char key[128];
          std::snprintf(key, sizeof(key), "prices::%s::%s::p%04d", origin.c_str(), destination.c_str(), p + 1);
          if (!UpsertWithRetry(page_store, key, page)) {
            ok = false;
            break;
          }
        }
        domain::MarketMetaDocument mdoc;
        mdoc.type = "market_meta";
        mdoc.origin = origin;
        mdoc.destination = destination;
        mdoc.snapshotId = snapshot;
        mdoc.totalRecords = static_cast<int>(records.size());
        mdoc.pageCount = page_count;
        mdoc.pageSize = kPricePageSize;
        mdoc.resetEpoch = meta.resetEpoch;
        const std::string meta_key = "market_meta::" + schedule::MarketKey(origin, destination);
        if (!UpsertWithRetry(meta_store, meta_key, mdoc)) {
          ok = false;
        }
        if (!ok) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
        done.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  const auto started = std::chrono::steady_clock::now();
  std::uint64_t tick = 0;
  auto last_print = started;
  while (done.load(std::memory_order_relaxed) < total) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto now = std::chrono::steady_clock::now();
    if (now - last_print >= std::chrono::seconds(10)) {
      ++tick;
      const auto finished = done.load(std::memory_order_relaxed);
      const double elapsed = std::chrono::duration<double>(now - started).count();
      const double rate = elapsed > 0 ? static_cast<double>(finished) / elapsed : 0.0;
      std::cout << "  [mkt " << std::setw(4) << tick << "] markets=" << finished << "/" << total
                << " rate=" << std::fixed << std::setprecision(1) << rate << "/s errors="
                << errors.load(std::memory_order_relaxed) << '\n';
      last_print = now;
    }
  }
  for (auto& t : pool) {
    t.join();
  }
  const auto err = errors.load();
  std::cout << "--- market price seed summary ---\n"
            << "  markets=" << total << " failed=" << err << " records_per_market="
            << meta.priceRecordsPerMarket << '\n';
  if (err > 0) {
    return Result<void>::Failure(
        Error::Make(ErrorCode::InternalError, "Market seed failures: " + std::to_string(err)));
  }
  return Result<void>::Success();
}

}  // namespace

Result<schedule::CatalogMeta> LoadCatalogMeta(platform::CouchbaseClient& client) {
  platform::DocumentStore<schedule::CatalogMeta> meta(client, schedule::kMetaCollection);
  auto loaded = meta.Get(schedule::kCatalogMetaKey);
  if (!loaded) {
    return Result<schedule::CatalogMeta>::Failure(loaded.error());
  }
  if (!loaded.value().has_value()) {
    return Result<schedule::CatalogMeta>::Failure(
        Error::Make(ErrorCode::NotFound, "Catalog meta missing — run load first"));
  }
  return Result<schedule::CatalogMeta>::Success(loaded.value()->document);
}

Result<schedule::CatalogMeta> ResetCatalog(platform::CouchbaseClient& client) {
  auto collections = AllCollections();
  collections.push_back(schedule::kMarketsCollection);
  collections.push_back(schedule::kPricesCollection);
  collections.push_back(schedule::kOdIndexCollection);
  auto topo = client.EnsureTopology(collections);
  if (!topo) {
    return Result<schedule::CatalogMeta>::Failure(topo.error());
  }

  platform::DocumentStore<schedule::CatalogMeta> meta_store(client, schedule::kMetaCollection);
  schedule::CatalogMeta meta;
  auto existing = meta_store.Get(schedule::kCatalogMetaKey);
  if (!existing) {
    return Result<schedule::CatalogMeta>::Failure(existing.error());
  }
  if (existing.value().has_value()) {
    meta = existing.value()->document;
    ++meta.resetEpoch;
  } else {
    meta.resetEpoch = 1;
  }
  meta.type = "catalog_meta";
  meta.airlineCount = schedule::AirlineCount();
  meta.routeCount = static_cast<int>(schedule::RouteCount());
  meta.weeksOut = schedule::kWeeksOut;
  meta.priceRecordsPerMarket = schedule::kPriceRecordsPerMarket;
  meta.oversellPercent = domain::kDefaultOversellPercent;
  meta.physicalCapacityPerFlight = TemplatePhysicalCapacity();
  meta.authorizationPerFlight =
      domain::AuthorizationFor(meta.physicalCapacityPerFlight, meta.oversellPercent);
  meta.priceSnapshotId = "snap-" + std::to_string(meta.resetEpoch);

  auto upserted = meta_store.Upsert(schedule::kCatalogMetaKey, meta);
  if (!upserted) {
    return Result<schedule::CatalogMeta>::Failure(upserted.error());
  }
  return Result<schedule::CatalogMeta>::Success(meta);
}

Result<void> SeedCatalog(const CouchbaseConfig& config, const schedule::CatalogMeta& meta,
                         SeedOptions options) {
  const std::uint64_t total = schedule::TotalFlights();
  if (options.start_index >= total) {
    return Result<void>::Failure(Error::Make(ErrorCode::ValidationFailed, "start_index past end of catalog"));
  }
  std::uint64_t count = options.count;
  if (count == 0) {
    count = total - options.start_index;
  }
  if (options.start_index + count > total) {
    count = total - options.start_index;
  }
  if (count == 0) {
    return Result<void>::Success();
  }

  std::cout << "Seeding market price sets...\n";
  if (auto markets = SeedMarkets(config, meta, options.threads); !markets) {
    return markets;
  }

  const int threads = std::max(1, options.threads);
  const std::uint64_t begin = options.start_index;
  const std::uint64_t end = begin + count;

  std::atomic<std::uint64_t> done{0};
  std::atomic<std::uint64_t> errors{0};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));

  const std::uint64_t chunk =
      (count + static_cast<std::uint64_t>(threads) - 1) / static_cast<std::uint64_t>(threads);
  std::cout << "Seeding flights (inventory+seatmap+flight+index+od_index)...\n";
  for (int t = 0; t < threads; ++t) {
    const std::uint64_t w_begin = begin + static_cast<std::uint64_t>(t) * chunk;
    if (w_begin >= end) {
      break;
    }
    const std::uint64_t w_end = std::min(end, w_begin + chunk);
    workers.emplace_back(SeedFlightRange, config, meta, w_begin, w_end, std::ref(done), std::ref(errors));
  }

  const auto started = std::chrono::steady_clock::now();
  std::uint64_t tick = 0;
  auto last_print = started;
  auto print_stats = [&]() {
    const auto finished = done.load(std::memory_order_relaxed);
    const auto err = errors.load(std::memory_order_relaxed);
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double rate = elapsed > 0.0 ? static_cast<double>(finished) / elapsed : 0.0;
    const double eta_s =
        (rate > 0.0 && finished < count) ? static_cast<double>(count - finished) / rate : 0.0;
    std::cout << "  [" << std::setw(4) << tick << "] flights=" << finished << "/" << count
              << "  rate=" << std::fixed << std::setprecision(1) << rate << "/s  errors=" << err
              << "  elapsed=" << std::setprecision(0) << elapsed << "s";
    if (finished < count) {
      std::cout << "  eta=" << static_cast<int>(eta_s) << "s";
    }
    std::cout << '\n';
  };

  while (done.load(std::memory_order_relaxed) < count) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto now = std::chrono::steady_clock::now();
    if (now - last_print >= std::chrono::seconds(10)) {
      ++tick;
      print_stats();
      last_print = now;
    }
  }
  for (auto& w : workers) {
    w.join();
  }

  const auto err = errors.load(std::memory_order_relaxed);
  const auto finished = done.load(std::memory_order_relaxed);
  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const std::uint64_t ok_flights = finished > err ? finished - err : 0;
  const double rate = elapsed > 0.0 ? static_cast<double>(finished) / elapsed : 0.0;

  std::cout << "\n--- seed telemetry summary ---\n"
            << "  flights_total=" << count << '\n'
            << "  flights_ok=" << ok_flights << '\n'
            << "  flights_failed=" << err << '\n'
            << "  routes=" << schedule::RouteCount() << " airlines=" << schedule::AirlineCount() << '\n'
            << "  threads=" << threads << '\n'
            << "  wall_seconds=" << std::fixed << std::setprecision(1) << elapsed << '\n'
            << "  avg_flights_per_sec=" << std::setprecision(1) << rate << '\n'
            << "------------------------------\n";

  if (err > 0) {
    return Result<void>::Failure(Error::Make(
        ErrorCode::InternalError, "Seed finished with " + std::to_string(err) + " flight failures"));
  }
  return Result<void>::Success();
}

}  // namespace reservations::seatmap
