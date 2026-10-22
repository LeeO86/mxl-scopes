#pragma once

#include "scope/analyse.hpp"

namespace scopes::cpu
{
// These helpers are `static`: they are compiled into the AVX2 and AVX-512 units with their
// flags, and an inline function with external linkage could be emitted there and then picked
// by the linker for the baseline code (SIGILL on an older CPU in a build that does not inline).
// The reference step for one v210 group: every pixelStep-th luma sample inside the width,
// odd x into the scratch sub-histogram when the plan has two. The AVX2 engine uses it for
// the groups it does not take in pairs, so both engines share the exact rule.
template <class C>
static inline void analyseGroup(AnalysePlan const& plan, std::uint8_t const* row, int g, C* counts, C* scratch)
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

// The four 32-bit words of one v210 group (little endian).
static inline void groupWords(std::uint8_t const* group, std::uint32_t w[4])
{
    for (int i = 0; i < 4; ++i)
    {
        w[i] = static_cast<std::uint32_t>(group[4 * i]) | (static_cast<std::uint32_t>(group[4 * i + 1]) << 8) |
               (static_cast<std::uint32_t>(group[4 * i + 2]) << 16) | (static_cast<std::uint32_t>(group[4 * i + 3]) << 24);
    }
}

// The reference step of `Ycbcr`: luma as analyseGroup into plane 0; chroma sample k (x = 6g + 2k)
// when its co-sited luma sample is taken, Cb into plane 1 and Cr into plane 2, odd k into the
// scratch sub-histogram when the plan has two.
template <class C>
static inline void analyseGroupYcbcr(AnalysePlan const& plan, std::uint8_t const* row, int g, C* counts, C* scratch)
{
    std::uint32_t w[4];
    groupWords(row + 16 * static_cast<std::size_t>(g), w);
    auto const f = [&](int word, int shift) { return static_cast<std::uint16_t>((w[word] >> shift) & 0x3ffu); };
    std::uint16_t const y[6] = {f(0, 10), f(1, 0), f(1, 20), f(2, 10), f(3, 0), f(3, 20)};
    std::uint16_t const cb[3] = {f(0, 0), f(1, 10), f(2, 20)};
    std::uint16_t const cr[3] = {f(0, 20), f(2, 0), f(3, 10)};
    auto const cbPlane = plan.planeCells;
    auto const crPlane = 2 * plan.planeCells;
    int const x0 = 6 * g;
    for (int i = 0; i < 6; i += plan.pixelStep)
    {
        int const x = x0 + i;
        if (x >= plan.width)
        {
            break;
        }
        auto const column = plan.columnOf[static_cast<std::size_t>(x)];
        auto* target = plan.twoSubs && (i & 1) != 0 ? scratch : counts;
        ++target[plan.rowOffset[y[i]] + column];
        if ((i & 1) == 0)
        {
            int const k = i / 2;
            auto* chroma = plan.twoSubs && (k & 1) != 0 ? scratch : counts;
            ++chroma[cbPlane + plan.rowOffset[cb[k]] + column];
            ++chroma[crPlane + plan.rowOffset[cr[k]] + column];
        }
    }
}

// The reference step of `Rgb`: every taken luma sample with its co-sited chroma pair (k = i / 2)
// gives R′, G′, B′ (§4.8), clamped to 0..1023 for the bins, into planes 0, 1, 2.
template <class C>
static inline void analyseGroupRgb(AnalysePlan const& plan, std::uint8_t const* row, int g, C* counts, C* scratch)
{
    std::uint32_t w[4];
    groupWords(row + 16 * static_cast<std::size_t>(g), w);
    auto const f = [&](int word, int shift) { return static_cast<int>((w[word] >> shift) & 0x3ffu); };
    int const y[6] = {f(0, 10), f(1, 0), f(1, 20), f(2, 10), f(3, 0), f(3, 20)};
    int const cb[3] = {f(0, 0), f(1, 10), f(2, 20)};
    int const cr[3] = {f(0, 20), f(2, 0), f(3, 10)};
    auto const clamp10 = [](int v) { return static_cast<std::size_t>(v < 0 ? 0 : v > 1023 ? 1023 : v); };
    int const x0 = 6 * g;
    for (int i = 0; i < 6; i += plan.pixelStep)
    {
        int const x = x0 + i;
        if (x >= plan.width)
        {
            break;
        }
        int r = 0;
        int gg = 0;
        int b = 0;
        rgbOfYcbcr(y[i], cb[i / 2], cr[i / 2], r, gg, b);
        auto const column = plan.columnOf[static_cast<std::size_t>(x)];
        auto* target = plan.twoSubs && (i & 1) != 0 ? scratch : counts;
        ++target[plan.rowOffset[clamp10(r)] + column];
        ++target[plan.planeCells + plan.rowOffset[clamp10(gg)] + column];
        ++target[2 * plan.planeCells + plan.rowOffset[clamp10(b)] + column];
    }
}

template <class C>
static inline void analyseGroupAny(AnalysePlan const& plan, std::uint8_t const* row, int g, C* counts, C* scratch)
{
    switch (plan.kind)
    {
    case ScopeKind::Ycbcr:
        analyseGroupYcbcr(plan, row, g, counts, scratch);
        break;
    case ScopeKind::Rgb:
        analyseGroupRgb(plan, row, g, counts, scratch);
        break;
    default:
        analyseGroup(plan, row, g, counts, scratch);
        break;
    }
}

// Folds the odd sub-histogram of a strip into the counts and clears it, in every plane.
template <class C>
static inline void foldStrip(AnalysePlan const& plan, AnalysePlan::Strip const& strip, C* counts, C* scratch)
{
    for (int b = 0; b < plan.bins * plan.planes; ++b)
    {
        auto const row = static_cast<std::size_t>(b) * static_cast<std::size_t>(plan.columns);
        for (auto i = row + static_cast<std::size_t>(strip.col0); i <= row + static_cast<std::size_t>(strip.col1); ++i)
        {
            counts[i] = static_cast<C>(counts[i] + scratch[i]);
            scratch[i] = 0;
        }
    }
}

// Starts loading the strip's bytes of a row a few rows ahead (kPrefetchRows).
static inline void prefetchStrip(AnalysePlan const& plan, AnalysePlan::Strip const& strip, std::uint8_t const* v210, int y)
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
