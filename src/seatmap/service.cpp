#include "seatmap/service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <sstream>

#include "common/time_format.hpp"
#include "seatmap/synthetic.hpp"

namespace reservations::seatmap {
namespace {

std::string SeatMapKey(const std::string& flight_id) { return "seatmap::" + flight_id; }
std::string FlightKey(const std::string& flight_id) { return "flight::" + flight_id; }
std::string InventoryKey(const std::string& flight_id) { return "inv::" + flight_id; }
std::string HoldKey(const std::string& hold_id) { return "hold::" + hold_id; }
std::string PnrKey(const std::string& locator) { return "pnr::" + locator; }
std::string IdempotencyKey(const std::string& key) { return "idem::" + key; }
std::string FlightIndexKey(const std::string& airline, const std::string& number, const std::string& date) {
  return "flight_idx::" + airline + "::" + number + "::" + date;
}

std::string RequestFingerprint(const domain::SeatPurchaseRequest& request) {
  std::ostringstream oss;
  oss << request.flightId << '|';
  for (const auto& sel : request.seatSelections) {
    oss << sel.passengerId << ':' << sel.seatNumber << ':' << sel.expectedPrice.amountMinor << ';';
  }
  oss << request.expectedTotal.amountMinor << '|' << request.expectedTotal.currency;
  return oss.str();
}

std::string BookingFingerprint(const domain::FlightBookingRequest& request) {
  std::ostringstream oss;
  oss << request.flightId << '|';
  for (const auto& p : request.passengers) {
    oss << p.passengerId << ':' << p.lastName << ';';
  }
  return oss.str();
}

domain::ErrorBody ToErrorBody(const Error& error) {
  return domain::ErrorBody{
      .code = ToWireCode(error.code),
      .message = error.message,
      .retryable = error.retryable,
  };
}

domain::SeatPurchaseResponse FailedPurchase(const std::string& request_id, const Error& error) {
  return domain::SeatPurchaseResponse{
      .messageType = "SeatPurchaseResponse",
      .requestId = request_id,
      .status = "failed",
      .error = ToErrorBody(error),
  };
}

domain::FlightBookingResponse FailedBooking(const std::string& request_id, const Error& error) {
  return domain::FlightBookingResponse{
      .messageType = "FlightBookingResponse",
      .requestId = request_id,
      .status = "failed",
      .error = ToErrorBody(error),
  };
}

}  // namespace

ReservationService::ReservationService(platform::CouchbaseClient& client, const Clock& clock)
    : client_(client),
      clock_(clock),
      flights_(client_, kFlightsCollection),
      seatmaps_(client_, kSeatmapsCollection),
      inventory_(client_, kInventoryCollection),
      holds_(client_, kHoldsCollection),
      pnrs_(client_, kPnrsCollection),
      idempotency_(client_, kIdempotencyCollection),
      flight_index_(client_, kFlightIndexCollection) {}

Result<domain::Flight> ReservationService::FlightFromId(const std::string& flight_id) {
  unsigned long long idx = 0;
  if (std::sscanf(flight_id.c_str(), "00000000-0000-4000-8000-%llx", &idx) != 1) {
    return Result<domain::Flight>::Failure(Error::Make(ErrorCode::FlightNotFound, "Invalid flight id"));
  }
  if (idx >= schedule::TotalFlights()) {
    return Result<domain::Flight>::Failure(Error::Make(ErrorCode::FlightNotFound, "Flight out of catalog range"));
  }
  const auto ref = schedule::FlightRef::FromGlobalIndex(static_cast<std::uint64_t>(idx));
  return Result<domain::Flight>::Success(
      BuildCatalogFlight(ref, catalog_meta_.resetEpoch).flight);
}

Result<domain::Flight> ReservationService::ResolveFlight(
    const std::variant<domain::FlightById, domain::FlightBySchedule>& flight) {
  if (const auto* by_id = std::get_if<domain::FlightById>(&flight)) {
    return FlightFromId(by_id->flightId);
  }

  const auto& by_sched = std::get<domain::FlightBySchedule>(flight);
  auto idx = flight_index_.Get(FlightIndexKey(by_sched.airlineCode, by_sched.flightNumber, by_sched.departureDate));
  if (!idx) {
    return Result<domain::Flight>::Failure(idx.error());
  }
  if (!idx.value().has_value()) {
    return Result<domain::Flight>::Failure(Error::Make(ErrorCode::FlightNotFound, "Flight not found"));
  }
  return FlightFromId(idx.value()->document.flightId);
}

Result<platform::DocumentStore<domain::SeatMapDocument>::Loaded> ReservationService::LoadSeatMap(
    const std::string& flight_id) {
  auto loaded = seatmaps_.Get(SeatMapKey(flight_id));
  if (!loaded) {
    return Result<platform::DocumentStore<domain::SeatMapDocument>::Loaded>::Failure(loaded.error());
  }
  const bool stale_or_missing =
      !loaded.value().has_value() || loaded.value()->document.resetEpoch < catalog_meta_.resetEpoch;
  if (stale_or_missing) {
    auto fresh = BuildSeatMapTemplate(flight_id, catalog_meta_.resetEpoch);
    auto upserted = seatmaps_.Upsert(SeatMapKey(flight_id), fresh);
    if (!upserted) {
      return Result<platform::DocumentStore<domain::SeatMapDocument>::Loaded>::Failure(upserted.error());
    }
    return Result<platform::DocumentStore<domain::SeatMapDocument>::Loaded>::Success(
        platform::DocumentStore<domain::SeatMapDocument>::Loaded{.document = std::move(fresh),
                                                                .cas = upserted.value().cas});
  }
  return Result<platform::DocumentStore<domain::SeatMapDocument>::Loaded>::Success(std::move(*loaded.value()));
}

Result<platform::DocumentStore<domain::InventoryDocument>::Loaded> ReservationService::LoadInventory(
    const std::string& flight_id) {
  auto loaded = inventory_.Get(InventoryKey(flight_id));
  if (!loaded) {
    return Result<platform::DocumentStore<domain::InventoryDocument>::Loaded>::Failure(loaded.error());
  }
  const bool stale_or_missing =
      !loaded.value().has_value() || loaded.value()->document.resetEpoch < catalog_meta_.resetEpoch;
  if (stale_or_missing) {
    domain::InventoryDocument fresh;
    fresh.type = "inventory";
    fresh.flightId = flight_id;
    fresh.physicalCapacity = catalog_meta_.physicalCapacityPerFlight > 0
                                 ? catalog_meta_.physicalCapacityPerFlight
                                 : TemplatePhysicalCapacity();
    fresh.oversellPercent = catalog_meta_.oversellPercent;
    fresh.authorization = catalog_meta_.authorizationPerFlight > 0
                              ? catalog_meta_.authorizationPerFlight
                              : domain::AuthorizationFor(fresh.physicalCapacity, fresh.oversellPercent);
    fresh.sold = 0;
    fresh.resetEpoch = catalog_meta_.resetEpoch;
    auto upserted = inventory_.Upsert(InventoryKey(flight_id), fresh);
    if (!upserted) {
      return Result<platform::DocumentStore<domain::InventoryDocument>::Loaded>::Failure(upserted.error());
    }
    return Result<platform::DocumentStore<domain::InventoryDocument>::Loaded>::Success(
        platform::DocumentStore<domain::InventoryDocument>::Loaded{.document = std::move(fresh),
                                                                   .cas = upserted.value().cas});
  }
  return Result<platform::DocumentStore<domain::InventoryDocument>::Loaded>::Success(std::move(*loaded.value()));
}

Result<std::vector<domain::BookableFlight>> ReservationService::ListBookableFlights(
    const std::vector<std::string>& flight_ids) {
  std::vector<domain::BookableFlight> out;
  out.reserve(flight_ids.size());
  for (const auto& flight_id : flight_ids) {
    auto flight = FlightFromId(flight_id);
    if (!flight) {
      continue;
    }
    auto inv = LoadInventory(flight_id);
    if (!inv) {
      continue;
    }
    const auto& inventory = inv.value().document;
    if (inventory.sold >= inventory.authorization) {
      continue;  // Full — omit from shopping options.
    }
    domain::BookableFlight row;
    row.flight = flight.value();
    row.physicalCapacity = inventory.physicalCapacity;
    row.authorization = inventory.authorization;
    row.sold = inventory.sold;
    row.remainingToSell = inventory.authorization - inventory.sold;
    out.push_back(std::move(row));
  }
  return Result<std::vector<domain::BookableFlight>>::Success(std::move(out));
}

Result<domain::FlightBookingResponse> ReservationService::BookFlight(
    const domain::FlightBookingRequest& request) {
  {
    auto existing = idempotency_.Get(IdempotencyKey(request.idempotencyKey));
    if (!existing) {
      return Result<domain::FlightBookingResponse>::Failure(existing.error());
    }
    if (existing.value().has_value()) {
      if (existing.value()->document.requestHash != BookingFingerprint(request)) {
        return Result<domain::FlightBookingResponse>::Success(
            FailedBooking(request.requestId, Error::Make(ErrorCode::DuplicateRequest,
                                                         "Idempotency key reused with different payload")));
      }
      auto prior = rfl::json::read<domain::FlightBookingResponse>(existing.value()->document.responseJson);
      if (!prior) {
        return Result<domain::FlightBookingResponse>::Failure(
            Error::Make(ErrorCode::InternalError, "Corrupt idempotency record"));
      }
      return Result<domain::FlightBookingResponse>::Success(std::move(prior.value()));
    }
  }

  if (request.passengers.empty()) {
    return Result<domain::FlightBookingResponse>::Success(
        FailedBooking(request.requestId, Error::Make(ErrorCode::ValidationFailed, "At least one passenger required")));
  }

  constexpr int kMaxAttempts = 8;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    auto flight_loaded = FlightFromId(request.flightId);
    if (!flight_loaded) {
      return Result<domain::FlightBookingResponse>::Success(
          FailedBooking(request.requestId, flight_loaded.error()));
    }
    const auto flight = flight_loaded.value();

    auto inv = LoadInventory(request.flightId);
    if (!inv) {
      return Result<domain::FlightBookingResponse>::Failure(inv.error());
    }
    auto inventory = inv.value().document;
    const int qty = static_cast<int>(request.passengers.size());
    if (inventory.sold + qty > inventory.authorization) {
      return Result<domain::FlightBookingResponse>::Success(
          FailedBooking(request.requestId,
                        Error::Make(ErrorCode::FlightFull, "Flight is full (authorization exhausted)")));
    }

    inventory.sold += qty;
    auto replaced = inventory_.Replace(InventoryKey(request.flightId), inventory, inv.value().cas);
    if (!replaced) {
      if (replaced.error().code == ErrorCode::VersionConflict) {
        continue;
      }
      return Result<domain::FlightBookingResponse>::Failure(replaced.error());
    }

    const std::string locator = locators_.Next();
    const std::string confirmation = "CONF-" + locator;
    const auto booked_at = FormatRfc3339(clock_.Now());

    domain::PnrDocument pnr;
    pnr.bookingReference = locator;
    pnr.confirmationNumber = confirmation;
    pnr.flightId = request.flightId;
    pnr.passengers = request.passengers;
    pnr.contact = request.contact;
    pnr.purchasedAt = booked_at;
    pnr.seatAssignmentPending = true;
    pnr.totalCharged = domain::Money{.amountMinor = 0, .currency = "USD"};
    (void)pnrs_.Upsert(PnrKey(locator), pnr);

    domain::FlightBookingResponse response;
    response.requestId = request.requestId;
    response.status = "confirmed";
    response.bookingReference = locator;
    response.confirmationNumber = confirmation;
    response.flight = flight;
    response.bookedAt = booked_at;
    response.seatAssignmentPending = true;

    domain::IdempotencyDocument idem;
    idem.idempotencyKey = request.idempotencyKey;
    idem.requestHash = BookingFingerprint(request);
    idem.responseJson = rfl::json::write(response);
    idem.createdAt = booked_at;
    (void)idempotency_.Insert(IdempotencyKey(request.idempotencyKey), idem);

    return Result<domain::FlightBookingResponse>::Success(std::move(response));
  }

  return Result<domain::FlightBookingResponse>::Failure(
      Error::Make(ErrorCode::InternalError, "Exhausted CAS retries while booking flight"));
}

std::vector<domain::CabinSummary> ReservationService::BuildSummary(const domain::SeatMapDocument& map) const {
  std::vector<domain::CabinSummary> summaries;
  auto find_or_add = [&](domain::CabinClass cabin) -> domain::CabinSummary& {
    for (auto& s : summaries) {
      if (s.cabin == cabin) {
        return s;
      }
    }
    summaries.push_back(domain::CabinSummary{.cabin = cabin});
    return summaries.back();
  };

  for (const auto& seat : map.seats) {
    auto& summary = find_or_add(seat.cabin);
    ++summary.totalSeats;
    switch (seat.status) {
      case domain::SeatStatus::available:
        ++summary.available;
        break;
      case domain::SeatStatus::held:
        ++summary.held;
        break;
      case domain::SeatStatus::occupied:
        ++summary.occupied;
        break;
      case domain::SeatStatus::blocked:
        ++summary.blocked;
        break;
    }
  }
  return summaries;
}

bool ReservationService::SeatMatchesFilters(const domain::Seat& seat,
                                            const std::optional<domain::AvailabilityFilters>& filters) const {
  if (!filters.has_value()) {
    return seat.status == domain::SeatStatus::available;
  }
  const auto& f = *filters;
  if (f.statuses.has_value()) {
    if (std::find(f.statuses->begin(), f.statuses->end(), seat.status) == f.statuses->end()) {
      return false;
    }
  } else if (seat.status != domain::SeatStatus::available) {
    return false;
  }
  if (f.cabins.has_value() &&
      std::find(f.cabins->begin(), f.cabins->end(), seat.cabin) == f.cabins->end()) {
    return false;
  }
  if (f.positions.has_value() && std::find(f.positions->begin(), f.positions->end(), seat.attributes.position) ==
                                     f.positions->end()) {
    return false;
  }
  if (f.requireExitRow.value_or(false) && !seat.attributes.exitRow.value_or(false)) {
    return false;
  }
  if (f.requireExtraLegroom.value_or(false) && !seat.attributes.extraLegroom.value_or(false)) {
    return false;
  }
  if (f.requirePowerOutlet.value_or(false) && !seat.attributes.powerOutlet.value_or(false)) {
    return false;
  }
  if (f.requireWheelchairAccessible.value_or(false) && !seat.attributes.wheelchairAccessible.value_or(false)) {
    return false;
  }
  if (f.maxPrice.has_value() && seat.price.has_value()) {
    if (seat.price->currency != f.maxPrice->currency || seat.price->amountMinor > f.maxPrice->amountMinor) {
      return false;
    }
  }
  return true;
}

Result<domain::SeatAvailabilityResponse> ReservationService::CheckAvailability(
    const domain::SeatAvailabilityRequest& request) {
  auto flight = ResolveFlight(request.flight);
  if (!flight) {
    return Result<domain::SeatAvailabilityResponse>::Failure(flight.error());
  }

  auto map = LoadSeatMap(flight.value().flightId);
  if (!map) {
    return Result<domain::SeatAvailabilityResponse>::Failure(map.error());
  }

  domain::SeatAvailabilityResponse response;
  response.requestId = request.requestId;
  response.generatedAt = FormatRfc3339(clock_.Now());
  response.flight = flight.value();
  response.summary = BuildSummary(map.value().document);
  response.seatMapVersion = map.value().document.seatMapVersion;
  if (request.includeLayout.value_or(true)) {
    response.layout = map.value().document.layout;
  }

  // Seat selection never fails for a full/oversold cabin — return zero selectable seats.
  const bool selection_open = !flight.value().seatSelectionOpen.has_value() || *flight.value().seatSelectionOpen;
  if (selection_open) {
    for (const auto& seat : map.value().document.seats) {
      if (SeatMatchesFilters(seat, request.filters)) {
        response.seats.push_back(seat);
      }
    }
  }
  return Result<domain::SeatAvailabilityResponse>::Success(std::move(response));
}

Result<domain::SeatHoldResponse> ReservationService::HoldSeats(const domain::SeatHoldRequest& request) {
  constexpr int kMaxAttempts = 8;
  const int hold_seconds = request.requestedHoldSeconds.value_or(600);
  const auto expires_at = clock_.Now() + std::chrono::seconds(hold_seconds);

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    auto flight_loaded = FlightFromId(request.flightId);
    if (!flight_loaded) {
      return Result<domain::SeatHoldResponse>::Failure(flight_loaded.error());
    }
    if (flight_loaded.value().seatSelectionOpen.has_value() && !*flight_loaded.value().seatSelectionOpen) {
      return Result<domain::SeatHoldResponse>::Failure(
          Error::Make(ErrorCode::FlightClosedForSales, "Seat selection is closed for this flight"));
    }

    auto map = LoadSeatMap(request.flightId);
    if (!map) {
      return Result<domain::SeatHoldResponse>::Failure(map.error());
    }
    if (request.seatMapVersion.has_value() && *request.seatMapVersion != map.value().document.seatMapVersion) {
      return Result<domain::SeatHoldResponse>::Failure(
          Error::Make(ErrorCode::SeatMapStale, "Seat map version is stale"));
    }

    domain::SeatHoldResponse response;
    response.requestId = request.requestId;
    response.holdId = uuid_.Next();
    response.flightId = request.flightId;
    response.expiresAt = FormatRfc3339(expires_at);
    response.totalPrice.currency = "USD";
    response.totalPrice.amountMinor = 0;

    auto& doc = map.value().document;
    for (const auto& seat_number : request.seats) {
      auto it = std::find_if(doc.seats.begin(), doc.seats.end(),
                             [&](const domain::Seat& s) { return s.seatNumber == seat_number; });
      if (it == doc.seats.end()) {
        return Result<domain::SeatHoldResponse>::Failure(
            Error::Make(ErrorCode::SeatNotFound, "Seat not found: " + seat_number));
      }
      if (it->status != domain::SeatStatus::available || !it->price.has_value()) {
        return Result<domain::SeatHoldResponse>::Failure(
            Error::Make(ErrorCode::SeatUnavailable, "Seat unavailable: " + seat_number));
      }
      it->status = domain::SeatStatus::held;
      it->heldUntil = response.expiresAt;
      it->holdId = response.holdId;
      response.heldSeats.push_back(domain::HeldSeat{.seatNumber = seat_number, .price = *it->price});
      response.totalPrice.amountMinor += it->price->amountMinor;
      response.totalPrice.currency = it->price->currency;
    }

    doc.seatMapVersion = "v" + std::to_string(std::stoll(doc.seatMapVersion.substr(1)) + 1);
    doc.updatedAt = FormatRfc3339(clock_.Now());

    auto replaced = seatmaps_.Replace(SeatMapKey(request.flightId), doc, map.value().cas);
    if (!replaced) {
      if (replaced.error().code == ErrorCode::VersionConflict) {
        continue;
      }
      return Result<domain::SeatHoldResponse>::Failure(replaced.error());
    }

    domain::HoldDocument hold;
    hold.holdId = response.holdId;
    hold.flightId = request.flightId;
    hold.seats = request.seats;
    for (const auto& hs : response.heldSeats) {
      hold.prices.push_back(hs.price);
    }
    hold.totalPrice = response.totalPrice;
    hold.expiresAt = response.expiresAt;
    hold.state = domain::HoldState::Active;
    hold.createdAt = FormatRfc3339(clock_.Now());

    auto stored = holds_.Upsert(HoldKey(response.holdId), hold, static_cast<std::uint32_t>(hold_seconds));
    if (!stored) {
      return Result<domain::SeatHoldResponse>::Failure(stored.error());
    }
    return Result<domain::SeatHoldResponse>::Success(std::move(response));
  }

  return Result<domain::SeatHoldResponse>::Failure(
      Error::Make(ErrorCode::InternalError, "Exhausted CAS retries while holding seats"));
}

Result<domain::SeatPurchaseResponse> ReservationService::PurchaseSeats(
    const domain::SeatPurchaseRequest& request) {
  // Idempotency: same key + same payload returns prior result.
  {
    auto existing = idempotency_.Get(IdempotencyKey(request.idempotencyKey));
    if (!existing) {
      return Result<domain::SeatPurchaseResponse>::Failure(existing.error());
    }
    if (existing.value().has_value()) {
      const auto fingerprint = RequestFingerprint(request);
      if (existing.value()->document.requestHash != fingerprint) {
        return Result<domain::SeatPurchaseResponse>::Success(
            FailedPurchase(request.requestId, Error::Make(ErrorCode::DuplicateRequest,
                                                          "Idempotency key reused with different payload")));
      }
      auto prior = rfl::json::read<domain::SeatPurchaseResponse>(existing.value()->document.responseJson);
      if (!prior) {
        return Result<domain::SeatPurchaseResponse>::Failure(
            Error::Make(ErrorCode::InternalError, "Corrupt idempotency record"));
      }
      return Result<domain::SeatPurchaseResponse>::Success(std::move(prior.value()));
    }
  }

  if (request.payment.paymentToken.size() < 8) {
    return Result<domain::SeatPurchaseResponse>::Success(
        FailedPurchase(request.requestId, Error::Make(ErrorCode::ValidationFailed, "Invalid payment token")));
  }

  constexpr int kMaxAttempts = 8;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    auto flight_loaded = FlightFromId(request.flightId);
    if (!flight_loaded) {
      return Result<domain::SeatPurchaseResponse>::Success(
          FailedPurchase(request.requestId, flight_loaded.error()));
    }
    const auto flight = flight_loaded.value();
    if (flight.seatSelectionOpen.has_value() && !*flight.seatSelectionOpen) {
      return Result<domain::SeatPurchaseResponse>::Success(
          FailedPurchase(request.requestId,
                         Error::Make(ErrorCode::FlightClosedForSales, "Seat selection is closed")));
    }

    std::optional<domain::HoldDocument> hold_doc;
    if (request.holdId.has_value()) {
      auto hold = holds_.Get(HoldKey(*request.holdId));
      if (!hold) {
        return Result<domain::SeatPurchaseResponse>::Failure(hold.error());
      }
      if (!hold.value().has_value()) {
        return Result<domain::SeatPurchaseResponse>::Success(
            FailedPurchase(request.requestId, Error::Make(ErrorCode::HoldNotFound, "Hold not found")));
      }
      hold_doc = hold.value()->document;
      if (hold_doc->state != domain::HoldState::Active) {
        return Result<domain::SeatPurchaseResponse>::Success(
            FailedPurchase(request.requestId, Error::Make(ErrorCode::HoldExpired, "Hold is not active")));
      }
      if (ParseRfc3339(hold_doc->expiresAt) <= clock_.Now()) {
        return Result<domain::SeatPurchaseResponse>::Success(
            FailedPurchase(request.requestId, Error::Make(ErrorCode::HoldExpired, "Hold has expired")));
      }
    }

    auto map = LoadSeatMap(request.flightId);
    if (!map) {
      return Result<domain::SeatPurchaseResponse>::Failure(map.error());
    }

    std::int64_t total = 0;
    std::string currency = request.expectedTotal.currency;
    std::vector<domain::SeatAssignment> assignments;
    auto& doc = map.value().document;

    for (const auto& selection : request.seatSelections) {
      auto it = std::find_if(doc.seats.begin(), doc.seats.end(),
                             [&](const domain::Seat& s) { return s.seatNumber == selection.seatNumber; });
      if (it == doc.seats.end()) {
        return Result<domain::SeatPurchaseResponse>::Success(
            FailedPurchase(request.requestId,
                           Error::Make(ErrorCode::SeatNotFound, "Seat not found: " + selection.seatNumber)));
      }

      const bool held_for_us = request.holdId.has_value() && it->status == domain::SeatStatus::held &&
                               it->holdId.has_value() && *it->holdId == *request.holdId;
      if (it->status != domain::SeatStatus::available && !held_for_us) {
        return Result<domain::SeatPurchaseResponse>::Success(
            FailedPurchase(request.requestId,
                           Error::Make(ErrorCode::SeatUnavailable, "Seat unavailable: " + selection.seatNumber)));
      }
      if (!it->price.has_value()) {
        return Result<domain::SeatPurchaseResponse>::Success(
            FailedPurchase(request.requestId, Error::Make(ErrorCode::InternalError, "Missing seat price")));
      }
      if (it->price->amountMinor != selection.expectedPrice.amountMinor ||
          it->price->currency != selection.expectedPrice.currency) {
        return Result<domain::SeatPurchaseResponse>::Success(
            FailedPurchase(request.requestId, Error::Make(ErrorCode::PriceChanged, "Seat price changed")));
      }

      total += it->price->amountMinor;
      currency = it->price->currency;
      assignments.push_back(domain::SeatAssignment{
          .passengerId = selection.passengerId,
          .seatNumber = selection.seatNumber,
          .cabin = it->cabin,
          .price = *it->price,
      });

      it->status = domain::SeatStatus::occupied;
      it->heldUntil = std::nullopt;
      it->holdId = std::nullopt;
      it->price = std::nullopt;
    }

    if (total != request.expectedTotal.amountMinor || currency != request.expectedTotal.currency) {
      return Result<domain::SeatPurchaseResponse>::Success(
          FailedPurchase(request.requestId, Error::Make(ErrorCode::PriceChanged, "Total price changed")));
    }

    doc.seatMapVersion = "v" + std::to_string(std::stoll(doc.seatMapVersion.substr(1)) + 1);
    doc.updatedAt = FormatRfc3339(clock_.Now());

    auto replaced = seatmaps_.Replace(SeatMapKey(request.flightId), doc, map.value().cas);
    if (!replaced) {
      if (replaced.error().code == ErrorCode::VersionConflict) {
        continue;
      }
      return Result<domain::SeatPurchaseResponse>::Failure(replaced.error());
    }

    if (hold_doc.has_value() && CanTransition(hold_doc->state, domain::HoldState::Consumed)) {
      hold_doc->state = domain::HoldState::Consumed;
      (void)holds_.Upsert(HoldKey(hold_doc->holdId), *hold_doc);
    }

    const auto purchased_at = FormatRfc3339(clock_.Now());
    std::string locator;
    std::string confirmation;

    std::vector<domain::ChargeLine> charges;
    for (const auto& assignment : assignments) {
      charges.push_back(domain::ChargeLine{
          .type = "seat_fee",
          .description = "Seat selection " + assignment.seatNumber,
          .passengerId = assignment.passengerId,
          .seatNumber = assignment.seatNumber,
          .amount = assignment.price,
      });
    }

    domain::PnrDocument pnr;
    if (request.bookingReference.has_value()) {
      auto existing_pnr = pnrs_.Get(PnrKey(*request.bookingReference));
      if (!existing_pnr) {
        return Result<domain::SeatPurchaseResponse>::Failure(existing_pnr.error());
      }
      if (!existing_pnr.value().has_value()) {
        return Result<domain::SeatPurchaseResponse>::Success(
            FailedPurchase(request.requestId, Error::Make(ErrorCode::NotFound, "Booking not found")));
      }
      pnr = existing_pnr.value()->document;
      locator = pnr.bookingReference;
      confirmation = pnr.confirmationNumber;
      pnr.seatAssignments = assignments;
      pnr.charges = charges;
      pnr.totalCharged = domain::Money{.amountMinor = total, .currency = currency};
      pnr.seatAssignmentPending = false;
      ++pnr.version;
    } else {
      locator = locators_.Next();
      confirmation = "CONF-" + locator;
      pnr.bookingReference = locator;
      pnr.confirmationNumber = confirmation;
      pnr.flightId = request.flightId;
      pnr.passengers = request.passengers;
      pnr.seatAssignments = assignments;
      pnr.charges = charges;
      pnr.totalCharged = domain::Money{.amountMinor = total, .currency = currency};
      pnr.contact = request.contact;
      pnr.purchasedAt = purchased_at;
      pnr.seatAssignmentPending = false;
    }
    (void)pnrs_.Upsert(PnrKey(locator), pnr);

    domain::SeatPurchaseResponse response;
    response.requestId = request.requestId;
    response.status = "confirmed";
    response.bookingReference = locator;
    response.confirmationNumber = confirmation;
    response.flight = flight;
    response.seatAssignments = assignments;
    response.charges = charges;
    response.totalCharged = pnr.totalCharged;
    response.payment = domain::PaymentInfo{
        .transactionId = "txn-" + uuid_.Next().substr(0, 8),
        .method = "card",
        .authorizedAt = purchased_at,
        .last4 = request.payment.last4,
    };
    response.purchasedAt = purchased_at;

    domain::IdempotencyDocument idem;
    idem.idempotencyKey = request.idempotencyKey;
    idem.requestHash = RequestFingerprint(request);
    idem.responseJson = rfl::json::write(response);
    idem.createdAt = purchased_at;
    (void)idempotency_.Insert(IdempotencyKey(request.idempotencyKey), idem);

    return Result<domain::SeatPurchaseResponse>::Success(std::move(response));
  }

  return Result<domain::SeatPurchaseResponse>::Failure(
      Error::Make(ErrorCode::InternalError, "Exhausted CAS retries while purchasing seats"));
}

}  // namespace reservations::seatmap
