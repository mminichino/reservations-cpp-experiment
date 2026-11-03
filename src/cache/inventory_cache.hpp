#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cache/flight_write_fence.hpp"
#include "common/env.hpp"
#include "common/result.hpp"
#include "domain/types.hpp"
#include "platform/couchbase_client.hpp"
#include "platform/kv_client_pool.hpp"

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

  static constexpr int kDefaultGetManyParallelism = 16;

  [[nodiscard]] FlightWriteFence::Guard Fence(std::string flight_id) {
    return fence_.Acquire(std::move(flight_id));
  }

  /// Wait for fence, then return cached entry or load from KV and fill.
  [[nodiscard]] Result<Entry> Get(platform::CouchbaseClient& client, std::string_view flight_id,
                                  std::uint64_t reset_epoch);

  /// Parallel Get for many flights. Each worker leases its own CouchbaseClient from a pool
  /// so KV ops overlap. Same per-flight fence/gen/load workflow as Get.
  [[nodiscard]] Result<std::unordered_map<std::string, Entry>> GetMany(
      const CouchbaseConfig& config, const std::vector<std::string>& flight_ids,
      std::uint64_t reset_epoch, int parallelism = kDefaultGetManyParallelism);

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
  [[nodiscard]] Result<void> EnsurePool(const CouchbaseConfig& config, int capacity);

  FlightWriteFence fence_;
  mutable std::mutex mu_;
  std::uint64_t approx_bytes_{0};
  std::unordered_map<std::string, Entry> entries_;

  std::mutex pool_mu_;
  std::unique_ptr<platform::KvClientPool> kv_pool_;
};

}  // namespace reservations::cache
