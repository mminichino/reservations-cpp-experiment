#include <iostream>

#include "common/env.hpp"
#include "platform/couchbase_client.hpp"
#include "schedule/catalog.hpp"
#include "seatmap/seed.hpp"

int main(int argc, char** argv) {
  const std::string env_path = (argc > 1) ? argv[1] : ".env";
  if (auto loaded = reservations::LoadDotEnv(env_path); !loaded) {
    std::cerr << "warning: " << loaded.error().message << " (continuing with process env)\n";
  }

  auto config = reservations::LoadCouchbaseConfig();
  if (!config) {
    std::cerr << "config error: " << config.error().message << '\n';
    return 1;
  }

  reservations::platform::CouchbaseClient client(config.value());
  if (auto connected = client.Connect(); !connected) {
    std::cerr << "connect error: " << connected.error().message << '\n';
    return 1;
  }

  std::cout << "Resetting catalog inventory (epoch bump)...\n"
            << "  airlines=" << reservations::schedule::kAirlineCount
            << " flights/week/airline=" << reservations::schedule::kFlightsPerWeekPerAirline
            << " weeks=" << reservations::schedule::kWeeksOut
            << " total_flights=" << reservations::schedule::TotalFlights() << '\n';

  auto meta = reservations::seatmap::ResetCatalog(client);
  if (!meta) {
    std::cerr << "reset error: " << meta.error().message << '\n';
    return 1;
  }

  const auto& m = meta.value();
  const std::uint64_t total_au =
      reservations::schedule::TotalFlights() * static_cast<std::uint64_t>(m.authorizationPerFlight);

  std::cout << "Done.\n"
            << "  reset_epoch=" << m.resetEpoch << '\n'
            << "  physical_capacity/flight=" << m.physicalCapacityPerFlight << '\n'
            << "  authorization/flight=" << m.authorizationPerFlight << " (oversell "
            << static_cast<int>(m.oversellPercent * 100) << "%)\n"
            << "  total_sellable_seats(AU)=" << total_au << '\n'
            << "  note: per-flight docs are materialized lazily; stale epochs are treated as fully open\n";
  return 0;
}
