#include "cache/inventory_generation.hpp"

#include "platform/document_store.hpp"

namespace reservations::cache {

Result<void> PublishInventoryGeneration(platform::CouchbaseClient& client, std::string_view flight_id,
                                        std::uint64_t cas) {
  platform::DocumentStore<InventoryGenerationDocument> store(client, kInventoryGenerationCollection);
  InventoryGenerationDocument doc;
  doc.flightId = std::string(flight_id);
  doc.cas = cas;
  auto upserted = store.Upsert(InventoryGenerationKey(flight_id), doc);
  if (!upserted) {
    return Result<void>::Failure(upserted.error());
  }
  return Result<void>::Success();
}

Result<std::optional<std::uint64_t>> FetchInventoryGeneration(platform::CouchbaseClient& client,
                                                              std::string_view flight_id) {
  platform::DocumentStore<InventoryGenerationDocument> store(client, kInventoryGenerationCollection);
  auto loaded = store.Get(InventoryGenerationKey(flight_id));
  if (!loaded) {
    return Result<std::optional<std::uint64_t>>::Failure(loaded.error());
  }
  if (!loaded.value().has_value()) {
    return Result<std::optional<std::uint64_t>>::Success(std::nullopt);
  }
  return Result<std::optional<std::uint64_t>>::Success(loaded.value()->document.cas);
}

}  // namespace reservations::cache
