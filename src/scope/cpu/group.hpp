#pragma once

#include "scope/analyse.hpp"

namespace scopes::cpu
{
// The reference step for one v210 group: every pixelStep-th luma sample inside the width,
// odd x into the scratch sub-histogram when the plan has two. The AVX2 engine uses it for
// the groups it does not take in pairs, so both engines share the exact rule.
inline void analyseGroup(AnalysePlan const& plan, std::uint8_t const* row, int g, std::uint16_t* counts, std::uint16_t* scratch)
{
    std::uint16_t y[6];
    unpackGroupLuma(row + 16 * static_cast<std::size_t>(g), y);
    int const x0 = 6 * g;
    for (int i = 0; i < 6; i += plan.pixelStep)
    {
        int const x = x0 + i;
        if (x >= plan.width)
        {
            break;
        }
        auto const idx = plan.rowOffset[y[i]] + plan.columnOf[static_cast<std::size_t>(x)];
        if (plan.twoSubs && (i & 1) != 0)
        {
            ++scratch[idx];
        }
        else
        {
            ++counts[idx];
        }
    }
}

// Folds the odd-x sub-histogram of a strip into the counts and clears it.
inline void foldStrip(AnalysePlan const& plan, AnalysePlan::Strip const& strip, std::uint16_t* counts, std::uint16_t* scratch)
{
    for (int b = 0; b < plan.bins; ++b)
    {
        auto const row = static_cast<std::size_t>(b) * static_cast<std::size_t>(plan.columns);
        for (auto i = row + static_cast<std::size_t>(strip.col0); i <= row + static_cast<std::size_t>(strip.col1); ++i)
        {
            counts[i] = static_cast<std::uint16_t>(counts[i] + scratch[i]);
            scratch[i] = 0;
        }
    }
}

// Starts loading the strip's bytes of a row a few rows ahead (kPrefetchRows).
inline void prefetchStrip(AnalysePlan const& plan, AnalysePlan::Strip const& strip, std::uint8_t const* v210, int y)
{
    int const ahead = y + kPrefetchRows * plan.lineStep;
    if (ahead >= plan.height)
    {
        return;
    }
    auto const* row = v210 + static_cast<std::size_t>(ahead) * plan.stride;
    for (int byte = 16 * strip.g0; byte < 16 * strip.g1; byte += 64)
    {
        __builtin_prefetch(row + byte);
    }
    __builtin_prefetch(row + 16 * strip.g1 - 1);
}
} // namespace scopes::cpu
