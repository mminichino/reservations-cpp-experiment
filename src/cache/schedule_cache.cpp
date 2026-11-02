#include "cache/schedule_cache.hpp"

#include <utility>

#include "platform/document_store.hpp"
#include "schedule/catalog.hpp"
#include "seatmap/service.hpp"

namespace reservations::cache {

platform::DataSnapshotDocument ScheduleCache::CurrentSnapshot() const {
  std::lock_guard lock(mu_);
  return snapshot_;
}

Result<platform::DataSnapshotDocument> ScheduleCache::FetchSnapshot(platform::CouchbaseClient& client) {
  platform::DocumentStore<platform::DataSnapshotDocument> store(client, schedule::kMetaCollection);
  auto loaded = store.Get(platform::kScheduleSnapshotKey);
  if (!loaded) {
    return Result<platform::DataSnapshotDocument>::Failure(loaded.error());
  }
  if (!loaded.value().has_value()) {
    return Result<platform::DataSnapshotDocument>::Failure(
        Error::Make(ErrorCode::NotFound, "schedule snapshot missing — run load"));
  }
  return Result<platform::DataSnapshotDocument>::Success(std::move(loaded.value()->document));
}

Result<std::shared_ptr<const std::vector<std::string>>> ScheduleCache::GetDirectFlights(
    platform::CouchbaseClient& client, std::string_view origin, std::string_view destination,
    std::string_view departure_date, std::uint64_t reset_epoch) {
  auto snap = FetchSnapshot(client);
  if (!snap) {
    return Result<std::shared_ptr<const std::vector<std::string>>>::Failure(snap.error());
  }
  const std::string key = schedule::OdDateIndexKey(origin, destination, departure_date);
  const std::string snap_id = snap.value().snapshotId;

  {
    std::lock_guard lock(mu_);
    snapshot_ = snap.value();
    auto it = od_.find(key);
    if (it != od_.end() && it->second.first == snap_id && it->second.second) {
      return Result<std::shared_ptr<const std::vector<std::string>>>::Success(it->second.second);
    }
  }

  platform::DocumentStore<domain::OdFlightIndexDocument> store(client, schedule::kOdIndexCollection);
  auto loaded = store.Get(key);
  if (!loaded) {
    return Result<std::shared_ptr<const std::vector<std::string>>>::Failure(loaded.error());
  }
  std::vector<std::string> ids;
  if (loaded.value().has_value() && loaded.value()->document.resetEpoch >= reset_epoch) {
    ids = loaded.value()->document.flightIds;
  }
  auto ptr = std::make_shared<const std::vector<std::string>>(std::move(ids));
  {
    std::lock_guard lock(mu_);
    snapshot_ = snap.value();
    for (auto it = od_.begin(); it != od_.end();) {
      if (it->second.first != snap_id) {
        it = od_.erase(it);
      } else {
        ++it;
      }
    }
    od_[key] = {snap_id, ptr};
  }
  return Result<std::shared_ptr<const std::vector<std::string>>>::Success(std::move(ptr));
}

Result<std::shared_ptr<const domain::Flight>> ScheduleCache::GetFlight(platform::CouchbaseClient& client,
                                                                       std::string_view flight_id) {
  auto snap = FetchSnapshot(client);
  if (!snap) {
    return Result<std::shared_ptr<const domain::Flight>>::Failure(snap.error());
  }
  const std::string id(flight_id);
  const std::string snap_id = snap.value().snapshotId;

  {
    std::lock_guard lock(mu_);
    snapshot_ = snap.value();
    auto it = flights_.find(id);
    if (it != flights_.end() && it->second.first == snap_id && it->second.second) {
      return Result<std::shared_ptr<const domain::Flight>>::Success(it->second.second);
    }
  }

  platform::DocumentStore<domain::Flight> store(client, seatmap::kFlightsCollection);
  auto loaded = store.Get(seatmap::FlightKey(id));
  if (!loaded) {
    return Result<std::shared_ptr<const domain::Flight>>::Failure(loaded.error());
  }
  if (!loaded.value().has_value()) {
    return Result<std::shared_ptr<const domain::Flight>>::Failure(
        Error::Make(ErrorCode::FlightNotFound, "Flight not found"));
  }
  auto ptr = std::make_shared<const domain::Flight>(std::move(loaded.value()->document));
  {
    std::lock_guard lock(mu_);
    snapshot_ = snap.value();
    for (auto it = flights_.begin(); it != flights_.end();) {
      if (it->second.first != snap_id) {
        it = flights_.erase(it);
      } else {
        ++it;
      }
    }
    flights_[id] = {snap_id, ptr};
  }
  return Result<std::shared_ptr<const domain::Flight>>::Success(std::move(ptr));
}

}  // namespace reservations::cache
