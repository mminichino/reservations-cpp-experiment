#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reservations::schedule {

enum class Alliance { oneworld, star };

struct AirlineInfo {
  std::string_view code;  // IATA
  std::string_view name;
  Alliance alliance;
  std::string_view primary_hub;  // IATA airport
};

struct AirportInfo {
  std::string_view code;
  std::string_view city;
  std::string_view country;  // ISO 3166-1 alpha-2
};

/// Directed nonstop route operated by one alliance carrier (realistic hub/spoke + key trunks).
struct RouteInfo {
  int airline_index;
  std::string_view origin;
  std::string_view destination;
  int weekly_frequency;  // flights per week on this directed route
};

/// OneWorld + Star Alliance majors used by the experiment.
inline constexpr std::array<AirlineInfo, 20> kAirlines{{
    {"AA", "American Airlines", Alliance::oneworld, "DFW"},
    {"BA", "British Airways", Alliance::oneworld, "LHR"},
    {"CX", "Cathay Pacific", Alliance::oneworld, "HKG"},
    {"QF", "Qantas", Alliance::oneworld, "SYD"},
    {"QR", "Qatar Airways", Alliance::oneworld, "DOH"},
    {"IB", "Iberia", Alliance::oneworld, "MAD"},
    {"AY", "Finnair", Alliance::oneworld, "HEL"},
    {"JL", "Japan Airlines", Alliance::oneworld, "HND"},
    {"AS", "Alaska Airlines", Alliance::oneworld, "SEA"},
    {"RJ", "Royal Jordanian", Alliance::oneworld, "AMM"},
    {"UA", "United Airlines", Alliance::star, "ORD"},
    {"LH", "Lufthansa", Alliance::star, "FRA"},
    {"AC", "Air Canada", Alliance::star, "YYZ"},
    {"NH", "ANA", Alliance::star, "HND"},
    {"SQ", "Singapore Airlines", Alliance::star, "SIN"},
    {"TK", "Turkish Airlines", Alliance::star, "IST"},
    {"LX", "SWISS", Alliance::star, "ZRH"},
    {"OS", "Austrian", Alliance::star, "VIE"},
    {"SK", "SAS", Alliance::star, "ARN"},
    {"NZ", "Air New Zealand", Alliance::star, "AKL"},
}};

inline constexpr std::array<AirportInfo, 48> kAirports{{
    {"DFW", "Dallas", "US"},   {"ORD", "Chicago", "US"},  {"LAX", "Los Angeles", "US"},
    {"JFK", "New York", "US"}, {"MIA", "Miami", "US"},    {"SEA", "Seattle", "US"},
    {"SFO", "San Francisco", "US"}, {"BOS", "Boston", "US"}, {"PHX", "Phoenix", "US"},
    {"CLT", "Charlotte", "US"}, {"IAH", "Houston", "US"}, {"DEN", "Denver", "US"},
    {"EWR", "Newark", "US"},   {"IAD", "Washington", "US"}, {"YYZ", "Toronto", "CA"},
    {"YVR", "Vancouver", "CA"}, {"YUL", "Montreal", "CA"}, {"LHR", "London", "GB"},
    {"LGW", "London", "GB"},   {"MAN", "Manchester", "GB"}, {"FRA", "Frankfurt", "DE"},
    {"MUC", "Munich", "DE"},   {"ZRH", "Zurich", "CH"},  {"VIE", "Vienna", "AT"},
    {"MAD", "Madrid", "ES"},   {"BCN", "Barcelona", "ES"}, {"HEL", "Helsinki", "FI"},
    {"ARN", "Stockholm", "SE"}, {"CPH", "Copenhagen", "DK"}, {"IST", "Istanbul", "TR"},
    {"DOH", "Doha", "QA"},     {"DXB", "Dubai", "AE"},   {"HKG", "Hong Kong", "HK"},
    {"HND", "Tokyo", "JP"},    {"NRT", "Tokyo", "JP"},   {"KIX", "Osaka", "JP"},
    {"SIN", "Singapore", "SG"}, {"SYD", "Sydney", "AU"}, {"MEL", "Melbourne", "AU"},
    {"AKL", "Auckland", "NZ"}, {"CDG", "Paris", "FR"},   {"AMS", "Amsterdam", "NL"},
    {"FCO", "Rome", "IT"},     {"MXP", "Milan", "IT"},       {"GRU", "Sao Paulo", "BR"},
    {"GIG", "Rio de Janeiro", "BR"}, {"PVG", "Shanghai", "CN"},
}};

/// Compact realistic route table: domestic spokes into hubs + alliance trunks + cross-hub links.
/// Indices into kAirlines. weekly_frequency kept modest so full materialization stays feasible.
[[nodiscard]] inline const std::vector<RouteInfo>& Routes() {
  static const std::vector<RouteInfo> kRoutes = [] {
    std::vector<RouteInfo> r;
    auto add = [&](int airline, std::string_view o, std::string_view d, int freq) {
      r.push_back(RouteInfo{airline, o, d, freq});
      r.push_back(RouteInfo{airline, d, o, freq});  // return
    };

    // American (0) — DFW / CLT / MIA / ORD / LAX / JFK / PHX / LHR / MAD / DOH
    add(0, "DFW", "LAX", 21);
    add(0, "DFW", "ORD", 21);
    add(0, "DFW", "JFK", 14);
    add(0, "DFW", "MIA", 21);
    add(0, "DFW", "PHX", 14);
    add(0, "CLT", "JFK", 14);
    add(0, "CLT", "MIA", 14);
    add(0, "JFK", "LHR", 14);
    add(0, "JFK", "MAD", 7);
    add(0, "DFW", "LHR", 7);
    add(0, "MIA", "MAD", 7);
    add(0, "ORD", "LHR", 7);

    // British Airways (1) — LHR
    add(1, "LHR", "JFK", 21);
    add(1, "LHR", "ORD", 14);
    add(1, "LHR", "LAX", 14);
    add(1, "LHR", "SFO", 7);
    add(1, "LHR", "MAD", 21);
    add(1, "LHR", "HEL", 14);
    add(1, "LHR", "DOH", 7);
    add(1, "LHR", "SIN", 7);
    add(1, "LHR", "SYD", 7);
    add(1, "LHR", "HKG", 7);
    add(1, "LGW", "BCN", 14);

    // Cathay (2) — HKG
    add(2, "HKG", "NRT", 14);
    add(2, "HKG", "SIN", 14);
    add(2, "HKG", "SYD", 14);
    add(2, "HKG", "LAX", 7);
    add(2, "HKG", "LHR", 7);
    add(2, "HKG", "DOH", 7);

    // Qantas (3) — SYD
    add(3, "SYD", "MEL", 28);
    add(3, "SYD", "AKL", 14);
    add(3, "SYD", "LAX", 14);
    add(3, "SYD", "SIN", 14);
    add(3, "SYD", "HKG", 7);
    add(3, "SYD", "LHR", 7);
    add(3, "MEL", "SIN", 7);

    // Qatar (4) — DOH
    add(4, "DOH", "LHR", 21);
    add(4, "DOH", "JFK", 14);
    add(4, "DOH", "FRA", 14);
    add(4, "DOH", "SIN", 14);
    add(4, "DOH", "HKG", 14);
    add(4, "DOH", "SYD", 7);
    add(4, "DOH", "MAD", 7);
    add(4, "DOH", "IST", 14);

    // Iberia (5) — MAD
    add(5, "MAD", "BCN", 28);
    add(5, "MAD", "LHR", 21);
    add(5, "MAD", "JFK", 14);
    add(5, "MAD", "MIA", 7);
    add(5, "MAD", "GRU", 7);

    // Finnair (6) — HEL
    add(6, "HEL", "LHR", 14);
    add(6, "HEL", "FRA", 14);
    add(6, "HEL", "NRT", 7);
    add(6, "HEL", "HKG", 7);

    // JAL (7) — HND/NRT
    add(7, "HND", "NRT", 7);
    add(7, "HND", "KIX", 21);
    add(7, "NRT", "LAX", 14);
    add(7, "NRT", "SFO", 7);
    add(7, "NRT", "JFK", 7);
    add(7, "NRT", "LHR", 7);
    add(7, "HND", "SIN", 7);

    // Alaska (8) — SEA
    add(8, "SEA", "LAX", 21);
    add(8, "SEA", "SFO", 21);
    add(8, "SEA", "ORD", 14);
    add(8, "SEA", "JFK", 7);
    add(8, "SEA", "DFW", 7);

    // Royal Jordanian (9) — AMM
    add(9, "AMM", "LHR", 7);
    add(9, "AMM", "FRA", 7);
    add(9, "AMM", "IST", 7);

    // United (10) — ORD / EWR / IAH / SFO / DEN / IAD
    add(10, "ORD", "LAX", 21);
    add(10, "ORD", "SFO", 21);
    add(10, "ORD", "EWR", 21);
    add(10, "ORD", "DEN", 21);
    add(10, "ORD", "IAH", 14);
    add(10, "EWR", "LHR", 14);
    add(10, "EWR", "FRA", 14);
    add(10, "SFO", "NRT", 14);
    add(10, "SFO", "SIN", 7);
    add(10, "IAH", "FRA", 7);
    add(10, "IAD", "FRA", 14);
    add(10, "DEN", "LAX", 14);

    // Lufthansa (11) — FRA / MUC
    add(11, "FRA", "MUC", 28);
    add(11, "FRA", "LHR", 21);
    add(11, "FRA", "JFK", 14);
    add(11, "FRA", "ORD", 14);
    add(11, "FRA", "SFO", 7);
    add(11, "FRA", "SIN", 7);
    add(11, "FRA", "NRT", 7);
    add(11, "FRA", "IST", 14);
    add(11, "MUC", "LHR", 14);
    add(11, "MUC", "JFK", 7);

    // Air Canada (12) — YYZ / YVR
    add(12, "YYZ", "YVR", 21);
    add(12, "YYZ", "YUL", 21);
    add(12, "YYZ", "LHR", 14);
    add(12, "YYZ", "FRA", 14);
    add(12, "YYZ", "ORD", 14);
    add(12, "YVR", "LAX", 14);
    add(12, "YVR", "NRT", 7);

    // ANA (13) — HND/NRT
    add(13, "HND", "KIX", 21);
    add(13, "NRT", "LAX", 14);
    add(13, "NRT", "SFO", 7);
    add(13, "NRT", "ORD", 7);
    add(13, "NRT", "FRA", 7);
    add(13, "HND", "SIN", 7);

    // Singapore (14) — SIN
    add(14, "SIN", "HKG", 21);
    add(14, "SIN", "SYD", 14);
    add(14, "SIN", "NRT", 14);
    add(14, "SIN", "LHR", 14);
    add(14, "SIN", "FRA", 14);
    add(14, "SIN", "SFO", 7);
    add(14, "SIN", "LAX", 7);

    // Turkish (15) — IST
    add(15, "IST", "LHR", 21);
    add(15, "IST", "FRA", 21);
    add(15, "IST", "JFK", 14);
    add(15, "IST", "ORD", 7);
    add(15, "IST", "SIN", 7);
    add(15, "IST", "DOH", 14);

    // SWISS (16) — ZRH
    add(16, "ZRH", "LHR", 21);
    add(16, "ZRH", "FRA", 21);
    add(16, "ZRH", "JFK", 14);
    add(16, "ZRH", "ORD", 7);
    add(16, "ZRH", "SIN", 7);

    // Austrian (17) — VIE
    add(17, "VIE", "FRA", 21);
    add(17, "VIE", "LHR", 14);
    add(17, "VIE", "IST", 14);
    add(17, "VIE", "JFK", 7);

    // SAS (18) — ARN / CPH
    add(18, "ARN", "CPH", 21);
    add(18, "ARN", "LHR", 14);
    add(18, "ARN", "FRA", 14);
    add(18, "CPH", "LHR", 14);
    add(18, "CPH", "EWR", 7);

    // Air New Zealand (19) — AKL
    add(19, "AKL", "SYD", 21);
    add(19, "AKL", "MEL", 14);
    add(19, "AKL", "LAX", 7);
    add(19, "AKL", "SFO", 7);
    add(19, "AKL", "SIN", 7);

    return r;
  }();
  return kRoutes;
}

[[nodiscard]] inline std::size_t RouteCount() { return Routes().size(); }

/// Connection hubs preferred when building one-stop journeys.
inline constexpr std::array<std::string_view, 16> kConnectionHubs{{
    "LHR", "FRA", "DOH", "IST", "SIN", "HKG", "ORD", "DFW",
    "JFK", "LAX", "YYZ", "MAD", "ZRH", "NRT", "SYD", "SEA",
}};

[[nodiscard]] inline const AirlineInfo& Airline(int index) {
  return kAirlines.at(static_cast<std::size_t>(index));
}

[[nodiscard]] inline int AirlineCount() { return static_cast<int>(kAirlines.size()); }

}  // namespace reservations::schedule
