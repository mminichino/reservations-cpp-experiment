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

 private:
  [[nodiscard]] Result<platform::DataSnapshotDocument> FetchSnapshot(platform::CouchbaseClient& client);
  [[nodiscard]] Result<std::vector<domain::PriceRecord>> LoadFromKv(platform::CouchbaseClient& client,
                                                                    std::string_view origin,
                                                                    std::string_view destination,
                                                                    std::string_view snapshot_id);

  mutable std::mutex mu_;
  platform::DataSnapshotDocument snapshot_{};
  // key = origin::destination
  std::unordered_map<std::string, std::pair<std::string, std::shared_ptr<const std::vector<domain::PriceRecord>>>>
      by_market_;
};

}  // namespace reservations::cache
