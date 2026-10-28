#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common/clock.hpp"
#include "common/env.hpp"
#include "common/uuid.hpp"
#include "domain/messages.hpp"
#include "platform/couchbase_client.hpp"
#include "schedule/catalog.hpp"
#include "seatmap/seed.hpp"
#include "seatmap/service.hpp"

namespace {

std::atomic<bool> g_running{true};

void OnSignal(int) { g_running.store(false, std::memory_order_relaxed); }

struct LatencyStats {
  std::mutex mutex;
  std::vector<double> samples_ms;
  std::uint64_t ok{0};
  std::uint64_t errors{0};
  std::uint64_t soft{0};

  void RecordOk(double ms) {
    std::scoped_lock lock(mutex);
    samples_ms.push_back(ms);
    ++ok;
  }

  void RecordError() {
    std::scoped_lock lock(mutex);
    ++errors;
  }

  void RecordSoft() {
    std::scoped_lock lock(mutex);
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
    std::scoped_lock lock(mutex);
    Snapshot snap{ok, errors, soft, 0, 0, 0};
    if (!samples_ms.empty()) {
      auto sorted = samples_ms;
      std::sort(sorted.begin(), sorted.end());
      auto pct = [&](double p) {
        const auto idx = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size()))) - 1;
        return sorted[std::min(idx, sorted.size() - 1)];
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

void Worker(reservations::CouchbaseConfig config, reservations::schedule::CatalogMeta meta,
            std::atomic<std::uint64_t>& tickets_sold, std::uint64_t total_au, LatencyStats& avail_stats,
            LatencyStats& book_stats) {
  reservations::platform::CouchbaseClient client(std::move(config));
  if (auto connected = client.Connect(); !connected) {
    std::cerr << "worker connect error: " << connected.error().message << '\n';
    return;
  }
  if (auto opened = client.OpenBucket(); !opened) {
    std::cerr << "worker open bucket error: " << opened.error().message << '\n';
    return;
  }

  reservations::SystemClock clock;
  reservations::seatmap::ReservationService service(client, clock);
  service.SetCatalogMeta(meta);
  reservations::UuidGenerator uuids;
  std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> coin(0.0, 1.0);
  std::uniform_int_distribution<std::uint64_t> flight_dist(0, reservations::schedule::TotalFlights() - 1);

  while (g_running.load(std::memory_order_relaxed) &&
         tickets_sold.load(std::memory_order_relaxed) < total_au) {
    const bool do_purchase = coin(rng) < 0.01;  // 1% buy, 99% availability
    const auto ref = reservations::schedule::FlightRef::FromGlobalIndex(flight_dist(rng));
    const std::string flight_id = reservations::schedule::FlightId(ref);

    if (!do_purchase) {
      reservations::domain::SeatAvailabilityRequest req;
      req.requestId = uuids.Next();
      req.flight = reservations::domain::FlightById{.flightId = flight_id};
      const auto start = std::chrono::steady_clock::now();
      auto result = service.CheckAvailability(req);
      const auto ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      if (!result) {
        avail_stats.RecordError();
      } else {
        avail_stats.RecordOk(ms);
        if (result.value().seats.empty()) {
          avail_stats.RecordSoft();  // no selectable seats (still a successful check)
        }
      }
      continue;
    }

    reservations::domain::FlightBookingRequest req;
    req.requestId = uuids.Next();
    req.idempotencyKey = "idempotency-key-" + uuids.Next().substr(0, 12);
    req.flightId = flight_id;
    req.passengers = {reservations::domain::Passenger{
        .passengerId = "p1",
        .type = reservations::domain::PassengerType::adult,
        .firstName = "Test",
        .lastName = "Passenger1",
        .dateOfBirth = "1990-01-01",
    }};
    req.contact = reservations::domain::ContactInfo{.email = "test@example.com", .phone = "+15550000001"};

    const auto start = std::chrono::steady_clock::now();
    auto result = service.BookFlight(req);
    const auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!result) {
      book_stats.RecordError();
    } else if (result.value().status == "confirmed") {
      book_stats.RecordOk(ms);
      tickets_sold.fetch_add(1, std::memory_order_relaxed);
    } else if (result.value().error.has_value() &&
               result.value().error->code == "FLIGHT_CLOSED_FOR_SALES") {
      book_stats.RecordSoft();  // that flight's AU is exhausted
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
      reservations::schedule::TotalFlights() * static_cast<std::uint64_t>(m.authorizationPerFlight);

  std::cout << "exercise: 99% availability / 1% ticket purchase\n"
            << "  catalog flights=" << reservations::schedule::TotalFlights()
            << " reset_epoch=" << m.resetEpoch << " AU/flight=" << m.authorizationPerFlight
            << " total_AU=" << total_au << '\n'
            << "  threads=" << threads << " (stops when all AU sold or Ctrl+C)\n\n";

  std::atomic<std::uint64_t> tickets_sold{0};
  LatencyStats avail_stats;
  LatencyStats book_stats;

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back(Worker, config.value(), m, std::ref(tickets_sold), total_au, std::ref(avail_stats),
                         std::ref(book_stats));
  }

  std::cout << std::right << std::setw(6) << "#"
            << "  " << std::setw(10) << "avail/s"
            << "  " << std::setw(8) << "p50ms"
            << "  " << std::setw(8) << "p95ms"
            << "  " << std::setw(8) << "p99ms"
            << "  " << std::setw(6) << "err"
            << "  " << std::setw(7) << "empty"
            << "  |  "
            << std::setw(10) << "book/s"
            << "  " << std::setw(8) << "p50ms"
            << "  " << std::setw(8) << "p95ms"
            << "  " << std::setw(8) << "p99ms"
            << "  " << std::setw(6) << "err"
            << "  " << std::setw(6) << "full"
            << "  " << std::setw(12) << "sold"
            << "  " << std::setw(6) << "%AU" << '\n';
  std::cout << std::string(130, '-') << '\n';

  std::uint64_t interval = 0;
  while (g_running.load(std::memory_order_relaxed) &&
         tickets_sold.load(std::memory_order_relaxed) < total_au) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    ++interval;
    const auto avail = avail_stats.TakeAndReset();
    const auto book = book_stats.TakeAndReset();
    const auto sold = tickets_sold.load(std::memory_order_relaxed);
    const double pct = total_au == 0 ? 0.0 : (100.0 * static_cast<double>(sold) / static_cast<double>(total_au));
    std::cout << std::right << std::fixed << std::setprecision(1) << std::setw(6) << interval << "  "
              << std::setw(10) << (static_cast<double>(avail.ok) / 5.0) << "  " << std::setw(8) << avail.p50
              << "  " << std::setw(8) << avail.p95 << "  " << std::setw(8) << avail.p99 << "  " << std::setw(6)
              << avail.errors << "  " << std::setw(7) << avail.soft << "  |  " << std::setw(10)
              << (static_cast<double>(book.ok) / 5.0) << "  " << std::setw(8) << book.p50 << "  " << std::setw(8)
              << book.p95 << "  " << std::setw(8) << book.p99 << "  " << std::setw(6) << book.errors << "  "
              << std::setw(6) << book.soft << "  " << std::setw(12) << sold << "  " << std::setw(6) << pct
              << std::endl;
  }

  g_running.store(false, std::memory_order_relaxed);
  for (auto& t : workers) {
    if (t.joinable()) {
      t.join();
    }
  }

  const auto sold = tickets_sold.load();
  std::cout << "\nStopped. tickets_sold=" << sold << " / " << total_au
            << (sold >= total_au ? " (catalog AU exhausted)\n" : " (interrupted)\n");
  return 0;
}
