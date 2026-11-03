#include "cache/inventory_cache.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

#include <rfl/json.hpp>

#include "platform/document_store.hpp"
#include "seatmap/service.hpp"

namespace reservations::cache {

Result<std::shared_ptr<const InventoryCache::Entry>> InventoryCache::LoadFromKv(
    platform::CouchbaseClient& client, std::string_view flight_id, std::uint64_t reset_epoch) {
  platform::DocumentStore<domain::InventoryDocument> store(client, seatmap::kInventoryCollection);
  auto loaded = store.Get(seatmap::InventoryKey(std::string(flight_id)));
  if (!loaded) {
    return Result<std::shared_ptr<const Entry>>::Failure(loaded.error());
  }
  if (!loaded.value().has_value() || loaded.value()->document.resetEpoch < reset_epoch) {
    return Result<std::shared_ptr<const Entry>>::Failure(
        Error::Make(ErrorCode::NotFound, "Inventory missing for " + std::string(flight_id)));
  }
  auto entry = std::make_shared<Entry>();
  entry->document = std::move(loaded.value()->document);
  entry->cas = loaded.value()->cas;
  entry->resetEpoch = entry->document.resetEpoch;
  entry->bytes = rfl::json::write(entry->document).size() + std::string(flight_id).size();
  return Result<std::shared_ptr<const Entry>>::Success(std::shared_ptr<const Entry>(std::move(entry)));
}

void InventoryCache::StoreEntry(std::string_view flight_id, std::shared_ptr<const Entry> entry) {
  std::lock_guard lock(mu_);
  const std::string key(flight_id);
  auto existing = entries_.find(key);
  if (existing != entries_.end()) {
    approx_bytes_ -= existing->second->bytes;
  }
  approx_bytes_ += entry->bytes;
  entries_[key] = std::move(entry);
}

std::uint64_t InventoryCache::ApproxBytes() const {
  std::lock_guard lock(mu_);
  return approx_bytes_;
}

Result<std::shared_ptr<const InventoryCache::Entry>> InventoryCache::Get(
    platform::CouchbaseClient& client, std::string_view flight_id, std::uint64_t reset_epoch) {
  const std::string key = seatmap::InventoryKey(std::string(flight_id));

  std::shared_ptr<const Entry> cached;
  {
    std::lock_guard lock(mu_);
    auto it = entries_.find(std::string(flight_id));
    if (it != entries_.end() && it->second->resetEpoch >= reset_epoch) {
      cached = it->second;
    }
  }

  if (cached) {
    auto exists = client.Exists(seatmap::kInventoryCollection, key);
    if (!exists) {
      return Result<std::shared_ptr<const Entry>>::Failure(exists.error());
    }
    if (exists.value().found && exists.value().cas == cached->cas) {
      return Result<std::shared_ptr<const Entry>>::Success(std::move(cached));
    }
    // Missing or newer CAS → full load below.
  }

  auto loaded = LoadFromKv(client, flight_id, reset_epoch);
  if (!loaded) {
    return loaded;
  }
  StoreEntry(flight_id, loaded.value());
  return loaded;
}

Result<void> InventoryCache::EnsureWorkers(const CouchbaseConfig& config, int parallelism) {
  std::lock_guard lock(pool_mu_);
  const int n = std::max({parallelism, kDefaultGetManyParallelism, 16});
  if (!workers_) {
    workers_ = std::make_unique<platform::KvWorkerPool>(config, n);
  }
  return workers_->EnsureStarted();
}

Result<std::unordered_map<std::string, std::shared_ptr<const InventoryCache::Entry>>>
InventoryCache::GetMany(const CouchbaseConfig& config, const std::vector<std::string>& flight_ids,
                        std::uint64_t reset_epoch, int parallelism) {
  std::set<std::string> unique;
  for (const auto& id : flight_ids) {
    if (!id.empty()) {
      unique.insert(id);
    }
  }
  std::vector<std::string> ids(unique.begin(), unique.end());
  std::unordered_map<std::string, std::shared_ptr<const Entry>> out;
  if (ids.empty()) {
    return Result<std::unordered_map<std::string, std::shared_ptr<const Entry>>>::Success(std::move(out));
  }

  if (auto ready = EnsureWorkers(config, parallelism); !ready) {
    return Result<std::unordered_map<std::string, std::shared_ptr<const Entry>>>::Failure(ready.error());
  }

  std::mutex out_mu;
  auto ran = workers_->ParallelFor(ids.size(), [&](platform::CouchbaseClient& client, std::size_t i) {
    auto loaded = Get(client, ids[i], reset_epoch);
    if (!loaded) {
      return Result<void>::Failure(loaded.error());
    }
    std::lock_guard lock(out_mu);
    out.emplace(ids[i], std::move(loaded.value()));
    return Result<void>::Success();
  });
  if (!ran) {
    return Result<std::unordered_map<std::string, std::shared_ptr<const Entry>>>::Failure(ran.error());
  }
  return Result<std::unordered_map<std::string, std::shared_ptr<const Entry>>>::Success(std::move(out));
}

void InventoryCache::Put(std::string_view flight_id, domain::InventoryDocument document, std::uint64_t cas,
                         std::uint64_t reset_epoch) {
  auto entry = std::make_shared<Entry>();
  entry->document = std::move(document);
  entry->cas = cas;
  entry->resetEpoch = reset_epoch;
  entry->bytes = rfl::json::write(entry->document).size() + std::string(flight_id).size();
  StoreEntry(flight_id, std::shared_ptr<const Entry>(std::move(entry)));
}

void InventoryCache::Invalidate(std::string_view flight_id) {
  std::lock_guard lock(mu_);
  auto it = entries_.find(std::string(flight_id));
  if (it == entries_.end()) {
    return;
  }
  approx_bytes_ -= it->second->bytes;
  entries_.erase(it);
}

}  // namespace reservations::cache
