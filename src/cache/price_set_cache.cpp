#include "cache/price_set_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "platform/document_store.hpp"
#include "schedule/catalog.hpp"

namespace reservations::cache {

platform::DataSnapshotDocument PriceSetCache::CurrentSnapshot() const {
  std::lock_guard lock(mu_);
  return snapshot_;
}

Result<platform::DataSnapshotDocument> PriceSetCache::FetchSnapshot(platform::CouchbaseClient& client) {
  platform::DocumentStore<platform::DataSnapshotDocument> store(client, schedule::kMetaCollection);
  auto loaded = store.Get(platform::kPriceSnapshotKey);
  if (!loaded) {
    return Result<platform::DataSnapshotDocument>::Failure(loaded.error());
  }
  if (!loaded.value().has_value()) {
    return Result<platform::DataSnapshotDocument>::Failure(
        Error::Make(ErrorCode::NotFound, "price snapshot missing — run load"));
  }
  return Result<platform::DataSnapshotDocument>::Success(std::move(loaded.value()->document));
}

Result<std::vector<domain::PriceRecord>> PriceSetCache::LoadFromKv(
    platform::CouchbaseClient& client, std::string_view origin, std::string_view destination,
    [[maybe_unused]] std::string_view snapshot_id) {
  platform::DocumentStore<domain::MarketMetaDocument> meta_store(client, schedule::kMarketsCollection);
  const std::string meta_key = "market_meta::" + schedule::MarketKey(origin, destination);
  auto meta = meta_store.Get(meta_key);
  std::vector<domain::PriceRecord> all;
  if (!meta) {
    return Result<std::vector<domain::PriceRecord>>::Failure(meta.error());
  }
  if (!meta.value().has_value()) {
    return Result<std::vector<domain::PriceRecord>>::Success(std::move(all));
  }
  const int pages = std::max(1, meta.value()->document.pageCount);
  platform::DocumentStore<domain::MarketPricePageDocument> page_store(client, schedule::kPricesCollection);
  for (int p = 1; p <= pages; ++p) {
    char key[128];
    std::snprintf(key, sizeof(key), "prices::%s::%s::p%04d", std::string(origin).c_str(),
                  std::string(destination).c_str(), p);
    auto page = page_store.Get(key);
    if (!page) {
      return Result<std::vector<domain::PriceRecord>>::Failure(page.error());
    }
    if (!page.value().has_value()) {
      continue;
    }
    auto& recs = page.value()->document.records;
    all.insert(all.end(), std::make_move_iterator(recs.begin()), std::make_move_iterator(recs.end()));
  }
  return Result<std::vector<domain::PriceRecord>>::Success(std::move(all));
}

Result<std::shared_ptr<const std::vector<domain::PriceRecord>>> PriceSetCache::GetMarketPrices(
    platform::CouchbaseClient& client, std::string_view origin, std::string_view destination) {
  auto snap = FetchSnapshot(client);
  if (!snap) {
    return Result<std::shared_ptr<const std::vector<domain::PriceRecord>>>::Failure(snap.error());
  }
  const std::string market = schedule::MarketKey(origin, destination);
  const std::string snap_id = snap.value().snapshotId;

  {
    std::lock_guard lock(mu_);
    snapshot_ = snap.value();
    auto it = by_market_.find(market);
    if (it != by_market_.end() && it->second.first == snap_id && it->second.second) {
      return Result<std::shared_ptr<const std::vector<domain::PriceRecord>>>::Success(it->second.second);
    }
  }

  auto loaded = LoadFromKv(client, origin, destination, snap_id);
  if (!loaded) {
    return Result<std::shared_ptr<const std::vector<domain::PriceRecord>>>::Failure(loaded.error());
  }
  auto ptr = std::make_shared<const std::vector<domain::PriceRecord>>(std::move(loaded.value()));
  {
    std::lock_guard lock(mu_);
    snapshot_ = snap.value();
    // Drop entries for other snapshots when the global price snapshot advances.
    for (auto it = by_market_.begin(); it != by_market_.end();) {
      if (it->second.first != snap_id) {
        it = by_market_.erase(it);
      } else {
        ++it;
      }
    }
    by_market_[market] = {snap_id, ptr};
  }
  return Result<std::shared_ptr<const std::vector<domain::PriceRecord>>>::Success(std::move(ptr));
}

}  // namespace reservations::cache
