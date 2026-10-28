#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "types.hpp"

namespace reservations::domain {

struct AvailabilityFilters {
  std::optional<std::vector<CabinClass>> cabins;
  std::optional<std::vector<SeatStatus>> statuses;
  std::optional<std::vector<SeatPosition>> positions;
  std::optional<bool> requireExitRow;
  std::optional<bool> requireExtraLegroom;
  std::optional<bool> requirePowerOutlet;
  std::optional<bool> requireWheelchairAccessible;
  std::optional<Money> maxPrice;
  std::optional<int> seatsTogether;
};

struct FlightById {
  std::string flightId;
};

struct FlightBySchedule {
  std::string airlineCode;
  std::string flightNumber;
  std::string departureDate;
};

struct ErrorBody {
  std::string code;
  std::string message;
  bool retryable{false};
};

struct FlightBookingRequest {
  std::string messageType{"FlightBookingRequest"};
  std::string requestId;
  std::string idempotencyKey;
  std::string flightId;
  std::vector<Passenger> passengers;
  ContactInfo contact{};
  std::optional<std::string> fareClass{"Y"};
};

struct FlightBookingResponse {
  std::string messageType{"FlightBookingResponse"};
  std::string requestId;
  std::string status;  // confirmed | failed
  std::optional<std::string> bookingReference;
  std::optional<std::string> confirmationNumber;
  std::optional<Flight> flight;
  std::optional<std::string> bookedAt;
  std::optional<ErrorBody> error;
  /// True when the booking is confirmed but no physical seat has been assigned yet.
  std::optional<bool> seatAssignmentPending;
};

struct BookableFlight {
  Flight flight;
  int physicalCapacity{0};
  int authorization{0};
  int sold{0};
  int remainingToSell{0};
};

struct SeatAvailabilityRequest {
  std::string messageType{"SeatAvailabilityRequest"};
  std::string requestId;
  std::variant<FlightById, FlightBySchedule> flight;
  std::optional<AvailabilityFilters> filters;
  std::optional<bool> includeLayout{true};
  std::optional<std::string> currency;
};

struct SeatAvailabilityResponse {
  std::string messageType{"SeatAvailabilityResponse"};
  std::string requestId;
  std::string generatedAt;
  Flight flight;
  std::vector<CabinSummary> summary;
  std::optional<std::vector<CabinLayout>> layout;
  std::vector<Seat> seats;
  std::string seatMapVersion;
};

struct SeatHoldRequest {
  std::string messageType{"SeatHoldRequest"};
  std::string requestId;
  std::string flightId;
  std::optional<std::string> seatMapVersion;
  std::vector<std::string> seats;
  std::optional<int> requestedHoldSeconds{600};
};

struct HeldSeat {
  std::string seatNumber;
  Money price{};
};

struct SeatHoldResponse {
  std::string messageType{"SeatHoldResponse"};
  std::string requestId;
  std::string holdId;
  std::string flightId;
  std::vector<HeldSeat> heldSeats;
  std::string expiresAt;
  Money totalPrice{};
};

struct CardPayment {
  std::string method{"card"};
  std::string paymentToken;
  std::string brand;
  std::string last4;
  std::optional<std::string> cardholderName;
  Address billingAddress{};
};

struct SeatPurchaseRequest {
  std::string messageType{"SeatPurchaseRequest"};
  std::string requestId;
  std::string idempotencyKey;
  std::string flightId;
  std::optional<std::string> bookingReference;
  std::optional<std::string> holdId;
  std::optional<std::string> fareClass;
  std::vector<Passenger> passengers;
  std::vector<SeatSelection> seatSelections;
  ContactInfo contact{};
  CardPayment payment{};
  Money expectedTotal{};
  std::string termsAcceptedAt;
  std::string termsVersion;
};

struct PaymentInfo {
  std::string transactionId;
  std::string method;
  std::string authorizedAt;
  std::optional<std::string> last4;
};

struct SeatPurchaseResponse {
  std::string messageType{"SeatPurchaseResponse"};
  std::string requestId;
  std::string status;  // confirmed | pending_payment_action | failed
  std::optional<std::string> bookingReference;
  std::optional<std::string> confirmationNumber;
  std::optional<Flight> flight;
  std::optional<std::vector<SeatAssignment>> seatAssignments;
  std::optional<std::vector<ChargeLine>> charges;
  std::optional<Money> totalCharged;
  std::optional<PaymentInfo> payment;
  std::optional<std::string> purchasedAt;
  std::optional<ErrorBody> error;
};

struct ErrorResponse {
  std::string messageType{"ErrorResponse"};
  std::optional<std::string> requestId;
  ErrorBody error{};
};

}  // namespace reservations::domain
