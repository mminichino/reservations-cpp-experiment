#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/result.hpp"
#include "domain/pricing.hpp"
#include "platform/couchbase_client.hpp"
#include "platform/snapshot.hpp"

namespace reservations::cache {

/// Process-local cache of full market price sets keyed by market + price snapshotId.
/// Currency is checked with a tiny Get of `snapshot::prices` before serving.
class PriceSetCache {
 public:
  /// Returns all price records for the market under the current price snapshot.
  [[nodiscard]] Result<std::shared_ptr<const std::vector<domain::PriceRecord>>> GetMarketPrices(
      platform::CouchbaseClient& client, std::string_view origin, std::string_view destination);

  /// Last observed price snapshot from the tiny KV check (empty if never fetched).
  [[nodiscard]] platform::DataSnapshotDocument CurrentSnapshot() const;

  /// Approximate resident size of cached market price payloads (serialized JSON bytes).
  [[nodiscard]] std::uint64_t ApproxBytes() const;

 private:
  [[nodiscard]] Result<platform::DataSnapshotDocument> FetchSnapshot(platform::CouchbaseClient& client);
  [[nodiscard]] Result<std::vector<domain::PriceRecord>> LoadFromKv(platform::CouchbaseClient& client,
                                                                    std::string_view origin,
                                                                    std::string_view destination,
                                                                    std::string_view snapshot_id);

  struct MarketEntry {
    std::string snapshot_id;
    std::shared_ptr<const std::vector<domain::PriceRecord>> records;
    std::uint64_t bytes{0};
  };

  mutable std::mutex mu_;
  platform::DataSnapshotDocument snapshot_{};
  std::uint64_t approx_bytes_{0};
  // key = origin::destination
  std::unordered_map<std::string, MarketEntry> by_market_;
};

}  // namespace reservations::cache
