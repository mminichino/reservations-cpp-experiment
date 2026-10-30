#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "types.hpp"

namespace reservations::domain {

enum class SalesChannel {
  direct_web,
  direct_mobile,
  call_center,
  airport,
  travel_agency,
  corporate,
  ndc_api,
  ota,
  kiosk
};

enum class TripType { one_way, round_trip, open_jaw, circle_trip };

enum class DayOfWeek { MON, TUE, WED, THU, FRI, SAT, SUN };

struct Market {
  std::string origin;
  std::string destination;
};

struct SalesLocation {
  std::string country;
  std::optional<std::string> city;
  std::optional<std::string> officeId;
  std::optional<std::string> agencyId;
};

struct DateRange {
  std::optional<std::string> from;
  std::optional<std::string> to;
};

struct IntRange {
  std::optional<int> min;
  std::optional<int> max;
};

struct LocationPattern {
  std::string level;  // region|country|city|office|agency
  std::string code;
};

struct ChannelCondition {
  std::optional<std::vector<SalesChannel>> include;
  std::optional<std::vector<SalesChannel>> exclude;
};

struct LocationCondition {
  std::optional<std::vector<LocationPattern>> include;
  std::optional<std::vector<LocationPattern>> exclude;
};

struct DateRangeCondition {
  std::optional<std::vector<DateRange>> include;
  std::optional<std::vector<DateRange>> exclude;
};

struct DayOfWeekCondition {
  std::optional<std::vector<DayOfWeek>> include;
  std::optional<std::vector<DayOfWeek>> exclude;
};

struct AdvancePurchaseCondition {
  std::optional<IntRange> include;
  std::optional<IntRange> exclude;
};

struct TripTypeCondition {
  std::optional<std::vector<TripType>> include;
  std::optional<std::vector<TripType>> exclude;
};

struct PassengerTypeCondition {
  std::optional<std::vector<PassengerType>> include;
  std::optional<std::vector<PassengerType>> exclude;
};

struct AdditionalDimensionCondition {
  std::string name;
  std::optional<std::vector<std::string>> include;
  std::optional<std::vector<std::string>> exclude;
};

struct PriceConditions {
  std::optional<ChannelCondition> salesChannels;
  std::optional<LocationCondition> salesLocations;
  std::optional<DateRangeCondition> travelDates;
  std::optional<DateRangeCondition> returnTravelDates;
  std::optional<DateRangeCondition> bookingDates;
  std::optional<DayOfWeekCondition> departureDaysOfWeek;
  std::optional<AdvancePurchaseCondition> advancePurchaseDays;
  std::optional<TripTypeCondition> tripTypes;
  std::optional<PassengerTypeCondition> passengerTypes;
  std::optional<std::vector<AdditionalDimensionCondition>> additionalDimensions;
};

struct PriceRecord {
  std::string recordId;
  std::string carrier;
  std::string bookingClass;
  std::optional<CabinClass> cabin;
  std::optional<std::string> fareBasis;
  Money value{};
  int priority{0};
  std::optional<int> sequence;
  std::optional<DateRange> effective;
  PriceConditions conditions{};
};

struct PricingContext {
  SalesChannel salesChannel{SalesChannel::direct_web};
  SalesLocation salesLocation{};
  std::string bookingDate;
  TripType tripType{TripType::one_way};
  PassengerType passengerType{PassengerType::adult};
  std::string outboundDepartureDate;
  std::optional<std::string> returnDepartureDate;
};

/// Persisted page of market prices (one Couchbase doc ≈ one page).
struct MarketPricePageDocument {
  std::string type{"market_prices"};
  std::string marketOrigin;
  std::string marketDestination;
  std::string snapshotId;
  int pageNumber{1};
  int pageCount{1};
  std::vector<PriceRecord> records;
  std::uint64_t resetEpoch{0};
};

struct MarketMetaDocument {
  std::string type{"market_meta"};
  std::string origin;
  std::string destination;
  std::string snapshotId;
  int totalRecords{0};
  int pageCount{0};
  int pageSize{500};
  std::uint64_t resetEpoch{0};
};

/// Index of flight ids for one O&D local date (direct services).
struct OdFlightIndexDocument {
  std::string type{"od_index"};
  std::string origin;
  std::string destination;
  std::string departureDate;
  std::vector<std::string> flightIds;
  std::uint64_t resetEpoch{0};
};

}  // namespace reservations::domain
