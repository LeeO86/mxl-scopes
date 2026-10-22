// The AVX2 analyse step (SPECIFICATION §4.9): one 256-bit load is two v210 groups (12 luma
// samples). Shifts and masks extract the samples, a 32-bit multiply bins them, an add places
// them in their column, and a scalar loop increments the cells. No gathers (§3.2).
// This file is compiled with -mavx2 -mfma; it is only called after the run-time check.
#include "scope/analyse.hpp"
#include "scope/cpu/group.hpp"
#include "scope/cpu/multi.hpp"

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

template <class C, int PS, bool TWO>
void analyseRows(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    auto* counts = hist.data<C>();
    auto* scratch = hist.scratchData<C>();
    alignas(32) std::uint32_t buffer[16 * kChunkPairs];
    __m256i const mask = _mm256_set1_epi32(0x3ff);
    __m256i const shiftA = _mm256_setr_epi32(10, 0, 10, 0, 10, 0, 10, 0);
    __m256i const bins = _mm256_set1_epi32(plan.bins);
    __m256i const columns = _mm256_set1_epi32(plan.columns);
    auto const* offA = plan.simdOffA.data();
    auto const* offB = plan.simdOffB.data();
    for (auto const& strip : stripsOf(plan, range))
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
                    // Every factor fits 16 bits: vpmaddwd (one uop) gives the exact products.
                    __m256i const binA = _mm256_madd_epi16(_mm256_srli_epi32(_mm256_madd_epi16(a, bins), 10), columns);
                    __m256i const binB = _mm256_madd_epi16(_mm256_srli_epi32(_mm256_madd_epi16(b, bins), 10), columns);
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

// Vector traits of the three-plane kernels (cpu/multi.hpp): two groups per load.
struct Avx2V
{

    using Reg = __m256i;
    // Bit per lane: the lane is below `low` or above `high` (signed).
    static std::uint32_t outside(Reg v, Reg low, Reg high)
    {
        Reg const out = _mm256_or_si256(_mm256_cmpgt_epi32(low, v), _mm256_cmpgt_epi32(v, high));
        return static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(out)));
    }
    static constexpr int kGroups = 2;
    static constexpr int kChunk = 32;
    static Reg set1(int v)
    {
        return _mm256_set1_epi32(v);
    }
    static Reg loadu(void const* p)
    {
        return _mm256_loadu_si256(static_cast<__m256i const*>(p));
    }
    static void store(std::uint32_t* p, Reg v)
    {
        _mm256_store_si256(reinterpret_cast<__m256i*>(p), v);
    }
    static Reg and_(Reg a, Reg b)
    {
        return _mm256_and_si256(a, b);
    }
    static Reg add(Reg a, Reg b)
    {
        return _mm256_add_epi32(a, b);
    }
    static Reg sub(Reg a, Reg b)
    {
        return _mm256_sub_epi32(a, b);
    }
    static Reg mullo(Reg a, Reg b)
    {
        return _mm256_mullo_epi32(a, b);
    }
    // a × b for lanes that hold values 0..32767 (high halves zero): one vpmaddwd instead of the
    // two-µop vpmulld, the same 32-bit product.
    static Reg mulU16(Reg a, Reg b)
    {
        return _mm256_madd_epi16(a, b);
    }
    // a × b for a signed `a` in −32768..32767 (its high half is cleared first) and a constant `b`
    // in 0..32767: one vpmaddwd, the exact signed product.
    static Reg mulSmall(Reg a, Reg b)
    {
        return _mm256_madd_epi16(_mm256_and_si256(a, _mm256_set1_epi32(0xffff)), b);
    }
    // a × ca + b × cb for 16-bit signed a, b, ca, cb: one vpmaddwd on (a, b) pairs.
    static Reg dot2(Reg a, Reg b, int ca, int cb)
    {
        Reg const pairs = _mm256_blend_epi16(a, _mm256_slli_epi32(b, 16), 0xaa);
        return _mm256_madd_epi16(pairs, _mm256_set1_epi32(static_cast<int>((static_cast<std::uint32_t>(cb) << 16) | (static_cast<std::uint32_t>(ca) & 0xffffu))));
    }
    static Reg srli(Reg a, int n)
    {
        return _mm256_srli_epi32(a, n);
    }
    static Reg slli(Reg a, int n)
    {
        return _mm256_slli_epi32(a, n);
    }
    static Reg srai(Reg a, int n)
    {
        return _mm256_srai_epi32(a, n);
    }
    static Reg srlv(Reg a, Reg n)
    {
        return _mm256_srlv_epi32(a, n);
    }
    static Reg min(Reg a, Reg b)
    {
        return _mm256_min_epi32(a, b);
    }
    static Reg max(Reg a, Reg b)
    {
        return _mm256_max_epi32(a, b);
    }
    static Reg permute(Reg words, Reg index)
    {
        return _mm256_permutevar8x32_epi32(words, index);
    }
    // Per group: Y0 Y1 Y3 Y4 (A), the chroma pair of each (k = 0, 0, 1, 2), and for B (lanes 1
    // and 3: Y2, Y5) the pairs k = 1 and 2.
    static Reg shiftYA()
    {
        return _mm256_setr_epi32(10, 0, 10, 0, 10, 0, 10, 0);
    }
    static Reg permCbA()
    {
        return _mm256_setr_epi32(0, 0, 1, 2, 4, 4, 5, 6);
    }
    static Reg shiftCbA()
    {
        return _mm256_setr_epi32(0, 0, 10, 20, 0, 0, 10, 20);
    }
    static Reg permCrA()
    {
        return _mm256_setr_epi32(0, 0, 2, 3, 4, 4, 6, 7);
    }
    static Reg shiftCrA()
    {
        return _mm256_setr_epi32(20, 20, 0, 10, 20, 20, 0, 10);
    }
    static Reg permCbB()
    {
        return _mm256_setr_epi32(0, 1, 0, 2, 4, 5, 4, 6);
    }
    static Reg shiftCbB()
    {
        return _mm256_setr_epi32(0, 10, 0, 20, 0, 10, 0, 20);
    }
    static Reg permCrB()
    {
        return _mm256_setr_epi32(0, 2, 0, 3, 4, 6, 4, 7);
    }
    static Reg shiftCrB()
    {
        return _mm256_setr_epi32(0, 0, 0, 10, 0, 0, 0, 10);
    }
};

template <int PS>
void analyseStep(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    if (plan.wide)
    {
        if (plan.twoSubs)
        {
            analyseRows<std::uint32_t, PS, true>(plan, v210, hist, range);
        }
        else
        {
            analyseRows<std::uint32_t, PS, false>(plan, v210, hist, range);
        }
    }
    else if (plan.twoSubs)
    {
        analyseRows<std::uint16_t, PS, true>(plan, v210, hist, range);
    }
    else
    {
        analyseRows<std::uint16_t, PS, false>(plan, v210, hist, range);
    }
}
} // namespace

bool avx2Compiled()
{
    return true;
}

std::uint64_t analyseAvx2(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    if (std::uint64_t outOfGamut = 0; cpu::analyseMulti<Avx2V>(plan, v210, hist, range, outOfGamut))
    {
        return outOfGamut;
    }
    switch (plan.pixelStep)
    {
    case 1:
        analyseStep<1>(plan, v210, hist, range);
        break;
    case 2:
        analyseStep<2>(plan, v210, hist, range);
        break;
    case 3:
        analyseStep<3>(plan, v210, hist, range);
        break;
    default:
        analyseStep<6>(plan, v210, hist, range);
        break;
    }
    return 0;
}
} // namespace scopes
