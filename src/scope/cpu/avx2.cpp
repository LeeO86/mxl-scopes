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

// PIC: the picture colour (§4.4); each sample also adds its co-sited Cb and Cr (>> sumShift) to
// its cell's sum word, the lanes moved next to the luma lanes as in Avx2V (permCbA ... shiftCrB).
template <class C, int PS, bool TWO, bool PIC = false>
void analyseRows(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    auto* counts = hist.data<C>();
    auto* scratch = hist.scratchData<C>();
    auto* sums = hist.sums.data();
    alignas(32) std::uint32_t buffer[16 * kChunkPairs];
    alignas(32) std::uint32_t cbBuffer[PIC ? 16 * kChunkPairs : 1];
    alignas(32) std::uint32_t crBuffer[PIC ? 16 * kChunkPairs : 1];
    __m128i const sumShift = _mm_cvtsi32_si128(plan.sumShift);
    auto const chroma = [&](__m256i words, __m256i perm, __m256i shift) {
        return _mm256_srl_epi32(_mm256_and_si256(_mm256_srlv_epi32(_mm256_permutevar8x32_epi32(words, perm), shift), _mm256_set1_epi32(0x3ff)), sumShift);
    };
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
                    if constexpr (PIC)
                    {
                        auto* cb = cbBuffer + 16 * p;
                        auto* cr = crBuffer + 16 * p;
                        _mm256_store_si256(reinterpret_cast<__m256i*>(cb), chroma(words, _mm256_setr_epi32(0, 0, 1, 2, 4, 4, 5, 6), _mm256_setr_epi32(0, 0, 10, 20, 0, 0, 10, 20)));
                        _mm256_store_si256(reinterpret_cast<__m256i*>(cr), chroma(words, _mm256_setr_epi32(0, 0, 2, 3, 4, 4, 6, 7), _mm256_setr_epi32(20, 20, 0, 10, 20, 20, 0, 10)));
                        _mm256_store_si256(reinterpret_cast<__m256i*>(cb + 8), chroma(words, _mm256_setr_epi32(0, 1, 0, 2, 4, 5, 4, 6), _mm256_setr_epi32(0, 10, 0, 20, 0, 10, 0, 20)));
                        _mm256_store_si256(reinterpret_cast<__m256i*>(cr + 8), chroma(words, _mm256_setr_epi32(0, 2, 0, 3, 4, 6, 4, 7), _mm256_setr_epi32(0, 0, 0, 10, 0, 0, 0, 10)));
                    }
                }
                for (int p = 0; p < n; ++p)
                {
                    auto const* lane = buffer + 16 * p;
                    // The sum word of lane k's sample (both 32-bit sums in one add, as the scalar step).
                    auto const sum = [&](int k) {
                        if constexpr (PIC)
                        {
                            sums[lane[k]] += (static_cast<std::uint64_t>(crBuffer[16 * p + k]) << 32) | cbBuffer[16 * p + k];
                        }
                    };
                    for (int k : Lanes<PS>::even)
                    {
                        ++counts[lane[k]];
                        sum(k);
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
                            sum(k);
                        }
                    }
                }
            }
            for (int g = tail; g < strip.g1; ++g)
            {
                if constexpr (PIC)
                {
                    cpu::analyseGroupLumaPicture(plan, row, g, counts, scratch, sums);
                }
                else
                {
                    cpu::analyseGroup(plan, row, g, counts, scratch);
                }
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
    // The gamut count (§4.7) in vector lanes: a lane set is all ones where its bit is set.
    using LaneSet = Reg;
    static LaneSet lanes(std::uint32_t bits)
    {
        auto const on = [&](int i) { return (bits >> i) & 1u ? -1 : 0; };
        return _mm256_setr_epi32(on(0), on(1), on(2), on(3), on(4), on(5), on(6), on(7));
    }
    // Adds 1 to the lanes of `valid` where any of r, g, b is below `low` or above `high`.
    static void countOutside(Reg& acc, Reg r, Reg g, Reg b, Reg low, Reg high, LaneSet valid)
    {
        Reg const most = _mm256_max_epi32(_mm256_max_epi32(r, g), b);
        Reg const least = _mm256_min_epi32(_mm256_min_epi32(r, g), b);
        Reg const out = _mm256_or_si256(_mm256_cmpgt_epi32(most, high), _mm256_cmpgt_epi32(low, least));
        acc = _mm256_sub_epi32(acc, _mm256_and_si256(out, valid));
    }
    static std::uint64_t sumLanes(Reg acc)
    {
        alignas(32) std::uint32_t v[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(v), acc);
        std::uint64_t sum = 0;
        for (auto x : v)
        {
            sum += x;
        }
        return sum;
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

template <int PS, bool PIC>
void analyseStep(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    if (plan.wide)
    {
        if (plan.twoSubs)
        {
            analyseRows<std::uint32_t, PS, true, PIC>(plan, v210, hist, range);
        }
        else
        {
            analyseRows<std::uint32_t, PS, false, PIC>(plan, v210, hist, range);
        }
    }
    else if (plan.twoSubs)
    {
        analyseRows<std::uint16_t, PS, true, PIC>(plan, v210, hist, range);
    }
    else
    {
        analyseRows<std::uint16_t, PS, false, PIC>(plan, v210, hist, range);
    }
}

template <bool PIC>
void analyseLuma(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    switch (plan.pixelStep)
    {
    case 1:
        analyseStep<1, PIC>(plan, v210, hist, range);
        break;
    case 2:
        analyseStep<2, PIC>(plan, v210, hist, range);
        break;
    case 3:
        analyseStep<3, PIC>(plan, v210, hist, range);
        break;
    default:
        analyseStep<6, PIC>(plan, v210, hist, range);
        break;
    }
}
} // namespace

bool avx2Compiled()
{
    return true;
}

std::uint64_t analyseAvx2(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    // The vectorscope takes the scalar step on every level: three samples a group, the increments
    // dominate (IMPLEMENTATION_PLAN.md deviation 27).
    if (plan.kind == ScopeKind::Vector)
    {
        return analyseScalar(plan, v210, hist, range);
    }
    if (std::uint64_t outOfGamut = 0; cpu::analyseMulti<Avx2V>(plan, v210, hist, range, outOfGamut))
    {
        return outOfGamut;
    }
    if (plan.sums > 0)
    {
        analyseLuma<true>(plan, v210, hist, range);
    }
    else
    {
        analyseLuma<false>(plan, v210, hist, range);
    }
    return 0;
}
} // namespace scopes
