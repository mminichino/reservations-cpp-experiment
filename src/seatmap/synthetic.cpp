#include "seatmap/synthetic.hpp"

#include <algorithm>
#include <cstdio>

#include "schedule/network.hpp"

namespace reservations::seatmap {
namespace {

void AddCabin(domain::SeatMapDocument& seatmap, domain::CabinClass cabin, int first_row, int last_row,
              const std::vector<std::string>& columns, const std::vector<int>& missing_rows,
              std::int64_t base_price_minor) {
  for (int row = first_row; row <= last_row; ++row) {
    if (std::find(missing_rows.begin(), missing_rows.end(), row) != missing_rows.end()) {
      continue;
    }
    for (const auto& col : columns) {
      if (col == "aisle") {
        continue;
      }
      domain::Seat seat;
      seat.seatNumber = std::to_string(row) + col;
      seat.row = row;
      seat.column = col;
      seat.cabin = cabin;
      seat.status = domain::SeatStatus::available;
      if (col == "A" || col == columns.front()) {
        seat.attributes.position = domain::SeatPosition::window;
      } else if (col == "C" || col == "D") {
        seat.attributes.position = domain::SeatPosition::aisle;
      } else {
        seat.attributes.position = domain::SeatPosition::middle;
      }
      seat.price = domain::Money{.amountMinor = base_price_minor, .currency = "USD"};
      seatmap.seats.push_back(std::move(seat));
    }
  }
}

std::string DepartureIso(const std::string& local_date, int hour) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%sT%02d:00:00Z", local_date.c_str(), hour % 24);
  return std::string(buf);
}

std::string ArrivalIso(const std::string& local_date, int hour) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%sT%02d:00:00Z", local_date.c_str(), (hour + 3) % 24);
  return std::string(buf);
}

domain::InventoryDocument BuildLegClassInventory(const std::string& flight_id, const std::string& origin,
                                                 const std::string& destination, std::uint64_t reset_epoch,
                                                 int physical, double oversell) {
  domain::InventoryDocument inv;
  inv.type = "inventory";
  inv.flightId = flight_id;
  inv.physicalCapacity = physical;
  inv.oversellPercent = oversell;
  inv.resetEpoch = reset_epoch;

  const int econ_phys = std::max(1, physical - 8);
  const int bus_phys = std::min(8, physical / 5);
  const int econ_au = domain::AuthorizationFor(econ_phys, oversell);
  const int bus_au = domain::AuthorizationFor(bus_phys, oversell);

  domain::LegInventory leg;
  leg.legIndex = 0;
  leg.origin = origin;
  leg.destination = destination;
  leg.cabins = {
      domain::CabinCapacity{.cabin = domain::CabinClass::business,
                            .physicalCapacity = bus_phys,
                            .authorizedCapacity = bus_au,
                            .seatsSold = 0,
                            .bidPrice = domain::Money{.amountMinor = 80000, .currency = "USD"}},
      domain::CabinCapacity{.cabin = domain::CabinClass::economy,
                            .physicalCapacity = econ_phys,
                            .authorizedCapacity = econ_au,
                            .seatsSold = 0,
                            .bidPrice = domain::Money{.amountMinor = 12000, .currency = "USD"}},
  };

  struct Spec {
    const char* rbd;
    domain::CabinClass cabin;
    int limit;
  };
  const Spec specs[] = {
      {"J", domain::CabinClass::business, bus_au},
      {"C", domain::CabinClass::business, bus_au},
      {"D", domain::CabinClass::business, std::max(1, bus_au / 2)},
      {"Y", domain::CabinClass::economy, econ_au},
      {"B", domain::CabinClass::economy, std::max(1, (econ_au * 9) / 10)},
      {"M", domain::CabinClass::economy, std::max(1, (econ_au * 8) / 10)},
      {"H", domain::CabinClass::economy, std::max(1, (econ_au * 7) / 10)},
      {"Q", domain::CabinClass::economy, std::max(1, (econ_au * 5) / 10)},
      {"V", domain::CabinClass::economy, std::max(1, (econ_au * 3) / 10)},
      {"L", domain::CabinClass::economy, std::max(1, (econ_au * 2) / 10)},
  };
  for (const auto& s : specs) {
    domain::ClassControl cc;
    cc.bookingClass = s.rbd;
    cc.cabin = s.cabin;
    cc.status = domain::ClassStatus::open;
    cc.bookingLimit = s.limit;
    cc.seatsSold = 0;
    cc.seatsAvailable = s.limit;
    leg.classes.push_back(std::move(cc));
  }
  inv.legs.push_back(std::move(leg));

  // Legacy book path tracks Y.
  inv.authorization = econ_au;
  inv.sold = 0;
  return inv;
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
  SyntheticFlightBundle bundle;
  const auto& route = schedule::Routes().at(ref.route_index);
  const auto& airline = schedule::Airline(route.airline_index);
  const std::string flight_id = schedule::FlightId(ref);
  const std::string local_date = schedule::LocalDepartureDate(ref.week, ref.flight_in_week);
  const int hour = 6 + (ref.flight_in_week % 14);

  bundle.flight.flightId = flight_id;
  bundle.flight.airlineCode = std::string(airline.code);
  bundle.flight.flightNumber = std::to_string(100 + static_cast<int>(ref.route_index % 8900));
  bundle.flight.origin = std::string(route.origin);
  bundle.flight.destination = std::string(route.destination);
  bundle.flight.scheduledDeparture = DepartureIso(local_date, hour);
  bundle.flight.scheduledArrival = ArrivalIso(local_date, hour);
  bundle.flight.aircraft = domain::Aircraft{
      .icaoType = "B738",
      .name = "Boeing 737-800",
      .configurationId = "CFG-ALLIANCE-1",
  };
  bundle.flight.status = domain::FlightStatus::scheduled;
  bundle.flight.seatSelectionOpen = true;

  bundle.index.type = "flight_index";
  bundle.index.flightId = flight_id;
  bundle.index.airlineCode = bundle.flight.airlineCode;
  bundle.index.flightNumber = bundle.flight.flightNumber;
  bundle.index.departureDate = local_date;

  thread_local const domain::SeatMapDocument kSeatTemplate =
      BuildSeatMapTemplate("00000000-0000-4000-8000-000000000000", 0);
  bundle.seatmap = kSeatTemplate;
  bundle.seatmap.flightId = flight_id;
  bundle.seatmap.resetEpoch = reset_epoch;

  const int physical = TemplatePhysicalCapacity();
  bundle.inventory = BuildLegClassInventory(flight_id, bundle.flight.origin, bundle.flight.destination,
                                            reset_epoch, physical, domain::kDefaultOversellPercent);
  return bundle;
}

}  // namespace reservations::seatmap
