#pragma once

// The analyse kernels of the three-plane kinds (`Ycbcr`, `Rgb`), written once over a vector
// traits type `V` that the AVX2 unit (two groups per load) and the AVX-512 unit (four) define.
// Included only by those units; every function is static, so nothing compiled with their flags
// can be picked by the linker for baseline code.
//
// Per load: unpack, bin and place the samples with vector code, store the cell indices, then a
// scalar loop increments them (no gathers or scatters, SPECIFICATION §3.2). `Rgb` computes
// R′G′B′ with the Q13 formula of §4.8 in 32-bit lanes, exactly as the scalar reference.

#include "scope/analyse.hpp"
#include "scope/cpu/group.hpp"
#include "scope/cpu/lanes.hpp"

#include <algorithm>

namespace scopes::cpu
{
template <class V, class C, ScopeKind K, int PS, bool TWO>
static void analyseRowsMulti(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    using R = typename V::Reg;
    constexpr int G = V::kGroups;
    constexpr int N = 4 * G;
    constexpr int kPer = K == ScopeKind::Rgb ? 6 * N : 3 * N;
    constexpr int kChunk = V::kChunk;
    static constexpr LaneList kLanes = makeLanes(K, G, PS);
    auto* counts = hist.data<C>();
    auto* scratch = hist.scratchData<C>();
    alignas(64) std::uint32_t buffer[kPer * kChunk];
    R const mask = V::set1(0x3ff);
    R const bins = V::set1(plan.bins);
    R const columns = V::set1(plan.columns);
    auto const cell = [&](R value, std::uint32_t const* offsets) {
        // ((code × bins) >> 10) × columns + offset, as AnalysePlan::rowOffset. Every factor fits
        // 16 bits and is not negative (code and bin ≤ 1023, bins ≤ 1024, columns ≤ 3840).
        return V::add(V::mulU16(V::srli(V::mulU16(value, bins), 10), columns), V::loadu(offsets));
    };
    for (auto const& strip : stripsOf(plan, range))
    {
        int const fullEnd = std::min(strip.g1, plan.fullGroups);
        int const loads = fullEnd > strip.g0 ? (fullEnd - strip.g0) / G : 0;
        int const tail = strip.g0 + G * loads;
        for (int y = 0; y < plan.height; y += plan.lineStep)
        {
            auto const* row = v210 + static_cast<std::size_t>(y) * plan.stride;
            prefetchStrip(plan, strip, v210, y);
            for (int l0 = 0; l0 < loads; l0 += kChunk)
            {
                int const n = std::min(kChunk, loads - l0);
                for (int l = 0; l < n; ++l)
                {
                    int const g = strip.g0 + G * (l0 + l);
                    auto const at = 4 * static_cast<std::size_t>(g);
                    R const words = V::loadu(row + 16 * static_cast<std::size_t>(g));
                    auto* out = buffer + kPer * l;
                    if constexpr (K == ScopeKind::Ycbcr)
                    {
                        R const s0 = V::and_(words, mask);
                        R const s10 = V::and_(V::srli(words, 10), mask);
                        R const s20 = V::and_(V::srli(words, 20), mask);
                        V::store(out, cell(s0, plan.simdOffW0.data() + at));
                        V::store(out + N, cell(s10, plan.simdOffW10.data() + at));
                        V::store(out + 2 * N, cell(s20, plan.simdOffW20.data() + at));
                    }
                    else
                    {
                        // Luma A (Y0 Y1 Y3 Y4) and B (Y2 Y5 in lanes 1, 3) with their co-sited
                        // chroma pairs, moved into the same lanes by a permute.
                        R const yA = V::and_(V::srlv(words, V::shiftYA()), mask);
                        R const yB = V::and_(V::srli(words, 20), mask);
                        R const cbA = V::and_(V::srlv(V::permute(words, V::permCbA()), V::shiftCbA()), mask);
                        R const crA = V::and_(V::srlv(V::permute(words, V::permCrA()), V::shiftCrA()), mask);
                        R const cbB = V::and_(V::srlv(V::permute(words, V::permCbB()), V::shiftCbB()), mask);
                        R const crB = V::and_(V::srlv(V::permute(words, V::permCrB()), V::shiftCrB()), mask);
                        R const zero = V::set1(0);
                        R const top = V::set1(1023);
                        R const round = V::set1(4096);
                        R const centre = V::set1(512);
                        auto const clamp = [&](R v) { return V::min(V::max(v, zero), top); };
                        // §4.8 in 32-bit lanes; the chroma differences (−512..511) and the Q13
                        // coefficients fit signed 16 bits, so mulSmall and dot2 are exact.
                        auto const rgb = [&](R yv, R cbv, R crv, R& r, R& gv, R& b) {
                            R const yy = V::add(V::slli(yv, 13), round);
                            R const db = V::sub(cbv, centre);
                            R const dr = V::sub(crv, centre);
                            r = clamp(V::srai(V::add(yy, V::mulSmall(dr, V::set1(kQ13CrR))), 13));
                            gv = clamp(V::srai(V::add(yy, V::dot2(db, dr, -kQ13CbG, -kQ13CrG)), 13));
                            b = clamp(V::srai(V::add(yy, V::mulSmall(db, V::set1(kQ13CbB))), 13));
                        };
                        R rA, gA, bA, rB, gB, bB;
                        rgb(yA, cbA, crA, rA, gA, bA);
                        rgb(yB, cbB, crB, rB, gB, bB);
                        auto const* offA = plan.simdOffA.data() + at;
                        auto const* offB = plan.simdOffB.data() + at;
                        auto const plane = static_cast<int>(plan.planeCells);
                        R const p1 = V::set1(plane);
                        R const p2 = V::set1(2 * plane);
                        V::store(out, cell(rA, offA));
                        V::store(out + N, cell(rB, offB));
                        V::store(out + 2 * N, V::add(cell(gA, offA), p1));
                        V::store(out + 3 * N, V::add(cell(gB, offB), p1));
                        V::store(out + 4 * N, V::add(cell(bA, offA), p2));
                        V::store(out + 5 * N, V::add(cell(bB, offB), p2));
                    }
                }
                for (int l = 0; l < n; ++l)
                {
                    auto const* lane = buffer + kPer * l;
#pragma GCC unroll 72
                    for (int k = 0; k < kLanes.nEven; ++k)
                    {
                        ++counts[lane[kLanes.even[static_cast<std::size_t>(k)]]];
                    }
#pragma GCC unroll 72
                    for (int k = 0; k < kLanes.nOdd; ++k)
                    {
                        if constexpr (TWO)
                        {
                            ++scratch[lane[kLanes.odd[static_cast<std::size_t>(k)]]];
                        }
                        else
                        {
                            ++counts[lane[kLanes.odd[static_cast<std::size_t>(k)]]];
                        }
                    }
                }
            }
            for (int g = tail; g < strip.g1; ++g)
            {
                analyseGroupAny(plan, row, g, counts, scratch);
            }
        }
        if constexpr (TWO)
        {
            foldStrip(plan, strip, counts, scratch);
        }
    }
}

template <class V, ScopeKind K, int PS, class C>
static void analyseMultiCells(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    if (plan.twoSubs)
    {
        analyseRowsMulti<V, C, K, PS, true>(plan, v210, hist, range);
    }
    else
    {
        analyseRowsMulti<V, C, K, PS, false>(plan, v210, hist, range);
    }
}

template <class V, ScopeKind K, int PS>
static void analyseMultiStep(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    if (plan.wide)
    {
        analyseMultiCells<V, K, PS, std::uint32_t>(plan, v210, hist, range);
    }
    else
    {
        analyseMultiCells<V, K, PS, std::uint16_t>(plan, v210, hist, range);
    }
}

template <class V, ScopeKind K>
static void analyseMultiKind(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    switch (plan.pixelStep)
    {
    case 1:
        analyseMultiStep<V, K, 1>(plan, v210, hist, range);
        break;
    case 2:
        analyseMultiStep<V, K, 2>(plan, v210, hist, range);
        break;
    case 3:
        analyseMultiStep<V, K, 3>(plan, v210, hist, range);
        break;
    default:
        analyseMultiStep<V, K, 6>(plan, v210, hist, range);
        break;
    }
}

// Ycbcr or Rgb; false for Luma (the caller's own step).
template <class V>
static bool analyseMulti(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    if (plan.kind == ScopeKind::Ycbcr)
    {
        analyseMultiKind<V, ScopeKind::Ycbcr>(plan, v210, hist, range);
        return true;
    }
    if (plan.kind == ScopeKind::Rgb)
    {
        analyseMultiKind<V, ScopeKind::Rgb>(plan, v210, hist, range);
        return true;
    }
    return false;
}
} // namespace scopes::cpu
