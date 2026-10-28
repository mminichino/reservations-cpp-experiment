#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "domain/types.hpp"

namespace reservations::schedule {

/// Scale assumptions for the experiment.
inline constexpr int kAirlineCount = 10;
inline constexpr int kFlightsPerWeekPerAirline = 28000;
inline constexpr int kWeeksOut = 52;

[[nodiscard]] inline constexpr std::uint64_t FlightsPerAirline() {
  return static_cast<std::uint64_t>(kFlightsPerWeekPerAirline) * static_cast<std::uint64_t>(kWeeksOut);
}

[[nodiscard]] inline constexpr std::uint64_t TotalFlights() {
  return static_cast<std::uint64_t>(kAirlineCount) * FlightsPerAirline();
}

/// Cluster-wide reset generation. Bumping this makes all prior inventory/seatmaps stale
/// (treated as fully open) without rewriting millions of documents.
struct CatalogMeta {
  std::string type{"catalog_meta"};
  std::uint64_t resetEpoch{1};
  int airlines{kAirlineCount};
  int flightsPerWeekPerAirline{kFlightsPerWeekPerAirline};
  int weeksOut{kWeeksOut};
  double oversellPercent{domain::kDefaultOversellPercent};
  int physicalCapacityPerFlight{0};  // filled when template is known
  int authorizationPerFlight{0};
};

struct FlightRef {
  int airline_index{0};     // [0, kAirlineCount)
  int week{0};              // [0, kWeeksOut)
  int flight_in_week{0};    // [0, kFlightsPerWeekPerAirline)

  [[nodiscard]] constexpr std::uint64_t GlobalIndex() const {
    return static_cast<std::uint64_t>(airline_index) * FlightsPerAirline() +
           static_cast<std::uint64_t>(week) * static_cast<std::uint64_t>(kFlightsPerWeekPerAirline) +
           static_cast<std::uint64_t>(flight_in_week);
  }

  [[nodiscard]] static FlightRef FromGlobalIndex(std::uint64_t index) {
    FlightRef ref;
    ref.airline_index = static_cast<int>(index / FlightsPerAirline());
    const std::uint64_t rem = index % FlightsPerAirline();
    ref.week = static_cast<int>(rem / static_cast<std::uint64_t>(kFlightsPerWeekPerAirline));
    ref.flight_in_week = static_cast<int>(rem % static_cast<std::uint64_t>(kFlightsPerWeekPerAirline));
    return ref;
  }
};

[[nodiscard]] inline std::string AirlineCode(int airline_index) {
  // Schema: ^[A-Z0-9]{2}$ — X0 .. X9
  char buf[3] = {'X', static_cast<char>('0' + (airline_index % 10)), '\0'};
  return std::string(buf);
}

/// Deterministic UUID (v4-shaped) from the global flight index.
[[nodiscard]] inline std::string FlightId(const FlightRef& ref) {
  const std::uint64_t idx = ref.GlobalIndex();
  char buf[37];
  // 00000000-0000-4000-8000-XXXXXXXXXXXX (12 hex digits from index)
  std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012llx",
                static_cast<unsigned long long>(idx % 0x1000000000000ULL));
  return std::string(buf);
}

[[nodiscard]] inline std::string LocalDepartureDate(int week, int flight_in_week) {
  // Spread departures across the week starting 2026-11-02 (Mon) as week 0.
  const int day_offset = week * 7 + (flight_in_week % 7);
  // Civil date from days since 2026-11-02 using a simple month table for 2026-2027.
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

inline constexpr const char* kMetaCollection = "meta";
inline constexpr const char* kCatalogMetaKey = "catalog_meta";

}  // namespace reservations::schedule
