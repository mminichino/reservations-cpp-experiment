#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rfl.hpp>
#include <rfl/json.hpp>

namespace reservations::domain {

enum class CabinClass { economy, premium_economy, business, first };

enum class SeatStatus { available, held, occupied, blocked };

enum class SeatPosition { window, middle, aisle };

enum class FlightStatus { scheduled, boarding, departed, arrived, delayed, cancelled };

enum class PassengerType { adult, child, infant_in_lap, infant_in_seat };

enum class HoldState : std::uint8_t { Active, Consumed, Expired, Released };

[[nodiscard]] constexpr bool CanTransition(HoldState from, HoldState to) noexcept {
  switch (from) {
    case HoldState::Active:
      return to == HoldState::Consumed || to == HoldState::Expired || to == HoldState::Released;
    case HoldState::Consumed:
    case HoldState::Expired:
    case HoldState::Released:
      return false;
  }
  return false;
}

struct Money {
  std::int64_t amountMinor{0};
  std::string currency{"USD"};
};

struct Aircraft {
  std::string icaoType;
  std::optional<std::string> name;
  std::string configurationId;
  std::optional<std::string> registration;
};

struct Flight {
  std::string flightId;
  std::string airlineCode;
  std::string flightNumber;
  std::string origin;
  std::string destination;
  std::string scheduledDeparture;
  std::string scheduledArrival;
  std::optional<std::string> terminal;
  std::optional<std::string> gate;
  std::optional<Aircraft> aircraft;
  FlightStatus status{FlightStatus::scheduled};
  std::optional<bool> seatSelectionOpen{true};
};

struct SeatAttributes {
  SeatPosition position{SeatPosition::window};
  std::optional<bool> exitRow;
  std::optional<bool> extraLegroom;
  std::optional<bool> bulkhead;
  std::optional<bool> nearLavatory;
  std::optional<bool> nearGalley;
  std::optional<bool> limitedRecline;
  std::optional<bool> powerOutlet;
  std::optional<bool> usb;
  std::optional<bool> personalScreen;
  std::optional<bool> wheelchairAccessible;
  std::optional<bool> bassinetPosition;
  std::optional<std::vector<std::string>> adjacentSeatNumbers;
};

struct SeatRestrictions {
  std::optional<int> minAge;
  std::optional<bool> noInfants;
  std::optional<bool> noChildrenUnaccompanied;
  std::optional<bool> requiresAbleBodied;
  std::optional<std::vector<std::string>> eliteTiersOnly;
};

struct Seat {
  std::string seatNumber;
  int row{0};
  std::string column;
  CabinClass cabin{CabinClass::economy};
  SeatStatus status{SeatStatus::available};
  SeatAttributes attributes{};
  std::optional<SeatRestrictions> restrictions;
  std::optional<Money> price;
  std::optional<std::string> priceValidUntil;
  std::optional<std::string> heldUntil;
  std::optional<std::string> holdId;
};

struct CabinLayout {
  CabinClass cabin{CabinClass::economy};
  int firstRow{0};
  int lastRow{0};
  std::vector<std::string> columnLayout;
  std::optional<std::vector<int>> exitRows;
  std::optional<std::vector<int>> missingRows;
};

struct CabinSummary {
  CabinClass cabin{CabinClass::economy};
  int totalSeats{0};
  int available{0};
  int held{0};
  int occupied{0};
  int blocked{0};
};

/// Persisted seat-map aggregate for a flight-date. Atomic CAS unit for seat assign/hold.
struct SeatMapDocument {
  std::string type{"seatmap"};
  std::string flightId;
  std::string seatMapVersion;
  std::vector<CabinLayout> layout;
  std::vector<Seat> seats;
  std::string updatedAt;
  std::uint64_t resetEpoch{0};
};

/// Class / booking inventory (separate from physical seat map). AU may exceed capacity.
struct InventoryDocument {
  std::string type{"inventory"};
  std::string flightId;
  int physicalCapacity{0};
  int authorization{0};
  int sold{0};
  double oversellPercent{0.08};
  std::uint64_t resetEpoch{0};
};

/// Industry-typical fixed overbooking rate used when seeding authorization levels.
inline constexpr double kDefaultOversellPercent = 0.08;

[[nodiscard]] inline int AuthorizationFor(int physical_capacity, double oversell_percent) {
  if (physical_capacity <= 0) {
    return 0;
  }
  const auto au = static_cast<int>(static_cast<double>(physical_capacity) * (1.0 + oversell_percent) + 0.999999);
  return std::max(physical_capacity, au);
}

struct FlightIndexDocument {
  std::string type{"flight_index"};
  std::string flightId;
  std::string airlineCode;
  std::string flightNumber;
  std::string departureDate;
};

struct HoldDocument {
  std::string type{"hold"};
  std::string holdId;
  std::string flightId;
  std::vector<std::string> seats;
  std::vector<Money> prices;
  Money totalPrice{};
  std::string expiresAt;
  HoldState state{HoldState::Active};
  std::string createdAt;
};

struct Address {
  std::string line1;
  std::optional<std::string> line2;
  std::string city;
  std::optional<std::string> region;
  std::string postalCode;
  std::string country;
};

struct Passenger {
  std::string passengerId;
  PassengerType type{PassengerType::adult};
  std::optional<std::string> title;
  std::string firstName;
  std::optional<std::string> middleName;
  std::string lastName;
  std::string dateOfBirth;
  std::optional<std::string> associatedAdultPassengerId;
};

struct ContactInfo {
  std::string email;
  std::string phone;
  std::optional<std::string> preferredLanguage;
};

struct SeatSelection {
  std::string passengerId;
  std::string seatNumber;
  Money expectedPrice{};
};

struct ChargeLine {
  std::string type;
  std::string description;
  std::optional<std::string> passengerId;
  std::optional<std::string> seatNumber;
  Money amount{};
};

struct SeatAssignment {
  std::string passengerId;
  std::string seatNumber;
  CabinClass cabin{CabinClass::economy};
  std::optional<std::string> ticketNumber;
  Money price{};
};

struct PnrDocument {
  std::string type{"pnr"};
  std::string bookingReference;
  std::string confirmationNumber;
  std::string flightId;
  std::vector<Passenger> passengers;
  std::vector<SeatAssignment> seatAssignments;
  std::vector<ChargeLine> charges;
  Money totalCharged{};
  ContactInfo contact{};
  std::string purchasedAt;
  std::uint64_t version{1};
  bool seatAssignmentPending{true};
};

struct IdempotencyDocument {
  std::string type{"idempotency"};
  std::string idempotencyKey;
  std::string requestHash;
  std::string responseJson;
  std::string createdAt;
};

}  // namespace reservations::domain
