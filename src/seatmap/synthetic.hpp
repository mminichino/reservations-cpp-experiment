#pragma once

#include <cstdint>
#include <string>

#include "domain/types.hpp"
#include "schedule/catalog.hpp"

namespace reservations::seatmap {

struct SyntheticFlightBundle {
  domain::Flight flight;
  domain::FlightIndexDocument index;
  domain::SeatMapDocument seatmap;
  domain::InventoryDocument inventory;
};

/// Shared cabin/seat template (physical layout identical across the catalog).
[[nodiscard]] domain::SeatMapDocument BuildSeatMapTemplate(const std::string& flight_id,
                                                           std::uint64_t reset_epoch);

[[nodiscard]] int TemplatePhysicalCapacity();

/// Deterministic flight bundle for a catalog slot at the given reset epoch.
[[nodiscard]] SyntheticFlightBundle BuildCatalogFlight(const schedule::FlightRef& ref,
                                                       std::uint64_t reset_epoch);

}  // namespace reservations::seatmap
