#pragma once

#include <memory>

#include "cache/client_caches.hpp"
#include "common/clock.hpp"
#include "common/result.hpp"
#include "domain/messages.hpp"
#include "platform/couchbase_client.hpp"
#include "schedule/catalog.hpp"

namespace reservations::availability {

class ClassAvailabilityService {
 public:
  /// Pass nullptr caches for pure KV (reflect-cpp DocumentStore) path; non-null enables local caches.
  ClassAvailabilityService(platform::CouchbaseClient& client, const Clock& clock,
                           std::shared_ptr<cache::ClientCaches> caches = nullptr);

  void SetCatalogMeta(schedule::CatalogMeta meta) { catalog_meta_ = std::move(meta); }

  [[nodiscard]] Result<domain::ClassAvailabilityResponse> Check(
      const domain::ClassAvailabilityRequest& request);

 private:
  platform::CouchbaseClient& client_;
  const Clock& clock_;
  schedule::CatalogMeta catalog_meta_{};
  std::shared_ptr<cache::ClientCaches> caches_;  // nullptr => no cache
};

}  // namespace reservations::availability
