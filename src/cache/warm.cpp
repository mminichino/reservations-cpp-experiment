#include "cache/warm.hpp"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform/couchbase_client.hpp"
#include "schedule/network.hpp"

namespace reservations::cache {
namespace {

struct MarketPair {
  std::string origin;
  std::string destination;
};

std::vector<MarketPair> BuildWarmMarkets() {
  std::set<std::pair<std::string, std::string>> seen;
  std::vector<MarketPair> out;

  auto add = [&](std::string_view o, std::string_view d) {
    if (o.empty() || d.empty() || o == d) {
      return;
    }
    auto key = std::make_pair(std::string(o), std::string(d));
    if (seen.insert(key).second) {
      out.push_back(MarketPair{key.first, key.second});
    }
  };

  for (const auto& route : schedule::Routes()) {
    add(route.origin, route.destination);
    for (const auto hub : schedule::kConnectionHubs) {
      if (hub == route.origin || hub == route.destination) {
        continue;
      }
      add(route.origin, hub);
      add(hub, route.destination);
    }
  }
  return out;
}

std::vector<std::string> BuildWarmDates() {
  std::set<std::string> seen;
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(schedule::kWeeksOut) * 7U);
  for (int week = 0; week < schedule::kWeeksOut; ++week) {
    for (int day = 0; day < 7; ++day) {
      auto date = schedule::LocalDepartureDate(week, day);
      if (seen.insert(date).second) {
        out.push_back(std::move(date));
      }
    }
  }
  return out;
}

template <typename Body>
Result<void> RunChunked(std::uint64_t count, int threads, const CouchbaseConfig& config, Body&& body) {
  if (count == 0) {
    return Result<void>::Success();
  }
  const int workers = std::max(1, threads);
  const std::uint64_t chunk =
      (count + static_cast<std::uint64_t>(workers) - 1) / static_cast<std::uint64_t>(workers);

  std::atomic<std::uint64_t> errors{0};
  std::mutex err_mu;
  Error first_error = Error::Make(ErrorCode::InternalError, "warm failed");

  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(workers));
  for (int t = 0; t < workers; ++t) {
    const std::uint64_t begin = static_cast<std::uint64_t>(t) * chunk;
    if (begin >= count) {
      break;
    }
    const std::uint64_t end = std::min(count, begin + chunk);
    pool.emplace_back([&, begin, end] {
      platform::CouchbaseClient client(config);
      if (auto connected = client.Connect(); !connected) {
        if (errors.fetch_add(1, std::memory_order_relaxed) == 0) {
          std::lock_guard lock(err_mu);
          first_error = connected.error();
        }
        return;
      }
      if (auto opened = client.OpenBucket(); !opened) {
        if (errors.fetch_add(1, std::memory_order_relaxed) == 0) {
          std::lock_guard lock(err_mu);
          first_error = opened.error();
        }
        return;
      }
      for (std::uint64_t i = begin; i < end; ++i) {
        auto step = body(client, i);
        if (!step) {
          if (errors.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::lock_guard lock(err_mu);
            first_error = step.error();
          }
          return;
        }
      }
    });
  }
  for (auto& th : pool) {
    th.join();
  }
  if (errors.load(std::memory_order_relaxed) > 0) {
    return Result<void>::Failure(first_error);
  }
  return Result<void>::Success();
}

}  // namespace

Result<void> WarmAll(const CouchbaseConfig& config, ClientCaches& caches,
                     const schedule::CatalogMeta& meta, int threads) {
  const auto markets = BuildWarmMarkets();
  const auto dates = BuildWarmDates();
  const std::uint64_t flight_count = schedule::TotalFlights();
  const std::uint64_t od_count =
      static_cast<std::uint64_t>(markets.size()) * static_cast<std::uint64_t>(dates.size());

  std::cout << "cache warm prerun:\n"
            << "  markets=" << markets.size() << " dates=" << dates.size()
            << " od_keys=" << od_count << " flights=" << flight_count << " threads=" << threads
            << '\n';

  const auto started = std::chrono::steady_clock::now();

  std::cout << "  warming price sets (" << markets.size() << ")...\n";
  if (auto prices = RunChunked(markets.size(), threads, config,
                               [&](platform::CouchbaseClient& client, std::uint64_t i) -> Result<void> {
                                 const auto& mkt = markets[static_cast<std::size_t>(i)];
                                 auto loaded = caches.prices->GetMarketPrices(client, mkt.origin, mkt.destination);
                                 if (!loaded) {
                                   return Result<void>::Failure(loaded.error());
                                 }
                                 return Result<void>::Success();
                               });
      !prices) {
    return prices;
  }
  std::cout << "  prices warm ok; approx_bytes=" << caches.ApproxBytes() << '\n';

  std::cout << "  warming od_index (" << od_count << ")...\n";
  std::atomic<std::uint64_t> od_done{0};
  if (auto ods = RunChunked(od_count, threads, config,
                            [&](platform::CouchbaseClient& client, std::uint64_t i) -> Result<void> {
                              const std::uint64_t mi = i / dates.size();
                              const std::uint64_t di = i % dates.size();
                              const auto& mkt = markets[static_cast<std::size_t>(mi)];
                              auto loaded = caches.schedule->GetDirectFlights(
                                  client, mkt.origin, mkt.destination, dates[static_cast<std::size_t>(di)],
                                  meta.resetEpoch);
                              if (!loaded) {
                                return Result<void>::Failure(loaded.error());
                              }
                              const auto finished = od_done.fetch_add(1, std::memory_order_relaxed) + 1;
                              if (finished % 50000ULL == 0ULL || finished == od_count) {
                                std::cout << "    od " << finished << "/" << od_count << '\n';
                              }
                              return Result<void>::Success();
                            });
      !ods) {
    return ods;
  }

  std::cout << "  warming flights+inventory (" << flight_count << ")...\n";
  std::atomic<std::uint64_t> flight_done{0};
  if (auto flights =
          RunChunked(flight_count, threads, config,
                     [&](platform::CouchbaseClient& client, std::uint64_t i) -> Result<void> {
                       const auto ref = schedule::FlightRef::FromGlobalIndex(i);
                       const std::string flight_id = schedule::FlightId(ref);
                       auto flight = caches.schedule->GetFlight(client, flight_id);
                       if (!flight) {
                         return Result<void>::Failure(flight.error());
                       }
                       auto inv = caches.inventory->Get(client, flight_id, meta.resetEpoch);
                       if (!inv) {
                         return Result<void>::Failure(inv.error());
                       }
                       const auto finished = flight_done.fetch_add(1, std::memory_order_relaxed) + 1;
                       if (finished % 20000ULL == 0ULL || finished == flight_count) {
                         std::cout << "    flights " << finished << "/" << flight_count << '\n';
                       }
                       return Result<void>::Success();
                     });
      !flights) {
    return flights;
  }

  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "cache warm complete: wall_seconds=" << std::fixed << std::setprecision(1) << elapsed
            << " approx_bytes=" << caches.ApproxBytes() << '\n';
  return Result<void>::Success();
}

}  // namespace reservations::cache
