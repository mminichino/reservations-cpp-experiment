#include "cache/inventory_cache.hpp"

#include <chrono>
#include <utility>

#include <rfl/json.hpp>

#include "platform/document_store.hpp"
#include "seatmap/service.hpp"

namespace reservations::cache {

Result<InventoryCache::Entry> InventoryCache::LoadFromKv(platform::CouchbaseClient& client,
                                                         std::string_view flight_id,
                                                         std::uint64_t reset_epoch) {
  platform::DocumentStore<domain::InventoryDocument> store(client, seatmap::kInventoryCollection);
  auto loaded = store.Get(seatmap::InventoryKey(std::string(flight_id)));
  if (!loaded) {
    return Result<Entry>::Failure(loaded.error());
  }
  if (!loaded.value().has_value() || loaded.value()->document.resetEpoch < reset_epoch) {
    return Result<Entry>::Failure(
        Error::Make(ErrorCode::NotFound, "Inventory missing for " + std::string(flight_id)));
  }
  Entry entry{.document = std::move(loaded.value()->document),
              .cas = loaded.value()->cas,
              .resetEpoch = loaded.value()->document.resetEpoch,
              .bytes = 0};
  entry.bytes = rfl::json::write(entry.document).size() + std::string(flight_id).size();
  return Result<Entry>::Success(std::move(entry));
}

std::uint64_t InventoryCache::ApproxBytes() const {
  std::lock_guard lock(mu_);
  return approx_bytes_;
}

Result<InventoryCache::Entry> InventoryCache::Get(platform::CouchbaseClient& client,
                                                  std::string_view flight_id,
                                                  std::uint64_t reset_epoch) {
  const bool fence_clear = fence_.WaitUntilClear(flight_id, std::chrono::milliseconds{100});

  if (fence_clear) {
    std::lock_guard lock(mu_);
    auto it = entries_.find(std::string(flight_id));
    if (it != entries_.end() && it->second.resetEpoch >= reset_epoch) {
      return Result<Entry>::Success(it->second);
    }
  }

  auto loaded = LoadFromKv(client, flight_id, reset_epoch);
  if (!loaded) {
    return loaded;
  }
  {
    std::lock_guard lock(mu_);
    const std::string key(flight_id);
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
      approx_bytes_ -= existing->second.bytes;
    }
    approx_bytes_ += loaded.value().bytes;
    entries_[key] = loaded.value();
  }
  return loaded;
}

Result<InventoryCache::Entry> InventoryCache::GetForUpdate(platform::CouchbaseClient& client,
                                                           std::string_view flight_id,
                                                           std::uint64_t reset_epoch) {
  auto loaded = LoadFromKv(client, flight_id, reset_epoch);
  if (!loaded) {
    return loaded;
  }
  {
    std::lock_guard lock(mu_);
    const std::string key(flight_id);
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
      approx_bytes_ -= existing->second.bytes;
    }
    approx_bytes_ += loaded.value().bytes;
    entries_[key] = loaded.value();
  }
  return loaded;
}

void InventoryCache::Put(std::string_view flight_id, domain::InventoryDocument document, std::uint64_t cas,
                         std::uint64_t reset_epoch) {
  Entry entry{.document = std::move(document),
              .cas = cas,
              .resetEpoch = reset_epoch,
              .bytes = 0};
  entry.bytes = rfl::json::write(entry.document).size() + std::string(flight_id).size();
  std::lock_guard lock(mu_);
  const std::string key(flight_id);
  auto existing = entries_.find(key);
  if (existing != entries_.end()) {
    approx_bytes_ -= existing->second.bytes;
  }
  approx_bytes_ += entry.bytes;
  entries_[key] = std::move(entry);
}

void InventoryCache::Invalidate(std::string_view flight_id) {
  std::lock_guard lock(mu_);
  auto it = entries_.find(std::string(flight_id));
  if (it == entries_.end()) {
    return;
  }
  approx_bytes_ -= it->second.bytes;
  entries_.erase(it);
}

}  // namespace reservations::cache
