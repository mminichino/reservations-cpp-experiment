#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cache/flight_write_fence.hpp"
#include "common/result.hpp"
#include "domain/types.hpp"
#include "platform/couchbase_client.hpp"

namespace reservations::cache {

/// Write-through inventory cache. Local FlightWriteFence + Couchbase fence::inv::<id>.
/// Shop waits on fences; serves cache only if no remote fence was seen and inv_gen::<id>
/// CAS matches the cached entry; otherwise full inventory KV refill.
class InventoryCache {
 public:
  struct Entry {
    domain::InventoryDocument document;
    std::uint64_t cas{0};
    std::uint64_t resetEpoch{0};
    std::uint64_t bytes{0};
  };

  [[nodiscard]] FlightWriteFence::Guard Fence(std::string flight_id) {
    return fence_.Acquire(std::move(flight_id));
  }

  /// Wait for fence, then return cached entry or load from KV and fill.
  [[nodiscard]] Result<Entry> Get(platform::CouchbaseClient& client, std::string_view flight_id,
                                  std::uint64_t reset_epoch);

  /// Authoritative KV get (used under fence for BookFlight before CAS). Still waits for other writers.
  [[nodiscard]] Result<Entry> GetForUpdate(platform::CouchbaseClient& client, std::string_view flight_id,
                                           std::uint64_t reset_epoch);

  /// Write-through after successful CAS Replace.
  void Put(std::string_view flight_id, domain::InventoryDocument document, std::uint64_t cas,
           std::uint64_t reset_epoch);

  void Invalidate(std::string_view flight_id);

  [[nodiscard]] FlightWriteFence& fence() noexcept { return fence_; }

  [[nodiscard]] std::uint64_t ApproxBytes() const;

 private:
  [[nodiscard]] Result<Entry> LoadFromKv(platform::CouchbaseClient& client, std::string_view flight_id,
                                         std::uint64_t reset_epoch);

  FlightWriteFence fence_;
  mutable std::mutex mu_;
  std::uint64_t approx_bytes_{0};
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace reservations::cache
