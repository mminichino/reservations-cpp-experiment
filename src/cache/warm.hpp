#pragma once

#include "cache/client_caches.hpp"
#include "common/env.hpp"
#include "common/result.hpp"
#include "schedule/catalog.hpp"

namespace reservations::cache {

/// Full process-wide cache warm: all price markets (incl. hub legs), all OD×date indexes,
/// and every flight + inventory doc. Uses one Couchbase client per worker thread.
[[nodiscard]] Result<void> WarmAll(const CouchbaseConfig& config, ClientCaches& caches,
                                   const schedule::CatalogMeta& meta, int threads);

}  // namespace reservations::cache
