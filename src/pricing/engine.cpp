#include "pricing/engine.hpp"

#include <algorithm>
#include <cstdio>

namespace reservations::pricing {
namespace {

int DayOfYear(const std::string& ymd) {
  int y = 0, m = 0, d = 0;
  if (std::sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
    return 0;
  }
  static const int kCum[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int doy = kCum[std::clamp(m, 1, 12)] + d;
  const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
  if (leap && m > 2) {
    ++doy;
  }
  return doy + y * 366;
}

domain::DayOfWeek DowFromDate(const std::string& ymd) {
  // 2026-11-02 is Monday.
  const int days = DayOfYear(ymd) - DayOfYear("2026-11-02");
  const int idx = ((days % 7) + 7) % 7;
  static const domain::DayOfWeek kMap[] = {
      domain::DayOfWeek::MON, domain::DayOfWeek::TUE, domain::DayOfWeek::WED, domain::DayOfWeek::THU,
      domain::DayOfWeek::FRI, domain::DayOfWeek::SAT, domain::DayOfWeek::SUN};
  return kMap[idx];
}

int AdvancePurchaseDays(const domain::PricingContext& ctx) {
  return DayOfYear(ctx.outboundDepartureDate) - DayOfYear(ctx.bookingDate);
}

bool InDateRange(const std::string& date, const domain::DateRange& range) {
  if (range.from.has_value() && date < *range.from) {
    return false;
  }
  if (range.to.has_value() && date > *range.to) {
    return false;
  }
  return range.from.has_value() || range.to.has_value();
}

bool InIntRange(int value, const domain::IntRange& range) {
  if (range.min.has_value() && value < *range.min) {
    return false;
  }
  if (range.max.has_value() && value > *range.max) {
    return false;
  }
  return range.min.has_value() || range.max.has_value();
}

template <typename T>
bool PassesIncludeExclude(const std::optional<std::vector<T>>& include, const std::optional<std::vector<T>>& exclude,
                          const T& value) {
  if (include.has_value()) {
    if (std::find(include->begin(), include->end(), value) == include->end()) {
      return false;
    }
  }
  if (exclude.has_value()) {
    if (std::find(exclude->begin(), exclude->end(), value) != exclude->end()) {
      return false;
    }
  }
  return true;
}

bool LocationMatches(const domain::SalesLocation& loc, const domain::LocationPattern& pat) {
  std::string needle;
  if (pat.level == "country") {
    needle = loc.country;
  } else if (pat.level == "city") {
    needle = loc.city.value_or("");
  } else if (pat.level == "office") {
    needle = loc.officeId.value_or("");
  } else if (pat.level == "agency") {
    needle = loc.agencyId.value_or("");
  } else {
    return false;
  }
  if (pat.code == "*") {
    return !needle.empty() || pat.level == "country";
  }
  if (!pat.code.empty() && pat.code.back() == '*') {
    const auto prefix = pat.code.substr(0, pat.code.size() - 1);
    return needle.rfind(prefix, 0) == 0;
  }
  return needle == pat.code;
}

bool PassesLocation(const domain::LocationCondition& cond, const domain::SalesLocation& loc) {
  if (cond.include.has_value()) {
    bool any = false;
    for (const auto& p : *cond.include) {
      if (LocationMatches(loc, p)) {
        any = true;
        break;
      }
    }
    if (!any) {
      return false;
    }
  }
  if (cond.exclude.has_value()) {
    for (const auto& p : *cond.exclude) {
      if (LocationMatches(loc, p)) {
        return false;
      }
    }
  }
  return true;
}

bool PassesDateCond(const domain::DateRangeCondition& cond, const std::string& date) {
  if (cond.include.has_value()) {
    bool any = false;
    for (const auto& r : *cond.include) {
      if (InDateRange(date, r)) {
        any = true;
        break;
      }
    }
    if (!any) {
      return false;
    }
  }
  if (cond.exclude.has_value()) {
    for (const auto& r : *cond.exclude) {
      if (InDateRange(date, r)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool Beats(const domain::PriceRecord& a, const domain::PriceRecord& b) {
  if (a.priority != b.priority) {
    return a.priority > b.priority;
  }
  if (a.value.amountMinor != b.value.amountMinor) {
    return a.value.amountMinor > b.value.amountMinor;
  }
  const int sa = a.sequence.value_or(0);
  const int sb = b.sequence.value_or(0);
  return sa < sb;
}

bool Matches(const domain::PriceRecord& record, const domain::PricingContext& ctx) {
  if (record.effective.has_value() && !InDateRange(ctx.bookingDate, *record.effective)) {
    return false;
  }
  const auto& c = record.conditions;
  if (c.salesChannels.has_value() &&
      !PassesIncludeExclude(c.salesChannels->include, c.salesChannels->exclude, ctx.salesChannel)) {
    return false;
  }
  if (c.salesLocations.has_value() && !PassesLocation(*c.salesLocations, ctx.salesLocation)) {
    return false;
  }
  if (c.travelDates.has_value() && !PassesDateCond(*c.travelDates, ctx.outboundDepartureDate)) {
    return false;
  }
  if (c.returnTravelDates.has_value()) {
    if (!ctx.returnDepartureDate.has_value()) {
      return false;
    }
    if (!PassesDateCond(*c.returnTravelDates, *ctx.returnDepartureDate)) {
      return false;
    }
  }
  if (c.bookingDates.has_value() && !PassesDateCond(*c.bookingDates, ctx.bookingDate)) {
    return false;
  }
  if (c.departureDaysOfWeek.has_value()) {
    const auto dow = DowFromDate(ctx.outboundDepartureDate);
    if (!PassesIncludeExclude(c.departureDaysOfWeek->include, c.departureDaysOfWeek->exclude, dow)) {
      return false;
    }
  }
  if (c.advancePurchaseDays.has_value()) {
    const int ap = AdvancePurchaseDays(ctx);
    const auto& apc = *c.advancePurchaseDays;
    if (apc.include.has_value() && !InIntRange(ap, *apc.include)) {
      return false;
    }
    if (apc.exclude.has_value() && InIntRange(ap, *apc.exclude)) {
      return false;
    }
  }
  if (c.tripTypes.has_value() &&
      !PassesIncludeExclude(c.tripTypes->include, c.tripTypes->exclude, ctx.tripType)) {
    return false;
  }
  if (c.passengerTypes.has_value() &&
      !PassesIncludeExclude(c.passengerTypes->include, c.passengerTypes->exclude, ctx.passengerType)) {
    return false;
  }
  return true;
}

std::optional<domain::PriceRecord> SelectBest(const std::vector<domain::PriceRecord>& records,
                                              const domain::PricingContext& ctx,
                                              std::string_view booking_class, int* evaluated,
                                              int* matched) {
  int ev = 0;
  int mt = 0;
  std::optional<domain::PriceRecord> best;
  for (const auto& rec : records) {
    if (rec.bookingClass != booking_class) {
      continue;
    }
    ++ev;
    if (!Matches(rec, ctx)) {
      continue;
    }
    ++mt;
    if (!best.has_value() || Beats(rec, *best)) {
      best = rec;
    }
  }
  if (evaluated) {
    *evaluated = ev;
  }
  if (matched) {
    *matched = mt;
  }
  return best;
}

std::vector<domain::PriceRecord> GenerateMarketPrices(std::string_view origin, std::string_view destination,
                                                      std::string_view carrier, int record_count,
                                                      std::string_view snapshot_id) {
  static constexpr const char* kClasses[] = {"Y", "B", "M", "H", "Q", "V", "W", "S", "T", "L",
                                             "K", "G", "N", "J", "C", "D", "I", "F", "A", "P"};
  static constexpr domain::CabinClass kCabins[] = {
      domain::CabinClass::economy, domain::CabinClass::economy, domain::CabinClass::economy,
      domain::CabinClass::economy, domain::CabinClass::economy, domain::CabinClass::economy,
      domain::CabinClass::premium_economy, domain::CabinClass::premium_economy,
      domain::CabinClass::economy, domain::CabinClass::economy, domain::CabinClass::economy,
      domain::CabinClass::economy, domain::CabinClass::economy, domain::CabinClass::business,
      domain::CabinClass::business, domain::CabinClass::business, domain::CabinClass::business,
      domain::CabinClass::first, domain::CabinClass::first, domain::CabinClass::first};

  std::vector<domain::PriceRecord> out;
  out.reserve(static_cast<std::size_t>(record_count));
  for (int i = 0; i < record_count; ++i) {
    domain::PriceRecord rec;
    char id[80];
    std::snprintf(id, sizeof(id), "%.*s-%.*s-%.*s-%05d", static_cast<int>(snapshot_id.size()),
                  snapshot_id.data(), static_cast<int>(origin.size()), origin.data(),
                  static_cast<int>(destination.size()), destination.data(), i);
    rec.recordId = id;
    rec.carrier = std::string(carrier);
    const int ci = i % 20;
    rec.bookingClass = kClasses[ci];
    rec.cabin = kCabins[ci];
    rec.fareBasis = rec.bookingClass + "OWFLEX";
    rec.value = domain::Money{.amountMinor = 5000 + (i % 97) * 1000 + ci * 2500, .currency = "USD"};
    rec.priority = 1000 - (ci * 30) + (i % 17);
    rec.sequence = i;
    rec.effective = domain::DateRange{.from = "2026-01-01", .to = "2027-12-31"};

    // Mix of wildcards and constrained records so requests match a subset.
    if (i % 5 == 0) {
      rec.conditions.salesChannels = domain::ChannelCondition{
          .include = std::vector<domain::SalesChannel>{domain::SalesChannel::direct_web,
                                                       domain::SalesChannel::ndc_api,
                                                       domain::SalesChannel::travel_agency}};
    }
    if (i % 7 == 0) {
      rec.conditions.tripTypes = domain::TripTypeCondition{
          .include = std::vector<domain::TripType>{domain::TripType::one_way, domain::TripType::round_trip}};
    }
    if (i % 11 == 0) {
      rec.conditions.advancePurchaseDays = domain::AdvancePurchaseCondition{
          .include = domain::IntRange{.min = 0, .max = 14 + (i % 60)}};
    }
    if (i % 13 == 0) {
      rec.conditions.passengerTypes = domain::PassengerTypeCondition{
          .include = std::vector<domain::PassengerType>{domain::PassengerType::adult,
                                                        domain::PassengerType::child}};
    }
    if (i % 17 == 0) {
      rec.conditions.salesLocations = domain::LocationCondition{
          .include = std::vector<domain::LocationPattern>{
              {.level = "country", .code = "US"},
              {.level = "country", .code = "GB"},
              {.level = "country", .code = "DE"},
              {.level = "country", .code = "*"},
          }};
    }
    out.push_back(std::move(rec));
  }
  return out;
}

}  // namespace reservations::pricing
