#pragma once

#include "common/clock.hpp"
#include "common/result.hpp"
#include "domain/messages.hpp"
#include "platform/couchbase_client.hpp"
#include "schedule/catalog.hpp"

namespace reservations::availability {

class ClassAvailabilityService {
 public:
  ClassAvailabilityService(platform::CouchbaseClient& client, const Clock& clock);

  void SetCatalogMeta(schedule::CatalogMeta meta) { catalog_meta_ = std::move(meta); }

  [[nodiscard]] Result<domain::ClassAvailabilityResponse> Check(
      const domain::ClassAvailabilityRequest& request);

 private:
  platform::CouchbaseClient& client_;
  const Clock& clock_;
  schedule::CatalogMeta catalog_meta_{};
};

}  // namespace reservations::availability
