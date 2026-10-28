#pragma once

#include <cstdint>

#include "common/env.hpp"
#include "common/result.hpp"
#include "platform/couchbase_client.hpp"
#include "schedule/catalog.hpp"

namespace reservations::seatmap {

struct SeedOptions {
  int threads{32};
  std::uint64_t start_index{0};
  /// Number of flights to materialize; 0 means through end of catalog from start_index.
  std::uint64_t count{0};
};

/// Ensures topology, bumps catalog reset epoch, seeds meta. Does not write per-flight docs.
[[nodiscard]] Result<schedule::CatalogMeta> ResetCatalog(platform::CouchbaseClient& client);

[[nodiscard]] Result<schedule::CatalogMeta> LoadCatalogMeta(platform::CouchbaseClient& client);

/// Parallel upsert of inventory, seatmap, flight, and flight_index for [start, start+count).
/// Each worker uses its own CouchbaseClient. Upsert-only with transient retries.
[[nodiscard]] Result<void> SeedCatalog(const CouchbaseConfig& config, const schedule::CatalogMeta& meta,
                                       SeedOptions options);

}  // namespace reservations::seatmap
