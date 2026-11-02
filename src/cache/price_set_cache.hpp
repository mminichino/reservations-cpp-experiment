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

/// Process-local cache of full market price sets.
/// Snapshot id is pinned once (experiment treats price data as static) — no per-call KV currency check.
class PriceSetCache {
 public:
  void PinSnapshot(platform::DataSnapshotDocument snapshot);

  [[nodiscard]] Result<std::shared_ptr<const std::vector<domain::PriceRecord>>> GetMarketPrices(
      platform::CouchbaseClient& client, std::string_view origin, std::string_view destination);

  [[nodiscard]] platform::DataSnapshotDocument CurrentSnapshot() const;

  [[nodiscard]] std::uint64_t ApproxBytes() const;

 private:
  [[nodiscard]] Result<std::vector<domain::PriceRecord>> LoadFromKv(platform::CouchbaseClient& client,
                                                                    std::string_view origin,
                                                                    std::string_view destination);

  struct MarketEntry {
    std::shared_ptr<const std::vector<domain::PriceRecord>> records;
    std::uint64_t bytes{0};
  };

  mutable std::mutex mu_;
  platform::DataSnapshotDocument snapshot_{};
  bool pinned_{false};
  std::uint64_t approx_bytes_{0};
  // key = origin::destination
  std::unordered_map<std::string, MarketEntry> by_market_;
};

}  // namespace reservations::cache
