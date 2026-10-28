#pragma once

#include <cstdint>

#include "common/result.hpp"
#include "platform/couchbase_client.hpp"
#include "schedule/catalog.hpp"

namespace reservations::seatmap {

/// Ensures topology, bumps catalog reset epoch (all flights become fully open), seeds meta.
/// Does not rewrite per-flight documents — stale epoch is treated as empty inventory.
[[nodiscard]] Result<schedule::CatalogMeta> ResetCatalog(platform::CouchbaseClient& client);

[[nodiscard]] Result<schedule::CatalogMeta> LoadCatalogMeta(platform::CouchbaseClient& client);

}  // namespace reservations::seatmap
