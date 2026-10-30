#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "common/env.hpp"
#include "platform/couchbase_client.hpp"
#include "schedule/catalog.hpp"
#include "seatmap/seed.hpp"

namespace {

void PrintUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " [env_file] [-j threads] [-d] [--subset N] [--start INDEX]\n"
            << "  env_file   path to .env (default: .env)\n"
            << "  -j N       parallel seed workers (default: hardware concurrency)\n"
            << "  -d         drop and recreate the bucket before seeding\n"
            << "  --subset N materialize only N flights (from --start; smoke/CI)\n"
            << "  --start I  global flight index to begin at (default: 0)\n"
            << "\nMaterializes market prices, then inventory/seatmap/flight/flight_index/od_index\n"
            << "for OneWorld+Star alliance routes. exercise expects a full catalog seed.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string env_path = ".env";
  int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  bool drop_bucket = false;
  std::uint64_t subset = 0;
  std::uint64_t start_index = 0;
  bool saw_env = false;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(arg, "-d") == 0) {
      drop_bucket = true;
      continue;
    }
    if (std::strcmp(arg, "-j") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "error: -j requires a thread count\n";
        return 1;
      }
      threads = std::max(1, std::atoi(argv[++i]));
      continue;
    }
    if (std::strcmp(arg, "--subset") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "error: --subset requires a count\n";
        return 1;
      }
      subset = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
      continue;
    }
    if (std::strcmp(arg, "--start") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "error: --start requires an index\n";
        return 1;
      }
      start_index = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
      continue;
    }
    if (arg[0] == '-') {
      std::cerr << "error: unknown option " << arg << '\n';
      PrintUsage(argv[0]);
      return 1;
    }
    if (!saw_env) {
      env_path = arg;
      saw_env = true;
    } else {
      std::cerr << "error: unexpected argument " << arg << '\n';
      PrintUsage(argv[0]);
      return 1;
    }
  }

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

  if (drop_bucket) {
    std::cout << "Dropping bucket '" << config.value().bucket << "'...\n";
    client.Close();
    if (auto dropped = client.DropBucket(); !dropped) {
      std::cerr << "drop error: " << dropped.error().message << '\n';
      return 1;
    }
    if (auto connected = client.Connect(); !connected) {
      std::cerr << "reconnect error: " << connected.error().message << '\n';
      return 1;
    }
  }

  const std::uint64_t catalog = reservations::schedule::TotalFlights();
  const std::uint64_t count = subset == 0 ? (catalog > start_index ? catalog - start_index : 0) : subset;

  std::cout << "Loading catalog (epoch bump + O&D prices + full materialization)...\n"
            << "  airlines=" << reservations::schedule::AirlineCount()
            << " routes=" << reservations::schedule::RouteCount()
            << " weeks=" << reservations::schedule::kWeeksOut << " catalog_flights=" << catalog
            << " price_records/market=" << reservations::schedule::kPriceRecordsPerMarket << '\n'
            << "  seed_start=" << start_index << " seed_count=" << count << " threads=" << threads
            << (drop_bucket ? " drop_bucket=yes" : "") << '\n';

  auto meta = reservations::seatmap::ResetCatalog(client);
  if (!meta) {
    std::cerr << "reset error: " << meta.error().message << '\n';
    return 1;
  }

  const auto& m = meta.value();
  const std::uint64_t total_au = catalog * static_cast<std::uint64_t>(m.authorizationPerFlight);

  std::cout << "  reset_epoch=" << m.resetEpoch << " snapshot=" << m.priceSnapshotId
            << " physical_capacity/flight=" << m.physicalCapacityPerFlight
            << " AU/flight=" << m.authorizationPerFlight << " total_AU=" << total_au << '\n'
            << "Materializing markets/prices then flights (stats every 10s)...\n";

  // Release bootstrap client before workers open their own connections.
  client.Close();

  reservations::seatmap::SeedOptions opts;
  opts.threads = threads;
  opts.start_index = start_index;
  opts.count = count;

  auto seeded = reservations::seatmap::SeedCatalog(config.value(), m, opts);
  if (!seeded) {
    std::cerr << "seed error: " << seeded.error().message << '\n';
    return 1;
  }

  std::cout << "Done. Catalog ready for exercise (avail=read, book=CAS update).\n";
  if (subset != 0 && start_index + count < catalog) {
    std::cout << "Note: partial seed (--subset); exercise samples the full catalog and will fail on "
                 "unmaterialized flights.\n";
  }
  return 0;
}
