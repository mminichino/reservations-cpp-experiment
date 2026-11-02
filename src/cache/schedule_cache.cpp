#include "cache/schedule_cache.hpp"

#include <utility>

#include <rfl/json.hpp>

#include "platform/document_store.hpp"
#include "schedule/catalog.hpp"
#include "seatmap/service.hpp"

namespace reservations::cache {
namespace {

std::uint64_t OdPayloadBytes(const std::string& key, const std::vector<std::string>& ids) {
  std::uint64_t n = key.size();
  for (const auto& id : ids) {
    n += id.size();
  }
  return n;
}

}  // namespace

void ScheduleCache::PinSnapshot(platform::DataSnapshotDocument snapshot) {
  std::lock_guard lock(mu_);
  snapshot_ = std::move(snapshot);
  pinned_ = true;
}

platform::DataSnapshotDocument ScheduleCache::CurrentSnapshot() const {
  std::lock_guard lock(mu_);
  return snapshot_;
}

std::uint64_t ScheduleCache::ApproxBytes() const {
  std::lock_guard lock(mu_);
  return approx_bytes_;
}

Result<std::shared_ptr<const std::vector<std::string>>> ScheduleCache::GetDirectFlights(
    platform::CouchbaseClient& client, std::string_view origin, std::string_view destination,
    std::string_view departure_date, std::uint64_t reset_epoch) {
  const std::string key = schedule::OdDateIndexKey(origin, destination, departure_date);

  {
    std::lock_guard lock(mu_);
    if (!pinned_) {
      return Result<std::shared_ptr<const std::vector<std::string>>>::Failure(
          Error::Make(ErrorCode::ValidationFailed, "schedule snapshot not pinned"));
    }
    auto it = od_.find(key);
    if (it != od_.end() && it->second.value) {
      return Result<std::shared_ptr<const std::vector<std::string>>>::Success(it->second.value);
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
  const std::uint64_t payload = OdPayloadBytes(key, *ptr);
  {
    std::lock_guard lock(mu_);
    auto existing = od_.find(key);
    if (existing != od_.end()) {
      approx_bytes_ -= existing->second.bytes;
    }
    od_[key] = Entry<std::vector<std::string>>{.value = ptr, .bytes = payload};
    approx_bytes_ += payload;
  }
  return Result<std::shared_ptr<const std::vector<std::string>>>::Success(std::move(ptr));
}

Result<std::shared_ptr<const domain::Flight>> ScheduleCache::GetFlight(platform::CouchbaseClient& client,
                                                                       std::string_view flight_id) {
  const std::string id(flight_id);

  {
    std::lock_guard lock(mu_);
    if (!pinned_) {
      return Result<std::shared_ptr<const domain::Flight>>::Failure(
          Error::Make(ErrorCode::ValidationFailed, "schedule snapshot not pinned"));
    }
    auto it = flights_.find(id);
    if (it != flights_.end() && it->second.value) {
      return Result<std::shared_ptr<const domain::Flight>>::Success(it->second.value);
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
  const std::uint64_t payload = rfl::json::write(*ptr).size() + id.size();
  {
    std::lock_guard lock(mu_);
    auto existing = flights_.find(id);
    if (existing != flights_.end()) {
      approx_bytes_ -= existing->second.bytes;
    }
    flights_[id] = Entry<domain::Flight>{.value = ptr, .bytes = payload};
    approx_bytes_ += payload;
  }
  return Result<std::shared_ptr<const domain::Flight>>::Success(std::move(ptr));
}

}  // namespace reservations::cache
