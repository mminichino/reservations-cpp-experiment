#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/result.hpp"
#include "platform/couchbase_client.hpp"

namespace reservations::cache {

inline constexpr const char* kInventoryGenerationCollection = "inventory";

[[nodiscard]] inline std::string InventoryGenerationKey(std::string_view flight_id) {
  return "inv_gen::" + std::string(flight_id);
}

/// Tiny sidecar: shop compares `cas` to the cached inventory CAS without loading the full doc.
struct InventoryGenerationDocument {
  std::string type{"inventory_generation"};
  std::string flightId;
  std::uint64_t cas{0};
};

[[nodiscard]] Result<void> PublishInventoryGeneration(platform::CouchbaseClient& client,
                                                      std::string_view flight_id, std::uint64_t cas);

/// Returns nullopt if the sidecar is missing (caller should treat as cache miss).
[[nodiscard]] Result<std::optional<std::uint64_t>> FetchInventoryGeneration(
    platform::CouchbaseClient& client, std::string_view flight_id);

}  // namespace reservations::cache
