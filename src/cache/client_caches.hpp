#pragma once

#include <memory>

#include "cache/inventory_cache.hpp"
#include "cache/price_set_cache.hpp"
#include "cache/schedule_cache.hpp"

namespace reservations::cache {

/// Per-worker (or shared) bundle of process-local caches for avail/book hot paths.
struct ClientCaches {
  std::shared_ptr<PriceSetCache> prices{std::make_shared<PriceSetCache>()};
  std::shared_ptr<ScheduleCache> schedule{std::make_shared<ScheduleCache>()};
  std::shared_ptr<InventoryCache> inventory{std::make_shared<InventoryCache>()};
};

}  // namespace reservations::cache
