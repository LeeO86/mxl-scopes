// The AVX-512 analyse step: the AVX2 step of avx2.cpp on 512-bit registers, one load is four
// v210 groups (24 luma samples). Only AVX-512F instructions; no gathers or scatters (§3.2).
// Compiled with -mavx512f; it is only called after the run-time check (cpu/dispatch.hpp).
#include "scope/analyse.hpp"
#include "scope/cpu/group.hpp"
#include "scope/cpu/multi.hpp"

#include <immintrin.h>

#include <algorithm>

namespace scopes
{
namespace
{
// Buffer positions of the samples of one group quad, by pixel step: group k has x = {0, 1, 3, 4}
// in lanes 4k..4k+3 and x = {2, 5} in lanes 16 + 4k + 1 and 16 + 4k + 3.
template <int PS>
struct Lanes;
template <>
struct Lanes<1>
{
    static constexpr int even[] = {0, 3, 17, 4, 7, 21, 8, 11, 25, 12, 15, 29};
    static constexpr int odd[] = {1, 2, 19, 5, 6, 23, 9, 10, 27, 13, 14, 31};
};
template <>
struct Lanes<2>
{
    static constexpr int even[] = {0, 3, 17, 4, 7, 21, 8, 11, 25, 12, 15, 29};
    static constexpr int odd[] = {-1};
};
template <>
struct Lanes<3>
{
    static constexpr int even[] = {0, 4, 8, 12};
    static constexpr int odd[] = {2, 6, 10, 14};
};
template <>
struct Lanes<6>
{
    static constexpr int even[] = {0, 4, 8, 12};
    static constexpr int odd[] = {-1};
};

constexpr int kChunkQuads = 16;

template <class C, int PS, bool TWO>
void analyseRows(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    auto* counts = hist.data<C>();
    auto* scratch = hist.scratchData<C>();
    alignas(64) std::uint32_t buffer[32 * kChunkQuads];
    __m512i const mask = _mm512_set1_epi32(0x3ff);
    __m512i const shiftA = _mm512_setr_epi32(10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 10, 0);
    __m512i const bins = _mm512_set1_epi32(plan.bins);
    __m512i const columns = _mm512_set1_epi32(plan.columns);
    auto const* offA = plan.simdOffA.data();
    auto const* offB = plan.simdOffB.data();
    for (auto const& strip : stripsOf(plan, range))
    {
        // Whole quads of full groups; the rest of the strip goes through the reference step.
        int const quadEnd = std::min(strip.g1, plan.fullGroups);
        int const quads = quadEnd > strip.g0 ? (quadEnd - strip.g0) / 4 : 0;
        int const tail = strip.g0 + 4 * quads;
        for (int y = 0; y < plan.height; y += plan.lineStep)
        {
            auto const* row = v210 + static_cast<std::size_t>(y) * plan.stride;
            cpu::prefetchStrip(plan, strip, v210, y);
            for (int q0 = 0; q0 < quads; q0 += kChunkQuads)
            {
                int const n = std::min(kChunkQuads, quads - q0);
                for (int q = 0; q < n; ++q)
                {
                    int const g = strip.g0 + 4 * (q0 + q);
                    __m512i const words = _mm512_loadu_si512(row + 16 * static_cast<std::size_t>(g));
                    __m512i const a = _mm512_and_si512(_mm512_srlv_epi32(words, shiftA), mask);
                    __m512i const b = _mm512_and_si512(_mm512_srli_epi32(words, 20), mask);
                    // Cell = ((code × bins) >> 10) × columns + column, as AnalysePlan::rowOffset.
                    __m512i const cellA = _mm512_mullo_epi32(_mm512_srli_epi32(_mm512_mullo_epi32(a, bins), 10), columns);
                    __m512i const cellB = _mm512_mullo_epi32(_mm512_srli_epi32(_mm512_mullo_epi32(b, bins), 10), columns);
                    __m512i const idxA = _mm512_add_epi32(cellA, _mm512_loadu_si512(offA + 4 * static_cast<std::size_t>(g)));
                    __m512i const idxB = _mm512_add_epi32(cellB, _mm512_loadu_si512(offB + 4 * static_cast<std::size_t>(g)));
                    _mm512_store_si512(buffer + 32 * q, idxA);
                    _mm512_store_si512(buffer + 32 * q + 16, idxB);
                }
                for (int q = 0; q < n; ++q)
                {
                    auto const* lane = buffer + 32 * q;
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

// Vector traits of the three-plane kernels (cpu/multi.hpp): four groups per load.
struct Avx512V
{

    using Reg = __m512i;
    // The gamut count (§4.7) in vector lanes: a lane set is a mask.
    using LaneSet = __mmask16;
    static LaneSet lanes(std::uint32_t bits)
    {
        return static_cast<__mmask16>(bits & 0xffffu);
    }
    // Adds 1 to the lanes of `valid` where any of r, g, b is below `low` or above `high`.
    static void countOutside(Reg& acc, Reg r, Reg g, Reg b, Reg low, Reg high, LaneSet valid)
    {
        Reg const most = _mm512_max_epi32(_mm512_max_epi32(r, g), b);
        Reg const least = _mm512_min_epi32(_mm512_min_epi32(r, g), b);
        __mmask16 const out = static_cast<__mmask16>((_mm512_cmpgt_epi32_mask(most, high) | _mm512_cmpgt_epi32_mask(low, least)) & valid);
        acc = _mm512_mask_add_epi32(acc, out, acc, _mm512_set1_epi32(1));
    }
    static std::uint64_t sumLanes(Reg acc)
    {
        alignas(64) std::uint32_t v[16];
        _mm512_store_si512(v, acc);
        std::uint64_t sum = 0;
        for (auto x : v)
        {
            sum += x;
        }
        return sum;
    }
    static constexpr int kGroups = 4;
    static constexpr int kChunk = 16;
    static Reg set1(int v)
    {
        return _mm512_set1_epi32(v);
    }
    static Reg loadu(void const* p)
    {
        return _mm512_loadu_si512(p);
    }
    static void store(std::uint32_t* p, Reg v)
    {
        _mm512_store_si512(p, v);
    }
    static Reg and_(Reg a, Reg b)
    {
        return _mm512_and_si512(a, b);
    }
    static Reg add(Reg a, Reg b)
    {
        return _mm512_add_epi32(a, b);
    }
    static Reg sub(Reg a, Reg b)
    {
        return _mm512_sub_epi32(a, b);
    }
    static Reg mullo(Reg a, Reg b)
    {
        return _mm512_mullo_epi32(a, b);
    }
    // AVX-512F has no vpmaddwd (that is AVX-512BW): the 32-bit multiply, same products.
    static Reg mulU16(Reg a, Reg b)
    {
        return _mm512_mullo_epi32(a, b);
    }
    static Reg mulSmall(Reg a, Reg b)
    {
        return _mm512_mullo_epi32(a, b);
    }
    static Reg dot2(Reg a, Reg b, int ca, int cb)
    {
        return _mm512_add_epi32(_mm512_mullo_epi32(a, _mm512_set1_epi32(ca)), _mm512_mullo_epi32(b, _mm512_set1_epi32(cb)));
    }
    static Reg srli(Reg a, unsigned n)
    {
        return _mm512_srli_epi32(a, n);
    }
    static Reg slli(Reg a, unsigned n)
    {
        return _mm512_slli_epi32(a, n);
    }
    static Reg srai(Reg a, unsigned n)
    {
        return _mm512_srai_epi32(a, n);
    }
    static Reg srlv(Reg a, Reg n)
    {
        return _mm512_srlv_epi32(a, n);
    }
    static Reg min(Reg a, Reg b)
    {
        return _mm512_min_epi32(a, b);
    }
    static Reg max(Reg a, Reg b)
    {
        return _mm512_max_epi32(a, b);
    }
    static Reg permute(Reg words, Reg index)
    {
        return _mm512_permutexvar_epi32(index, words);
    }
    // The AVX2 patterns (Avx2V in avx2.cpp) for four groups: word w of group j is lane 4j + w.
    static Reg pattern(int a, int b, int c, int d, bool perGroup)
    {
        int const o = perGroup ? 4 : 0;
        return _mm512_setr_epi32(a, b, c, d, a + o, b + o, c + o, d + o, a + 2 * o, b + 2 * o, c + 2 * o, d + 2 * o, a + 3 * o, b + 3 * o, c + 3 * o, d + 3 * o);
    }
    static Reg shiftYA()
    {
        return pattern(10, 0, 10, 0, false);
    }
    static Reg permCbA()
    {
        return pattern(0, 0, 1, 2, true);
    }
    static Reg shiftCbA()
    {
        return pattern(0, 0, 10, 20, false);
    }
    static Reg permCrA()
    {
        return pattern(0, 0, 2, 3, true);
    }
    static Reg shiftCrA()
    {
        return pattern(20, 20, 0, 10, false);
    }
    static Reg permCbB()
    {
        return pattern(0, 1, 0, 2, true);
    }
    static Reg shiftCbB()
    {
        return pattern(0, 10, 0, 20, false);
    }
    static Reg permCrB()
    {
        return pattern(0, 2, 0, 3, true);
    }
    static Reg shiftCrB()
    {
        return pattern(0, 0, 0, 10, false);
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

bool avx512Compiled()
{
    return true;
}

std::uint64_t analyseAvx512(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    if (std::uint64_t outOfGamut = 0; cpu::analyseMulti<Avx512V>(plan, v210, hist, range, outOfGamut))
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
