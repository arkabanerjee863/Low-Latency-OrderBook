#pragma once

// Conversion between external decimal prices and internal integer ticks.

#include <cmath>
#include <optional>

#include "orderbook/Types.hpp"

namespace orderbook {

[[nodiscard]] inline std::optional<Price> toTicks(double price, double tickSize) noexcept {
    if (!std::isfinite(price) || !std::isfinite(tickSize) || tickSize <= 0.0) return std::nullopt;
    const double ticks = price / tickSize;
    constexpr double kMaxExact = 9007199254740992.0; 
    if (!std::isfinite(ticks) || std::fabs(ticks) >= kMaxExact) return std::nullopt;
    const double rounded = std::nearbyint(ticks);
    if (std::fabs(ticks - rounded) > 1e-6) return std::nullopt;
    return static_cast<Price>(rounded);
}

[[nodiscard]] inline double toDecimal(Price ticks, double tickSize) noexcept {
    return static_cast<double>(ticks) * tickSize;
}

} 
