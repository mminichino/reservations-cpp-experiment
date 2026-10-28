#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "common/clock.hpp"
#include "common/result.hpp"
#include "common/uuid.hpp"
#include "domain/messages.hpp"
#include "platform/couchbase_client.hpp"
#include "platform/document_store.hpp"
#include "schedule/catalog.hpp"

namespace reservations::seatmap {

inline constexpr const char* kFlightsCollection = "flights";
inline constexpr const char* kSeatmapsCollection = "seatmaps";
inline constexpr const char* kInventoryCollection = "inventory";
inline constexpr const char* kHoldsCollection = "holds";
inline constexpr const char* kPnrsCollection = "pnrs";
inline constexpr const char* kIdempotencyCollection = "idempotency";
inline constexpr const char* kFlightIndexCollection = "flight_index";

inline std::vector<std::string> AllCollections() {
  return {kFlightsCollection,     kSeatmapsCollection,      kInventoryCollection, kHoldsCollection,
          kPnrsCollection,        kIdempotencyCollection,   kFlightIndexCollection,
          schedule::kMetaCollection};
}

/// Booking (class inventory) and seat-map workflows backed by Couchbase via reflect-cpp.
/// Thread-safety: externally synchronized through CouchbaseClient.
class ReservationService {
 public:
  ReservationService(platform::CouchbaseClient& client, const Clock& clock);

  void SetCatalogMeta(schedule::CatalogMeta meta) { catalog_meta_ = std::move(meta); }
  [[nodiscard]] const schedule::CatalogMeta& catalog_meta() const { return catalog_meta_; }

  [[nodiscard]] Result<std::vector<domain::BookableFlight>> ListBookableFlights(
      const std::vector<std::string>& flight_ids);

  [[nodiscard]] Result<domain::FlightBookingResponse> BookFlight(const domain::FlightBookingRequest& request);

  /// Seat map view. Empty seats is success (full / oversold / nothing selectable).
  [[nodiscard]] Result<domain::SeatAvailabilityResponse> CheckAvailability(
      const domain::SeatAvailabilityRequest& request);

  [[nodiscard]] Result<domain::SeatHoldResponse> HoldSeats(const domain::SeatHoldRequest& request);

  [[nodiscard]] Result<domain::SeatPurchaseResponse> PurchaseSeats(const domain::SeatPurchaseRequest& request);

 private:
  [[nodiscard]] Result<domain::Flight> ResolveFlight(
      const std::variant<domain::FlightById, domain::FlightBySchedule>& flight);
  [[nodiscard]] Result<domain::Flight> FlightFromId(const std::string& flight_id);
  [[nodiscard]] Result<platform::DocumentStore<domain::SeatMapDocument>::Loaded> LoadSeatMap(
      const std::string& flight_id);
  [[nodiscard]] Result<platform::DocumentStore<domain::InventoryDocument>::Loaded> LoadInventory(
      const std::string& flight_id);
  [[nodiscard]] std::vector<domain::CabinSummary> BuildSummary(const domain::SeatMapDocument& map) const;
  [[nodiscard]] bool SeatMatchesFilters(const domain::Seat& seat,
                                        const std::optional<domain::AvailabilityFilters>& filters) const;

  platform::CouchbaseClient& client_;
  const Clock& clock_;
  schedule::CatalogMeta catalog_meta_{};
  UuidGenerator uuid_;
  RecordLocatorGenerator locators_;

  platform::DocumentStore<domain::Flight> flights_;
  platform::DocumentStore<domain::SeatMapDocument> seatmaps_;
  platform::DocumentStore<domain::InventoryDocument> inventory_;
  platform::DocumentStore<domain::HoldDocument> holds_;
  platform::DocumentStore<domain::PnrDocument> pnrs_;
  platform::DocumentStore<domain::IdempotencyDocument> idempotency_;
  platform::DocumentStore<domain::FlightIndexDocument> flight_index_;
};

}  // namespace reservations::seatmap
