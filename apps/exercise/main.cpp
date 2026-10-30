#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "availability/service.hpp"
#include "common/clock.hpp"
#include "common/env.hpp"
#include "common/uuid.hpp"
#include "platform/couchbase_client.hpp"
#include "schedule/catalog.hpp"
#include "schedule/network.hpp"
#include "seatmap/seed.hpp"
#include "seatmap/service.hpp"
#include "seatmap/synthetic.hpp"

namespace {

std::atomic<bool> g_running{true};

void OnSignal(int) { g_running.store(false, std::memory_order_relaxed); }

struct LatencyStats {
  std::mutex mu;
  std::uint64_t ok{0};
  std::uint64_t errors{0};
  std::uint64_t soft{0};
  std::vector<double> samples_ms;

  void RecordOk(double ms) {
    std::scoped_lock lock(mu);
    ++ok;
    samples_ms.push_back(ms);
  }
  void RecordError() {
    std::scoped_lock lock(mu);
    ++errors;
  }
  void RecordSoft() {
    std::scoped_lock lock(mu);
    ++soft;
  }

  struct Snapshot {
    std::uint64_t ok{0};
    std::uint64_t errors{0};
    std::uint64_t soft{0};
    double p50{0};
    double p95{0};
    double p99{0};
  };

  Snapshot TakeAndReset() {
    std::scoped_lock lock(mu);
    Snapshot snap{ok, errors, soft, 0, 0, 0};
    if (!samples_ms.empty()) {
      auto sorted = samples_ms;
      std::sort(sorted.begin(), sorted.end());
      auto pct = [&](double p) {
        const auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
        return sorted[idx];
      };
      snap.p50 = pct(0.50);
      snap.p95 = pct(0.95);
      snap.p99 = pct(0.99);
    }
    ok = errors = soft = 0;
    samples_ms.clear();
    return snap;
  }
};

struct MarketPair {
  std::string origin;
  std::string destination;
};

std::vector<MarketPair> BuildShopMarkets() {
  std::vector<MarketPair> out;
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& route : reservations::schedule::Routes()) {
    auto key = std::make_pair(std::string(route.origin), std::string(route.destination));
    if (seen.insert(key).second) {
      out.push_back(MarketPair{key.first, key.second});
    }
  }
  return out;
}

void Worker(reservations::CouchbaseConfig config, reservations::schedule::CatalogMeta meta,
            std::atomic<std::uint64_t>& tickets_sold, std::uint64_t total_au, LatencyStats& shop_stats,
            LatencyStats& book_stats, const std::vector<MarketPair>& markets) {
  reservations::platform::CouchbaseClient client(config);
  if (auto connected = client.Connect(); !connected) {
    std::cerr << "worker connect error: " << connected.error().message << '\n';
    return;
  }
  if (auto opened = client.OpenBucket(); !opened) {
    std::cerr << "worker open bucket error: " << opened.error().message << '\n';
    return;
  }

  reservations::SystemClock clock;
  reservations::availability::ClassAvailabilityService shop(client, clock);
  shop.SetCatalogMeta(meta);
  reservations::seatmap::ReservationService seats(client, clock);
  seats.SetCatalogMeta(meta);
  reservations::UuidGenerator uuids;
  std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> coin(0.0, 1.0);
  std::uniform_int_distribution<std::size_t> market_dist(0, markets.empty() ? 0 : markets.size() - 1);
  std::uniform_int_distribution<int> week_dist(0, reservations::schedule::kWeeksOut - 1);
  std::uniform_int_distribution<std::uint64_t> flight_dist(0, reservations::schedule::TotalFlights() - 1);

  while (g_running.load(std::memory_order_relaxed) &&
         tickets_sold.load(std::memory_order_relaxed) < total_au) {
    const bool do_purchase = coin(rng) < 0.01;

    if (!do_purchase) {
      if (markets.empty()) {
        shop_stats.RecordError();
        continue;
      }
      const auto& mkt = markets[market_dist(rng)];
      reservations::domain::ClassAvailabilityRequest req;
      req.requestId = uuids.Next();
      req.tripType = reservations::domain::TripType::one_way;
      req.salesChannel = reservations::domain::SalesChannel::direct_web;
      req.salesLocation = reservations::domain::SalesLocation{.country = "US", .city = "NYC"};
      req.passengers = {{.passengerType = reservations::domain::PassengerType::adult, .count = 1}};
      req.seatCountCap = 9;
      req.detailLevel = "segment";
      req.filters = reservations::domain::ClassAvailabilityFilters{.maxConnections = 1,
                                                                   .maxJourneysPerSlice = 40};
      const int week = week_dist(rng);
      req.slices = {reservations::domain::TravelSlice{
          .origin = mkt.origin,
          .destination = mkt.destination,
          .departureDate = reservations::schedule::LocalDepartureDate(week, static_cast<int>(rng() % 7)),
      }};

      const auto start = std::chrono::steady_clock::now();
      auto result = shop.Check(req);
      const auto ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      if (!result) {
        shop_stats.RecordError();
      } else {
        shop_stats.RecordOk(ms);
        std::uint64_t open_classes = 0;
        for (const auto& slice : result.value().slices) {
          for (const auto& j : slice.journeys) {
            for (const auto& c : j.classes) {
              if (c.status == reservations::domain::ClassStatus::open) {
                ++open_classes;
              }
            }
          }
        }
        if (open_classes == 0) {
          shop_stats.RecordSoft();
        }
      }
      continue;
    }

    // 1% book against a random catalog flight (class Y inventory).
    const auto ref = reservations::schedule::FlightRef::FromGlobalIndex(flight_dist(rng));
    const std::string flight_id = reservations::schedule::FlightId(ref);
    reservations::domain::FlightBookingRequest req;
    req.requestId = uuids.Next();
    req.idempotencyKey = "idempotency-key-" + uuids.Next().substr(0, 12);
    req.flightId = flight_id;
    req.fareClass = "Y";
    req.passengers = {reservations::domain::Passenger{
        .passengerId = "p1",
        .type = reservations::domain::PassengerType::adult,
        .firstName = "Test",
        .lastName = "Passenger1",
        .dateOfBirth = "1990-01-01",
    }};
    req.contact = reservations::domain::ContactInfo{.email = "test@example.com", .phone = "+15550000001"};

    const auto start = std::chrono::steady_clock::now();
    auto result = seats.BookFlight(req);
    const auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!result) {
      book_stats.RecordError();
    } else if (result.value().status == "confirmed") {
      book_stats.RecordOk(ms);
      tickets_sold.fetch_add(1, std::memory_order_relaxed);
    } else if (result.value().error.has_value() &&
               (result.value().error->code == "FLIGHT_CLOSED_FOR_SALES" ||
                result.value().error->code == "FLIGHT_FULL")) {
      book_stats.RecordSoft();
    } else {
      book_stats.RecordError();
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string env_path = (argc > 1) ? argv[1] : ".env";
  int threads = 4;
  if (argc > 2) {
    threads = std::max(1, std::atoi(argv[2]));
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  if (auto loaded = reservations::LoadDotEnv(env_path); !loaded) {
    std::cerr << "warning: " << loaded.error().message << " (continuing with process env)\n";
  }

  auto config = reservations::LoadCouchbaseConfig();
  if (!config) {
    std::cerr << "config error: " << config.error().message << '\n';
    return 1;
  }

  reservations::platform::CouchbaseClient bootstrap(config.value());
  if (auto connected = bootstrap.Connect(); !connected) {
    std::cerr << "connect error: " << connected.error().message << '\n';
    return 1;
  }
  if (auto topo = bootstrap.EnsureTopology(reservations::seatmap::AllCollections()); !topo) {
    std::cerr << "topology error: " << topo.error().message << '\n';
    return 1;
  }

  auto meta = reservations::seatmap::LoadCatalogMeta(bootstrap);
  if (!meta) {
    std::cerr << "catalog error: " << meta.error().message << " (run ./load first)\n";
    return 1;
  }

  const auto& m = meta.value();
  const std::uint64_t total_au =
      reservations::schedule::TotalFlights() * static_cast<std::uint64_t>(std::max(1, m.authorizationPerFlight));
  const auto markets = BuildShopMarkets();

  std::cout << "exercise: 99% ClassAvailability (O&D shop) / 1% BookFlight\n"
            << "  airlines=" << reservations::schedule::AirlineCount()
            << " routes=" << reservations::schedule::RouteCount()
            << " flights=" << reservations::schedule::TotalFlights() << " markets=" << markets.size()
            << " reset_epoch=" << m.resetEpoch << " AU/flight=" << m.authorizationPerFlight
            << " total_AU=" << total_au << '\n'
            << "  threads=" << threads << " (stops when all AU sold or Ctrl+C)\n\n";

  std::atomic<std::uint64_t> tickets_sold{0};
  LatencyStats shop_stats;
  LatencyStats book_stats;
  std::vector<std::thread> workers;
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back(Worker, config.value(), m, std::ref(tickets_sold), total_au, std::ref(shop_stats),
                         std::ref(book_stats), std::cref(markets));
  }

  std::cout << std::right << std::setw(6) << "#"
            << "  " << std::setw(10) << "shop/s" << "  " << std::setw(8) << "p50ms" << "  " << std::setw(8)
            << "p95ms" << "  " << std::setw(8) << "p99ms" << "  " << std::setw(6) << "err" << "  "
            << std::setw(7) << "empty"
            << "  |  " << std::setw(10) << "book/s" << "  " << std::setw(8) << "p50ms" << "  " << std::setw(8)
            << "p95ms" << "  " << std::setw(8) << "p99ms" << "  " << std::setw(6) << "err" << "  "
            << std::setw(6) << "full" << "  " << std::setw(12) << "sold" << "  " << std::setw(6) << "%AU"
            << '\n';
  std::cout << std::string(118, '-') << '\n';

  int tick = 0;
  while (g_running.load(std::memory_order_relaxed) &&
         tickets_sold.load(std::memory_order_relaxed) < total_au) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    ++tick;
    const auto shop = shop_stats.TakeAndReset();
    const auto book = book_stats.TakeAndReset();
    const auto sold = tickets_sold.load(std::memory_order_relaxed);
    const double pct = total_au == 0 ? 0.0 : (100.0 * static_cast<double>(sold) / static_cast<double>(total_au));
    std::cout << std::fixed << std::setprecision(1) << std::setw(6) << tick << "  " << std::setw(10)
              << (static_cast<double>(shop.ok) / 5.0) << "  " << std::setw(8) << shop.p50 << "  "
              << std::setw(8) << shop.p95 << "  " << std::setw(8) << shop.p99 << "  " << std::setw(6)
              << shop.errors << "  " << std::setw(7) << shop.soft << "  |  " << std::setw(10)
              << (static_cast<double>(book.ok) / 5.0) << "  " << std::setw(8) << book.p50 << "  "
              << std::setw(8) << book.p95 << "  " << std::setw(8) << book.p99 << "  " << std::setw(6)
              << book.errors << "  " << std::setw(6) << book.soft << "  " << std::setw(12) << sold << "  "
              << std::setw(6) << pct << '\n';
  }

  g_running.store(false, std::memory_order_relaxed);
  for (auto& w : workers) {
    w.join();
  }
  const auto sold = tickets_sold.load(std::memory_order_relaxed);
  std::cout << "\nStopped. tickets_sold=" << sold
            << (sold >= total_au ? " (catalog AU exhausted)\n" : " (interrupted)\n");
  return 0;
}
