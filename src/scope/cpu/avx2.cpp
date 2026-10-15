// The AVX2 analyse step (SPECIFICATION §4.9): one 256-bit load is two v210 groups (12 luma
// samples). Shifts and masks extract the samples, a 32-bit multiply bins them, an add places
// them in their column, and a scalar loop increments the cells. No gathers (§3.2).
// This file is compiled with -mavx2 -mfma; it is only called after the run-time check.
#include "scope/analyse.hpp"
#include "scope/cpu/group.hpp"

#include <immintrin.h>

#include <algorithm>

namespace scopes
{
namespace
{
// Buffer positions of the samples of one group pair, by pixel step: lanes 0..7 hold
// x = {0, 1, 3, 4, 6, 7, 9, 10}, lanes 9, 11, 13, 15 hold x = {2, 5, 8, 11}.
template <int PS>
struct Lanes;
template <>
struct Lanes<1>
{
    static constexpr int even[] = {0, 3, 4, 7, 9, 13};
    static constexpr int odd[] = {1, 2, 5, 6, 11, 15};
};
template <>
struct Lanes<2>
{
    static constexpr int even[] = {0, 3, 4, 7, 9, 13};
    static constexpr int odd[] = {-1};
};
template <>
struct Lanes<3>
{
    static constexpr int even[] = {0, 4};
    static constexpr int odd[] = {2, 6};
};
template <>
struct Lanes<6>
{
    static constexpr int even[] = {0, 4};
    static constexpr int odd[] = {-1};
};

constexpr int kChunkPairs = 32;

template <int PS, bool TWO>
void analyseRows(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist)
{
    auto* counts = hist.counts.data();
    auto* scratch = hist.scratch.data();
    alignas(32) std::uint32_t buffer[16 * kChunkPairs];
    __m256i const mask = _mm256_set1_epi32(0x3ff);
    __m256i const shiftA = _mm256_setr_epi32(10, 0, 10, 0, 10, 0, 10, 0);
    __m256i const bins = _mm256_set1_epi32(plan.bins);
    __m256i const columns = _mm256_set1_epi32(plan.columns);
    auto const* offA = plan.simdOffA.data();
    auto const* offB = plan.simdOffB.data();
    for (auto const& strip : plan.strips)
    {
        // Whole pairs of full groups; the rest of the strip goes through the reference step.
        int const pairEnd = std::min(strip.g1, plan.fullGroups);
        int const pairs = pairEnd > strip.g0 ? (pairEnd - strip.g0) / 2 : 0;
        int const tail = strip.g0 + 2 * pairs;
        for (int y = 0; y < plan.height; y += plan.lineStep)
        {
            auto const* row = v210 + static_cast<std::size_t>(y) * plan.stride;
            cpu::prefetchStrip(plan, strip, v210, y);
            for (int p0 = 0; p0 < pairs; p0 += kChunkPairs)
            {
                int const n = std::min(kChunkPairs, pairs - p0);
                for (int p = 0; p < n; ++p)
                {
                    int const g = strip.g0 + 2 * (p0 + p);
                    __m256i const words = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(row + 16 * static_cast<std::size_t>(g)));
                    __m256i const a = _mm256_and_si256(_mm256_srlv_epi32(words, shiftA), mask);
                    __m256i const b = _mm256_and_si256(_mm256_srli_epi32(words, 20), mask);
                    // Cell = ((code × bins) >> 10) × columns + column, as AnalysePlan::rowOffset.
                    __m256i const binA = _mm256_mullo_epi32(_mm256_srli_epi32(_mm256_mullo_epi32(a, bins), 10), columns);
                    __m256i const binB = _mm256_mullo_epi32(_mm256_srli_epi32(_mm256_mullo_epi32(b, bins), 10), columns);
                    __m256i const idxA = _mm256_add_epi32(binA, _mm256_loadu_si256(reinterpret_cast<__m256i const*>(offA + 4 * static_cast<std::size_t>(g))));
                    __m256i const idxB = _mm256_add_epi32(binB, _mm256_loadu_si256(reinterpret_cast<__m256i const*>(offB + 4 * static_cast<std::size_t>(g))));
                    _mm256_store_si256(reinterpret_cast<__m256i*>(buffer + 16 * p), idxA);
                    _mm256_store_si256(reinterpret_cast<__m256i*>(buffer + 16 * p + 8), idxB);
                }
                for (int p = 0; p < n; ++p)
                {
                    auto const* lane = buffer + 16 * p;
                    for (int k : Lanes<PS>::even)
                    {
                        ++counts[lane[k]];
                    }
                    if constexpr (Lanes<PS>::odd[0] >= 0)
                    {
                        for (int k : Lanes<PS>::odd)
                        {
                            if constexpr (TWO)
                            {
                                ++scratch[lane[k]];
                            }
                            else
                            {
                                ++counts[lane[k]];
                            }
                        }
                    }
                }
            }
            for (int g = tail; g < strip.g1; ++g)
            {
                cpu::analyseGroup(plan, row, g, counts, scratch);
            }
        }
        if constexpr (TWO)
        {
            cpu::foldStrip(plan, strip, counts, scratch);
        }
    }
}

template <int PS>
void analyseStep(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist)
{
    if (plan.twoSubs)
    {
        analyseRows<PS, true>(plan, v210, hist);
    }
    else
    {
        analyseRows<PS, false>(plan, v210, hist);
    }
}
} // namespace

bool avx2Compiled()
{
    return true;
}

void analyseAvx2(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist)
{
    switch (plan.pixelStep)
    {
    case 1:
        analyseStep<1>(plan, v210, hist);
        break;
    case 2:
        analyseStep<2>(plan, v210, hist);
        break;
    case 3:
        analyseStep<3>(plan, v210, hist);
        break;
    default:
        analyseStep<6>(plan, v210, hist);
        break;
    }
}
} // namespace scopes
