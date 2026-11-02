#pragma once

#include <cstdint>
#include <memory>

#include "cache/inventory_cache.hpp"
#include "cache/price_set_cache.hpp"
#include "cache/schedule_cache.hpp"

namespace reservations::cache {

/// Shared process-wide cache bundle for avail/book hot paths (thread-safe via per-cache mutexes).
struct ClientCaches {
  std::shared_ptr<PriceSetCache> prices{std::make_shared<PriceSetCache>()};
  std::shared_ptr<ScheduleCache> schedule{std::make_shared<ScheduleCache>()};
  std::shared_ptr<InventoryCache> inventory{std::make_shared<InventoryCache>()};

  [[nodiscard]] std::uint64_t ApproxBytes() const {
    return prices->ApproxBytes() + schedule->ApproxBytes() + inventory->ApproxBytes();
  }
};

}  // namespace reservations::cache
