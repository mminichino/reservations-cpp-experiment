#include "availability/service.hpp"

#include <algorithm>
#include <set>

#include "common/time_format.hpp"
#include "pricing/engine.hpp"

namespace reservations::availability {
namespace {

constexpr const char* kRbds[] = {"Y", "B", "M", "H", "Q", "V", "J", "C", "D"};

std::string BookingDateFromRequest(const domain::ClassAvailabilityRequest& req, const Clock& clock) {
  if (req.bookingTime.has_value() && req.bookingTime->size() >= 10) {
    return req.bookingTime->substr(0, 10);
  }
  const auto now = FormatRfc3339(clock.Now());
  return now.size() >= 10 ? now.substr(0, 10) : "2026-11-01";
}

domain::CabinClass CabinForRbd(std::string_view rbd) {
  if (rbd == "J" || rbd == "C" || rbd == "D" || rbd == "I") {
    return domain::CabinClass::business;
  }
  if (rbd == "F" || rbd == "A" || rbd == "P") {
    return domain::CabinClass::first;
  }
  if (rbd == "W" || rbd == "S" || rbd == "T") {
    return domain::CabinClass::premium_economy;
  }
  return domain::CabinClass::economy;
}

domain::ClassAvailability EvaluateClass(const domain::InventoryDocument& inv,
                                        const std::vector<domain::PriceRecord>& prices,
                                        const domain::PricingContext& ctx, const domain::Market& market,
                                        std::string_view rbd, bool explain, int seat_cap) {
  domain::ClassAvailability out;
  out.bookingClass = std::string(rbd);
  out.cabin = CabinForRbd(rbd);
  out.status = domain::ClassStatus::closed;
  out.seatsAvailable = 0;

  const domain::ClassControl* ctrl = nullptr;
  if (!inv.legs.empty()) {
    for (const auto& c : inv.legs.front().classes) {
      if (c.bookingClass == rbd) {
        ctrl = &c;
        break;
      }
    }
  }

  int evaluated = 0;
  int matched = 0;
  auto best = pricing::SelectBest(prices, ctx, rbd, &evaluated, &matched);

  domain::DecisionTraceWire trace;
  trace.candidatesEvaluated = evaluated;
  trace.candidatesMatched = matched;

  if (!best.has_value()) {
    trace.closedReason = "no_applicable_price";
    if (explain) {
      out.decisionTrace = std::move(trace);
    }
    return out;
  }

  trace.appliedRecords = std::vector<domain::AppliedPriceRecord>{{market, best->recordId}};
  trace.appliedValue = best->value;

  if (ctrl == nullptr || ctrl->status == domain::ClassStatus::closed || ctrl->seatsAvailable <= 0) {
    trace.closedReason = ctrl == nullptr ? "class_not_offered" : "no_seats";
    if (explain) {
      out.decisionTrace = std::move(trace);
    }
    return out;
  }

  if (!inv.legs.empty()) {
    for (const auto& cabin : inv.legs.front().cabins) {
      if (cabin.cabin == out.cabin && cabin.bidPrice.has_value()) {
        if (best->value.amountMinor < cabin.bidPrice->amountMinor) {
          trace.closedReason = "value_below_bid_price";
          if (explain) {
            out.decisionTrace = std::move(trace);
          }
          return out;
        }
      }
    }
  }

  out.status = ctrl->status == domain::ClassStatus::waitlist ? domain::ClassStatus::waitlist
                                                            : domain::ClassStatus::open;
  if (out.status == domain::ClassStatus::open) {
    out.seatsAvailable = std::min(ctrl->seatsAvailable, seat_cap);
    if (out.seatsAvailable <= 0) {
      out.status = domain::ClassStatus::closed;
      out.seatsAvailable = 0;
      trace.closedReason = "no_seats";
    }
  }
  if (explain) {
    out.decisionTrace = std::move(trace);
  }
  return out;
}

}  // namespace

ClassAvailabilityService::ClassAvailabilityService(platform::CouchbaseClient& client, const Clock& clock,
                                                   std::shared_ptr<cache::ClientCaches> caches)
    : client_(client), clock_(clock), caches_(std::move(caches)) {
  if (!caches_) {
    caches_ = std::make_shared<cache::ClientCaches>();
  }
}

Result<domain::ClassAvailabilityResponse> ClassAvailabilityService::Check(
    const domain::ClassAvailabilityRequest& request) {
  if (request.slices.empty()) {
    return Result<domain::ClassAvailabilityResponse>::Failure(
        Error::Make(ErrorCode::ValidationFailed, "At least one slice required"));
  }

  const int seat_cap = request.seatCountCap.value_or(9);
  const bool explain = request.explain.value_or(false);
  const int max_conn = request.filters.has_value() ? request.filters->maxConnections.value_or(1) : 1;
  const int max_journeys =
      request.filters.has_value() ? request.filters->maxJourneysPerSlice.value_or(50) : 50;
  const std::string detail = request.detailLevel.value_or("segment");
  const std::string booking_date = BookingDateFromRequest(request, clock_);

  domain::PricingContext ctx;
  ctx.salesChannel = request.salesChannel;
  ctx.salesLocation = request.salesLocation;
  ctx.bookingDate = booking_date;
  ctx.tripType = request.tripType;
  ctx.passengerType = request.passengers.empty() ? domain::PassengerType::adult
                                                 : request.passengers.front().passengerType;
  ctx.outboundDepartureDate = request.slices.front().departureDate;
  if (request.slices.size() > 1) {
    ctx.returnDepartureDate = request.slices[1].departureDate;
  }

  domain::ClassAvailabilityResponse response;
  response.requestId = request.requestId;
  response.generatedAt = FormatRfc3339(clock_.Now());
  response.inventoryAsOf = response.generatedAt;

  std::set<std::string> snapshot_markets;

  for (std::size_t si = 0; si < request.slices.size(); ++si) {
    const auto& slice = request.slices[si];
    domain::SliceResult slice_out;
    slice_out.sliceIndex = static_cast<int>(si);
    slice_out.origin = slice.origin;
    slice_out.destination = slice.destination;
    slice_out.departureDate = slice.departureDate;

    domain::Market market{.origin = slice.origin, .destination = slice.destination};

    auto prices_ptr = caches_->prices->GetMarketPrices(client_, slice.origin, slice.destination);
    if (!prices_ptr) {
      return Result<domain::ClassAvailabilityResponse>::Failure(prices_ptr.error());
    }
    const auto& prices = *prices_ptr.value();
    const std::string snap = caches_->prices->CurrentSnapshot().snapshotId;
    if (snapshot_markets.insert(schedule::MarketKey(slice.origin, slice.destination)).second) {
      response.priceSnapshots.push_back(
          domain::PriceSnapshotRef{.market = market, .snapshotId = snap.empty() ? "unknown" : snap});
    }

    auto directs = caches_->schedule->GetDirectFlights(client_, slice.origin, slice.destination,
                                                       slice.departureDate, catalog_meta_.resetEpoch);
    if (!directs) {
      return Result<domain::ClassAvailabilityResponse>::Failure(directs.error());
    }

    int journey_count = 0;
    auto add_journey = [&](std::vector<std::string> flight_ids, const std::vector<domain::Market>& markets) {
      if (journey_count >= max_journeys) {
        slice_out.journeysTruncated = true;
        return;
      }
      domain::Journey journey;
      journey.journeyId = "J-" + std::to_string(si) + "-" + std::to_string(journey_count);
      journey.connectionCount = static_cast<int>(flight_ids.size()) - 1;
      journey.markets = markets;

      bool ok = true;
      for (std::size_t seg_i = 0; seg_i < flight_ids.size(); ++seg_i) {
        auto flight = caches_->schedule->GetFlight(client_, flight_ids[seg_i]);
        if (!flight) {
          ok = false;
          break;
        }
        domain::JourneySegment seg;
        seg.segmentIndex = static_cast<int>(seg_i);
        seg.flight = *flight.value();
        seg.boardPoint = seg.flight.origin;
        seg.offPoint = seg.flight.destination;

        auto inv = caches_->inventory->Get(client_, flight_ids[seg_i], catalog_meta_.resetEpoch);
        if (!inv) {
          ok = false;
          break;
        }

        domain::Market seg_market{.origin = seg.boardPoint, .destination = seg.offPoint};
        std::shared_ptr<const std::vector<domain::PriceRecord>> seg_prices_ptr = prices_ptr.value();
        if (!(seg_market.origin == market.origin && seg_market.destination == market.destination)) {
          auto loaded =
              caches_->prices->GetMarketPrices(client_, seg_market.origin, seg_market.destination);
          if (!loaded) {
            ok = false;
            break;
          }
          seg_prices_ptr = loaded.value();
        }

        std::vector<domain::ClassAvailability> class_avails;
        for (const char* rbd : kRbds) {
          class_avails.push_back(
              EvaluateClass(inv.value().document, *seg_prices_ptr, ctx, seg_market, rbd, explain, seat_cap));
        }
        if (detail == "segment" || detail == "leg") {
          seg.classes = class_avails;
        }
        if (detail == "leg") {
          seg.legs = inv.value().document.legs;
        }
        if (seg_i == 0) {
          journey.classes = class_avails;
        } else {
          for (auto& jc : journey.classes) {
            auto it = std::find_if(class_avails.begin(), class_avails.end(),
                                   [&](const domain::ClassAvailability& c) {
                                     return c.bookingClass == jc.bookingClass;
                                   });
            if (it == class_avails.end() || it->status != domain::ClassStatus::open) {
              jc.status = domain::ClassStatus::closed;
              jc.seatsAvailable = 0;
            } else {
              jc.seatsAvailable = std::min(jc.seatsAvailable, it->seatsAvailable);
            }
          }
        }
        journey.segments.push_back(std::move(seg));
      }
      if (!ok || journey.segments.empty()) {
        return;
      }
      slice_out.journeys.push_back(std::move(journey));
      ++journey_count;
    };

    for (const auto& fid : *directs.value()) {
      add_journey({fid}, {market});
    }

    if (max_conn >= 1) {
      for (const auto hub : schedule::kConnectionHubs) {
        if (hub == slice.origin || hub == slice.destination) {
          continue;
        }
        auto leg1 = caches_->schedule->GetDirectFlights(client_, slice.origin, hub, slice.departureDate,
                                                        catalog_meta_.resetEpoch);
        auto leg2 = caches_->schedule->GetDirectFlights(client_, hub, slice.destination, slice.departureDate,
                                                        catalog_meta_.resetEpoch);
        if (!leg1 || !leg2 || leg1.value()->empty() || leg2.value()->empty()) {
          continue;
        }
        const std::size_t n1 = std::min<std::size_t>(leg1.value()->size(), 2);
        const std::size_t n2 = std::min<std::size_t>(leg2.value()->size(), 2);
        for (std::size_t i = 0; i < n1; ++i) {
          for (std::size_t j = 0; j < n2; ++j) {
            add_journey({(*leg1.value())[i], (*leg2.value())[j]},
                        {domain::Market{.origin = slice.origin, .destination = std::string(hub)},
                         domain::Market{.origin = std::string(hub), .destination = slice.destination},
                         market});
            if (journey_count >= max_journeys) {
              break;
            }
          }
        }
        if (journey_count >= max_journeys) {
          break;
        }
      }
    }

    response.slices.push_back(std::move(slice_out));
  }

  return Result<domain::ClassAvailabilityResponse>::Success(std::move(response));
}

}  // namespace reservations::availability
