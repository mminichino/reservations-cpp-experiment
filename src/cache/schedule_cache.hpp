#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/result.hpp"
#include "domain/pricing.hpp"
#include "domain/types.hpp"
#include "platform/couchbase_client.hpp"
#include "platform/snapshot.hpp"

namespace reservations::cache {

/// Process-local OD-index and flight cache.
/// Snapshot id is pinned once (experiment treats schedule data as static) — no per-call KV currency check.
class ScheduleCache {
 public:
  void PinSnapshot(platform::DataSnapshotDocument snapshot);

  [[nodiscard]] Result<std::shared_ptr<const std::vector<std::string>>> GetDirectFlights(
      platform::CouchbaseClient& client, std::string_view origin, std::string_view destination,
      std::string_view departure_date, std::uint64_t reset_epoch);

  [[nodiscard]] Result<std::shared_ptr<const domain::Flight>> GetFlight(platform::CouchbaseClient& client,
                                                                        std::string_view flight_id);

  [[nodiscard]] platform::DataSnapshotDocument CurrentSnapshot() const;

  [[nodiscard]] std::uint64_t ApproxBytes() const;

 private:
  template <typename T>
  struct Entry {
    std::shared_ptr<const T> value;
    std::uint64_t bytes{0};
  };

  mutable std::mutex mu_;
  platform::DataSnapshotDocument snapshot_{};
  bool pinned_{false};
  std::uint64_t approx_bytes_{0};
  std::unordered_map<std::string, Entry<std::vector<std::string>>> od_;
  std::unordered_map<std::string, Entry<domain::Flight>> flights_;
};

}  // namespace reservations::cache
