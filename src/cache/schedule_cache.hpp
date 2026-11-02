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

/// Process-local OD-index and flight cache keyed by schedule snapshotId.
/// Currency checked via tiny Get of `snapshot::schedule`.
class ScheduleCache {
 public:
  [[nodiscard]] Result<std::shared_ptr<const std::vector<std::string>>> GetDirectFlights(
      platform::CouchbaseClient& client, std::string_view origin, std::string_view destination,
      std::string_view departure_date, std::uint64_t reset_epoch);

  [[nodiscard]] Result<std::shared_ptr<const domain::Flight>> GetFlight(platform::CouchbaseClient& client,
                                                                        std::string_view flight_id);

  [[nodiscard]] platform::DataSnapshotDocument CurrentSnapshot() const;

  [[nodiscard]] std::uint64_t ApproxBytes() const;

 private:
  [[nodiscard]] Result<platform::DataSnapshotDocument> FetchSnapshot(platform::CouchbaseClient& client);

  template <typename T>
  struct Entry {
    std::string snapshot_id;
    std::shared_ptr<const T> value;
    std::uint64_t bytes{0};
  };

  mutable std::mutex mu_;
  platform::DataSnapshotDocument snapshot_{};
  std::uint64_t approx_bytes_{0};
  std::unordered_map<std::string, Entry<std::vector<std::string>>> od_;
  std::unordered_map<std::string, Entry<domain::Flight>> flights_;
};

}  // namespace reservations::cache
