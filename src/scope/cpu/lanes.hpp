#pragma once

// Buffer positions of the samples of one SIMD load (G v210 groups), for the analyse kernels of
// the three-plane kinds (SPECIFICATION §5.1). Constexpr only: the AVX2 and AVX-512 units build
// their increment lists from it at compile time.
//
// A load of G groups has N = 4G 32-bit lanes; lane 4j + w is word w of group j.
// `Rgb`: per component c (R′ G′ B′) two vectors, A (Y0 Y1 Y3 Y4 of each group, all lanes) and
// B (Y2 Y5 in lanes 1 and 3), at buffer offsets 2N c and 2N c + N, the layout of the luma step.
// `Ycbcr`: three vectors, the fields at bit 0, 10 and 20 of each word, at offsets 0, N, 2N:
// word 0 is Cb0 Y0 Cr0, word 1 Y1 Cb1 Y2, word 2 Cr1 Y3 Cb2, word 3 Y4 Cr2 Y5.

#include "scope/analyse.hpp"

#include <array>

namespace scopes::cpu
{
struct LaneList
{
    // Into the counts, and into the scratch sub-histogram when the plan has two (else counts).
    std::array<int, 72> even{};
    int nEven = 0;
    std::array<int, 72> odd{};
    int nOdd = 0;
};

// Lane of luma sample i of group j in the A/B layout.
constexpr int lumaLaneAB(int n, int j, int i)
{
    constexpr int word[6] = {0, 1, 1, 2, 3, 3};
    return (i == 2 || i == 5 ? n : 0) + 4 * j + word[i];
}

// `Rgb`: the lanes of vector A (Y0 Y1 Y3 Y4 of each group) and B (Y2, Y5 in lanes 1 and 3) that
// hold a sample taken at this pixel step, as bit masks (for the gamut count, §4.7).
constexpr std::uint32_t validLanesA(int groups, int pixelStep)
{
    constexpr int x[4] = {0, 1, 3, 4};
    std::uint32_t mask = 0;
    for (int j = 0; j < groups; ++j)
    {
        for (int w = 0; w < 4; ++w)
        {
            mask |= x[w] % pixelStep == 0 ? 1u << (4 * j + w) : 0u;
        }
    }
    return mask;
}
constexpr std::uint32_t validLanesB(int groups, int pixelStep)
{
    std::uint32_t mask = 0;
    for (int j = 0; j < groups; ++j)
    {
        mask |= 2 % pixelStep == 0 ? 1u << (4 * j + 1) : 0u;
        mask |= 5 % pixelStep == 0 ? 1u << (4 * j + 3) : 0u;
    }
    return mask;
}

constexpr LaneList makeLanes(ScopeKind kind, int groups, int pixelStep)
{
    LaneList list;
    int const n = 4 * groups;
    auto put = [&](int lane, bool odd) {
        if (odd)
        {
            list.odd[static_cast<std::size_t>(list.nOdd++)] = lane;
        }
        else
        {
            list.even[static_cast<std::size_t>(list.nEven++)] = lane;
        }
    };
    if (kind == ScopeKind::Rgb)
    {
        // R′, G′, B′ of one sample next to each other: on a flat area the next sample of the same
        // plane often hits the same cell, and two other planes in between hide that
        // load-add-store chain (§4.3, store forwarding).
        for (int c = 0; c < 3; ++c)
        {
            for (int j = 0; j < groups; ++j)
            {
                for (int i = 0; i < 6; i += pixelStep)
                {
                    put(2 * n * c + lumaLaneAB(n, j, i), (i & 1) != 0);
                }
            }
        }
        return list;
    }
    // Ycbcr: block (0, N, 2N) and word of each sample.
    constexpr int yBlock[6] = {1, 0, 2, 1, 0, 2};
    constexpr int yWord[6] = {0, 1, 1, 2, 3, 3};
    constexpr int cbBlock[3] = {0, 1, 2};
    constexpr int cbWord[3] = {0, 1, 2};
    constexpr int crBlock[3] = {2, 0, 1};
    constexpr int crWord[3] = {0, 2, 3};
    for (int j = 0; j < groups; ++j)
    {
        for (int i = 0; i < 6; i += pixelStep)
        {
            put(n * yBlock[i] + 4 * j + yWord[i], (i & 1) != 0);
            if ((i & 1) == 0)
            {
                int const k = i / 2;
                put(n * cbBlock[k] + 4 * j + cbWord[k], (k & 1) != 0);
                put(n * crBlock[k] + 4 * j + crWord[k], (k & 1) != 0);
            }
        }
    }
    return list;
}
} // namespace scopes::cpu
