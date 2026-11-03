#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cache/flight_write_fence.hpp"
#include "common/env.hpp"
#include "common/result.hpp"
#include "domain/types.hpp"
#include "platform/couchbase_client.hpp"
#include "platform/kv_worker_pool.hpp"

namespace reservations::cache {

/// Write-through inventory cache. Local FlightWriteFence + Couchbase fence::inv::<id>.
/// Shop waits on fences; serves cache only if no remote fence was seen and a subdoc CAS
/// probe on the inventory key still matches the cached entry; otherwise full inventory refill.
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
  [[nodiscard]] Result<std::shared_ptr<const Entry>> Get(platform::CouchbaseClient& client,
                                                         std::string_view flight_id,
                                                         std::uint64_t reset_epoch);

  /// Parallel Get for many flights via a persistent KvWorkerPool (dedicated client per worker).
  [[nodiscard]] Result<std::unordered_map<std::string, std::shared_ptr<const Entry>>> GetMany(
      const CouchbaseConfig& config, const std::vector<std::string>& flight_ids,
      std::uint64_t reset_epoch, int parallelism = kDefaultGetManyParallelism);

  /// Authoritative KV get (used under fence for BookFlight before CAS). Still waits for other writers.
  [[nodiscard]] Result<std::shared_ptr<const Entry>> GetForUpdate(platform::CouchbaseClient& client,
                                                                  std::string_view flight_id,
                                                                  std::uint64_t reset_epoch);

  /// Write-through after successful CAS Replace.
  void Put(std::string_view flight_id, domain::InventoryDocument document, std::uint64_t cas,
           std::uint64_t reset_epoch);

  void Invalidate(std::string_view flight_id);

  [[nodiscard]] FlightWriteFence& fence() noexcept { return fence_; }

  [[nodiscard]] std::uint64_t ApproxBytes() const;

 private:
  [[nodiscard]] Result<std::shared_ptr<const Entry>> LoadFromKv(platform::CouchbaseClient& client,
                                                                std::string_view flight_id,
                                                                std::uint64_t reset_epoch);
  [[nodiscard]] Result<void> EnsureWorkers(const CouchbaseConfig& config, int parallelism);
  void StoreEntry(std::string_view flight_id, std::shared_ptr<const Entry> entry);

  FlightWriteFence fence_;
  mutable std::mutex mu_;
  std::uint64_t approx_bytes_{0};
  std::unordered_map<std::string, std::shared_ptr<const Entry>> entries_;

  std::mutex pool_mu_;
  std::unique_ptr<platform::KvWorkerPool> workers_;
};

}  // namespace reservations::cache
