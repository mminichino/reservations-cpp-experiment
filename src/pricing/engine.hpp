#pragma once

#include <optional>
#include <string>
#include <vector>

#include "domain/pricing.hpp"

namespace reservations::pricing {

/// Default precedence: higher priority, then higher value, then lower sequence.
[[nodiscard]] bool Beats(const domain::PriceRecord& a, const domain::PriceRecord& b);

[[nodiscard]] bool Matches(const domain::PriceRecord& record, const domain::PricingContext& ctx);

[[nodiscard]] std::optional<domain::PriceRecord> SelectBest(const std::vector<domain::PriceRecord>& records,
                                                            const domain::PricingContext& ctx,
                                                            std::string_view booking_class,
                                                            int* evaluated = nullptr,
                                                            int* matched = nullptr);

/// Deterministic synthetic price set for a market (full candidate set for one snapshot).
[[nodiscard]] std::vector<domain::PriceRecord> GenerateMarketPrices(std::string_view origin,
                                                                    std::string_view destination,
                                                                    std::string_view carrier,
                                                                    int record_count,
                                                                    std::string_view snapshot_id);

}  // namespace reservations::pricing
