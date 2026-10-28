#include "seatmap/seed.hpp"

#include "platform/document_store.hpp"
#include "seatmap/service.hpp"
#include "seatmap/synthetic.hpp"

namespace reservations::seatmap {

Result<schedule::CatalogMeta> LoadCatalogMeta(platform::CouchbaseClient& client) {
  platform::DocumentStore<schedule::CatalogMeta> meta(client, schedule::kMetaCollection);
  auto loaded = meta.Get(schedule::kCatalogMetaKey);
  if (!loaded) {
    return Result<schedule::CatalogMeta>::Failure(loaded.error());
  }
  if (!loaded.value().has_value()) {
    return Result<schedule::CatalogMeta>::Failure(
        Error::Make(ErrorCode::NotFound, "Catalog meta missing — run load first"));
  }
  return Result<schedule::CatalogMeta>::Success(loaded.value()->document);
}

Result<schedule::CatalogMeta> ResetCatalog(platform::CouchbaseClient& client) {
  auto topo = client.EnsureTopology(AllCollections());
  if (!topo) {
    return Result<schedule::CatalogMeta>::Failure(topo.error());
  }

  platform::DocumentStore<schedule::CatalogMeta> meta_store(client, schedule::kMetaCollection);
  schedule::CatalogMeta meta;
  auto existing = meta_store.Get(schedule::kCatalogMetaKey);
  if (!existing) {
    return Result<schedule::CatalogMeta>::Failure(existing.error());
  }
  if (existing.value().has_value()) {
    meta = existing.value()->document;
    ++meta.resetEpoch;
  } else {
    meta.resetEpoch = 1;
  }
  meta.type = "catalog_meta";
  meta.airlines = schedule::kAirlineCount;
  meta.flightsPerWeekPerAirline = schedule::kFlightsPerWeekPerAirline;
  meta.weeksOut = schedule::kWeeksOut;
  meta.oversellPercent = domain::kDefaultOversellPercent;
  meta.physicalCapacityPerFlight = TemplatePhysicalCapacity();
  meta.authorizationPerFlight =
      domain::AuthorizationFor(meta.physicalCapacityPerFlight, meta.oversellPercent);

  auto upserted = meta_store.Upsert(schedule::kCatalogMetaKey, meta);
  if (!upserted) {
    return Result<schedule::CatalogMeta>::Failure(upserted.error());
  }
  return Result<schedule::CatalogMeta>::Success(meta);
}

}  // namespace reservations::seatmap
