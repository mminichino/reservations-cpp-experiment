#include "seatmap/synthetic.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace reservations::seatmap {
namespace {

domain::SeatPosition PositionForColumn(const std::string& column, const std::vector<std::string>& layout) {
  std::vector<std::string> seats;
  for (const auto& entry : layout) {
    if (entry != "aisle") {
      seats.push_back(entry);
    }
  }
  if (seats.empty()) {
    return domain::SeatPosition::middle;
  }
  if (column == seats.front() || column == seats.back()) {
    return domain::SeatPosition::window;
  }
  for (std::size_t i = 0; i < layout.size(); ++i) {
    if (layout[i] == "aisle") {
      if (i > 0 && layout[i - 1] == column) {
        return domain::SeatPosition::aisle;
      }
      if (i + 1 < layout.size() && layout[i + 1] == column) {
        return domain::SeatPosition::aisle;
      }
    }
  }
  return domain::SeatPosition::middle;
}

void AddCabin(domain::SeatMapDocument& seatmap, domain::CabinClass cabin, int first, int last,
              const std::vector<std::string>& layout, const std::vector<int>& skip, std::int64_t base_price) {
  for (int row = first; row <= last; ++row) {
    if (std::find(skip.begin(), skip.end(), row) != skip.end()) {
      continue;
    }
    for (const auto& col : layout) {
      if (col == "aisle") {
        continue;
      }
      domain::Seat seat;
      seat.row = row;
      seat.column = col;
      seat.seatNumber = std::to_string(row) + col;
      seat.cabin = cabin;
      seat.status = domain::SeatStatus::available;
      seat.attributes.position = PositionForColumn(col, layout);
      seat.attributes.powerOutlet = true;
      if (row == 12) {
        seat.attributes.exitRow = true;
        seat.attributes.extraLegroom = true;
        seat.restrictions = domain::SeatRestrictions{
            .minAge = 15,
            .noInfants = true,
            .requiresAbleBodied = true,
        };
      }
      seat.price = domain::Money{.amountMinor = base_price + (row * 10), .currency = "USD"};
      seatmap.seats.push_back(std::move(seat));
    }
  }
}

std::string DepartureIso(const std::string& local_date, int hour) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%sT%02d:30:00-06:00", local_date.c_str(), hour);
  return std::string(buf);
}

std::string ArrivalIso(const std::string& local_date, int hour) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%sT%02d:45:00-06:00", local_date.c_str(), hour + 3);
  return std::string(buf);
}

}  // namespace

domain::SeatMapDocument BuildSeatMapTemplate(const std::string& flight_id, std::uint64_t reset_epoch) {
  const std::vector<std::string> economy_layout = {"A", "B", "aisle", "C", "D"};
  const std::vector<std::string> business_layout = {"A", "aisle", "D"};

  domain::SeatMapDocument seatmap;
  seatmap.type = "seatmap";
  seatmap.flightId = flight_id;
  seatmap.seatMapVersion = "v1";
  seatmap.updatedAt = "2026-10-28T10:00:00Z";
  seatmap.resetEpoch = reset_epoch;
  seatmap.layout = {
      domain::CabinLayout{
          .cabin = domain::CabinClass::business,
          .firstRow = 1,
          .lastRow = 2,
          .columnLayout = business_layout,
      },
      domain::CabinLayout{
          .cabin = domain::CabinClass::economy,
          .firstRow = 10,
          .lastRow = 14,
          .columnLayout = economy_layout,
          .exitRows = std::vector<int>{12},
          .missingRows = std::vector<int>{13},
      },
  };
  AddCabin(seatmap, domain::CabinClass::business, 1, 2, business_layout, {}, 5000);
  AddCabin(seatmap, domain::CabinClass::economy, 10, 14, economy_layout, {13}, 1234);
  return seatmap;
}

int TemplatePhysicalCapacity() {
  static const int kCapacity = [] {
    const auto map = BuildSeatMapTemplate("00000000-0000-4000-8000-000000000000", 0);
    int physical = 0;
    for (const auto& seat : map.seats) {
      if (seat.status != domain::SeatStatus::blocked) {
        ++physical;
      }
    }
    return physical;
  }();
  return kCapacity;
}

SyntheticFlightBundle BuildCatalogFlight(const schedule::FlightRef& ref, std::uint64_t reset_epoch) {
  static constexpr const char* kOrigins[] = {"XXX", "YYY", "ZZZ", "AAA"};
  static constexpr const char* kDests[] = {"YYY", "ZZZ", "AAA", "XXX"};

  SyntheticFlightBundle bundle;
  const std::string flight_id = schedule::FlightId(ref);
  const std::string airline = schedule::AirlineCode(ref.airline_index);
  const std::string local_date = schedule::LocalDepartureDate(ref.week, ref.flight_in_week);
  const int hour = 6 + (ref.flight_in_week % 14);

  bundle.flight.flightId = flight_id;
  bundle.flight.airlineCode = airline;
  bundle.flight.flightNumber = std::to_string(1 + (ref.flight_in_week % 9999));
  bundle.flight.origin = kOrigins[static_cast<std::size_t>(ref.flight_in_week % 4)];
  bundle.flight.destination = kDests[static_cast<std::size_t>(ref.flight_in_week % 4)];
  bundle.flight.scheduledDeparture = DepartureIso(local_date, hour);
  bundle.flight.scheduledArrival = ArrivalIso(local_date, hour);
  bundle.flight.aircraft = domain::Aircraft{
      .icaoType = "XXXX",
      .name = "Synthetic Twinjet",
      .configurationId = "CONFIG-1234",
  };
  bundle.flight.status = domain::FlightStatus::scheduled;
  bundle.flight.seatSelectionOpen = true;

  bundle.index.type = "flight_index";
  bundle.index.flightId = flight_id;
  bundle.index.airlineCode = airline;
  bundle.index.flightNumber = bundle.flight.flightNumber;
  bundle.index.departureDate = local_date;

  // Amortize cabin construction across flights (layout is identical for the catalog).
  thread_local const domain::SeatMapDocument kSeatTemplate =
      BuildSeatMapTemplate("00000000-0000-4000-8000-000000000000", 0);
  bundle.seatmap = kSeatTemplate;
  bundle.seatmap.flightId = flight_id;
  bundle.seatmap.resetEpoch = reset_epoch;

  const int physical = TemplatePhysicalCapacity();
  bundle.inventory.type = "inventory";
  bundle.inventory.flightId = flight_id;
  bundle.inventory.physicalCapacity = physical;
  bundle.inventory.oversellPercent = domain::kDefaultOversellPercent;
  bundle.inventory.authorization = domain::AuthorizationFor(physical, domain::kDefaultOversellPercent);
  bundle.inventory.sold = 0;
  bundle.inventory.resetEpoch = reset_epoch;

  return bundle;
}

}  // namespace reservations::seatmap
