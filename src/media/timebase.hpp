#pragma once

#include <cstdint>

namespace scopes
{
// Same 128-bit rounding as dmf-mxl/mxl lib/internal IndexConversion.hpp at 218ddaa.
// Index 0 is the SMPTE ST 2059 epoch. A zero numerator or denominator yields UINT64_MAX.
std::uint64_t timestampToIndex(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t timestampNs);
std::uint64_t indexToTimestamp(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t index);

// CLOCK_TAI (as mxlGetTime), CLOCK_REALTIME when the kernel has no TAI clock.
std::uint64_t taiNowNs();
} // namespace scopes
