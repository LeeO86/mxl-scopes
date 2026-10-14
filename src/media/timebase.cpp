#include "media/timebase.hpp"

#include <ctime>

namespace scopes
{
std::uint64_t timestampToIndex(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t timestampNs)
{
    if (rateNumerator == 0 || rateDenominator == 0)
    {
        return UINT64_MAX;
    }
    auto const num = static_cast<__int128>(timestampNs) * static_cast<__int128>(rateNumerator) +
                     static_cast<__int128>(500000000) * static_cast<__int128>(rateDenominator);
    auto const den = static_cast<__int128>(1000000000) * static_cast<__int128>(rateDenominator);
    return static_cast<std::uint64_t>(num / den);
}

std::uint64_t indexToTimestamp(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t index)
{
    if (rateNumerator == 0 || rateDenominator == 0)
    {
        return UINT64_MAX;
    }
    auto const num = static_cast<__int128>(index) * static_cast<__int128>(rateDenominator) * static_cast<__int128>(1000000000) +
                     static_cast<__int128>(rateNumerator) / 2;
    return static_cast<std::uint64_t>(num / static_cast<__int128>(rateNumerator));
}

std::uint64_t taiNowNs()
{
    timespec ts{};
#if defined(CLOCK_TAI)
    if (clock_gettime(CLOCK_TAI, &ts) != 0)
#endif
    {
        clock_gettime(CLOCK_REALTIME, &ts);
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}
} // namespace scopes
