#include "seatmap/seed.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include "platform/document_store.hpp"
#include "seatmap/service.hpp"
#include "seatmap/synthetic.hpp"

namespace reservations::seatmap {
namespace {

constexpr int kMaxAttempts = 8;

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

void SeedRange(const CouchbaseConfig& config, const schedule::CatalogMeta& meta, std::uint64_t begin,
               std::uint64_t end, std::atomic<std::uint64_t>& done, std::atomic<std::uint64_t>& errors) {
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
    // Prefer AU/capacity from catalog meta when present (set by ResetCatalog).
    if (meta.physicalCapacityPerFlight > 0) {
      bundle.inventory.physicalCapacity = meta.physicalCapacityPerFlight;
    }
    if (meta.authorizationPerFlight > 0) {
      bundle.inventory.authorization = meta.authorizationPerFlight;
    }
    bundle.inventory.oversellPercent = meta.oversellPercent;
    bundle.inventory.sold = 0;
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
    if (!ok) {
      errors.fetch_add(1, std::memory_order_relaxed);
    }
    done.fetch_add(1, std::memory_order_relaxed);
  }
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
  auto topo = client.EnsureTopology(AllCollections());
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
  meta.airlines = schedule::kAirlineCount;
  meta.flightsPerWeekPerAirline = schedule::kFlightsPerWeekPerAirline;
  meta.weeksOut = schedule::kWeeksOut;
  meta.oversellPercent = domain::kDefaultOversellPercent;
  meta.physicalCapacityPerFlight = TemplatePhysicalCapacity();
  meta.authorizationPerFlight =
      domain::AuthorizationFor(meta.physicalCapacityPerFlight, meta.oversellPercent);

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

  const int threads = std::max(1, options.threads);
  const std::uint64_t begin = options.start_index;
  const std::uint64_t end = begin + count;

  std::atomic<std::uint64_t> done{0};
  std::atomic<std::uint64_t> errors{0};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));

  const std::uint64_t chunk =
      (count + static_cast<std::uint64_t>(threads) - 1) / static_cast<std::uint64_t>(threads);
  for (int t = 0; t < threads; ++t) {
    const std::uint64_t w_begin = begin + static_cast<std::uint64_t>(t) * chunk;
    if (w_begin >= end) {
      break;
    }
    const std::uint64_t w_end = std::min(end, w_begin + chunk);
    workers.emplace_back(SeedRange, config, meta, w_begin, w_end, std::ref(done), std::ref(errors));
  }

  constexpr int kDocsPerFlight = 4;  // inventory, seatmap, flight, flight_index
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
    const std::uint64_t ok_flights = finished > err ? finished - err : 0;
    const std::uint64_t docs_ok = ok_flights * static_cast<std::uint64_t>(kDocsPerFlight);
    std::cout << "  [" << std::setw(4) << tick << "] "
              << "flights=" << finished << "/" << count << "  rate=" << std::fixed << std::setprecision(1)
              << rate << "/s  docs_ok~=" << docs_ok << "  errors=" << err
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
            << "  docs_upserted_ok~=" << (ok_flights * static_cast<std::uint64_t>(kDocsPerFlight))
            << " (inventory+seatmap+flight+flight_index)\n"
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
