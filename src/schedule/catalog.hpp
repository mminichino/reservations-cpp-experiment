#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "domain/types.hpp"
#include "schedule/network.hpp"

namespace reservations::schedule {

inline constexpr int kWeeksOut = 52;

/// Price records generated per O&D market (synthetic density for the availability engine).
inline constexpr int kPriceRecordsPerMarket = 2000;

[[nodiscard]] inline std::uint64_t FlightsOnRoute(std::size_t route_index) {
  const auto& route = Routes().at(route_index);
  return static_cast<std::uint64_t>(route.weekly_frequency) * static_cast<std::uint64_t>(kWeeksOut);
}

[[nodiscard]] inline std::uint64_t TotalFlights() {
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < RouteCount(); ++i) {
    total += FlightsOnRoute(i);
  }
  return total;
}

struct CatalogMeta {
  std::string type{"catalog_meta"};
  std::uint64_t resetEpoch{1};
  int airlineCount{0};
  int routeCount{0};
  int weeksOut{kWeeksOut};
  int priceRecordsPerMarket{kPriceRecordsPerMarket};
  double oversellPercent{domain::kDefaultOversellPercent};
  int physicalCapacityPerFlight{0};
  int authorizationPerFlight{0};
  std::string priceSnapshotId;
};

struct FlightRef {
  std::size_t route_index{0};
  int week{0};             // [0, kWeeksOut)
  int flight_in_week{0};   // [0, weekly_frequency)

  [[nodiscard]] std::uint64_t GlobalIndex() const {
    std::uint64_t base = 0;
    for (std::size_t i = 0; i < route_index; ++i) {
      base += FlightsOnRoute(i);
    }
    return base + static_cast<std::uint64_t>(week) * static_cast<std::uint64_t>(Routes().at(route_index).weekly_frequency) +
           static_cast<std::uint64_t>(flight_in_week);
  }

  [[nodiscard]] static FlightRef FromGlobalIndex(std::uint64_t index) {
    FlightRef ref;
    std::uint64_t cursor = index;
    for (std::size_t i = 0; i < RouteCount(); ++i) {
      const std::uint64_t n = FlightsOnRoute(i);
      if (cursor < n) {
        ref.route_index = i;
        const int freq = Routes().at(i).weekly_frequency;
        ref.week = static_cast<int>(cursor / static_cast<std::uint64_t>(freq));
        ref.flight_in_week = static_cast<int>(cursor % static_cast<std::uint64_t>(freq));
        return ref;
      }
      cursor -= n;
    }
    ref.route_index = RouteCount() > 0 ? RouteCount() - 1 : 0;
    return ref;
  }
};

[[nodiscard]] inline std::string AirlineCodeFor(const FlightRef& ref) {
  return std::string(Airline(Routes().at(ref.route_index).airline_index).code);
}

[[nodiscard]] inline std::string FlightId(const FlightRef& ref) {
  const std::uint64_t idx = ref.GlobalIndex();
  char buf[37];
  std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012llx",
                static_cast<unsigned long long>(idx % 0x1000000000000ULL));
  return std::string(buf);
}

[[nodiscard]] inline std::string LocalDepartureDate(int week, int flight_in_week) {
  const int day_offset = week * 7 + (flight_in_week % 7);
  int y = 2026;
  int m = 11;
  int d = 2 + day_offset;
  static const int kDaysInMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto leap = [](int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); };
  while (true) {
    int dim = kDaysInMonth[m];
    if (m == 2 && leap(y)) {
      dim = 29;
    }
    if (d <= dim) {
      break;
    }
    d -= dim;
    ++m;
    if (m > 12) {
      m = 1;
      ++y;
    }
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
  return std::string(buf);
}

[[nodiscard]] inline std::string MarketKey(std::string_view origin, std::string_view destination) {
  return std::string(origin) + "::" + std::string(destination);
}

[[nodiscard]] inline std::string OdDateIndexKey(std::string_view origin, std::string_view destination,
                                                std::string_view departure_date) {
  return "od::" + std::string(origin) + "::" + std::string(destination) + "::" + std::string(departure_date);
}

inline constexpr const char* kMetaCollection = "meta";
inline constexpr const char* kCatalogMetaKey = "catalog_meta";
inline constexpr const char* kMarketsCollection = "markets";
inline constexpr const char* kPricesCollection = "prices";
inline constexpr const char* kOdIndexCollection = "od_index";

}  // namespace reservations::schedule
