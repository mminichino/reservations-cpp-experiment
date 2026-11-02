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

 private:
  [[nodiscard]] Result<platform::DataSnapshotDocument> FetchSnapshot(platform::CouchbaseClient& client);

  mutable std::mutex mu_;
  platform::DataSnapshotDocument snapshot_{};
  std::unordered_map<std::string, std::pair<std::string, std::shared_ptr<const std::vector<std::string>>>> od_;
  std::unordered_map<std::string, std::pair<std::string, std::shared_ptr<const domain::Flight>>> flights_;
};

}  // namespace reservations::cache
