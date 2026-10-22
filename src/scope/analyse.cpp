#include "scope/analyse.hpp"

#include "scope/cpu/group.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace scopes
{
ScopeKind scopeKindOf(std::string const& mode)
{
    if (mode == "ycbcr-parade" || mode == "ycbcr-overlay")
    {
        return ScopeKind::Ycbcr;
    }
    if (mode == "rgb-parade" || mode == "rgb-overlay")
    {
        return ScopeKind::Rgb;
    }
    return ScopeKind::Luma;
}

void Histogram::reset(AnalysePlan const& plan)
{
    columns = plan.columns;
    bins = plan.bins;
    planes = plan.planes;
    wide = plan.wide;
    auto const cells = plan.planeCells * static_cast<std::size_t>(plan.planes);
    counts.assign(wide ? 0 : cells, 0);
    scratch.assign(!wide && plan.twoSubs ? cells : 0, 0);
    counts32.assign(wide ? cells : 0, 0);
    scratch32.assign(wide && plan.twoSubs ? cells : 0, 0);
    outOfGamut = 0;
}

void Histogram::clear()
{
    std::fill(counts.begin(), counts.end(), std::uint16_t{0});
    std::fill(scratch.begin(), scratch.end(), std::uint16_t{0});
    std::fill(counts32.begin(), counts32.end(), 0u);
    std::fill(scratch32.begin(), scratch32.end(), 0u);
    outOfGamut = 0;
}

AnalysePlan makeAnalysePlan(int width, int height, std::size_t stride, int lineStep, int pixelStep, int columns, int bins, int stripColumns, ScopeKind kind,
    int grains)
{
    if (grains < 1 || width < 1 || height < 1 || columns < 1 || bins < 1 || bins > 1024 || stride < v210RowBytes(width) || lineStep < 1 || pixelStep < 1 ||
        6 % pixelStep != 0 || stripColumns < 1)
    {
        throw std::invalid_argument("invalid analyse plan");
    }
    AnalysePlan plan;
    plan.width = width;
    plan.height = height;
    plan.stride = stride;
    plan.lineStep = lineStep;
    plan.pixelStep = pixelStep;
    plan.columns = columns;
    plan.bins = bins;
    plan.kind = kind;
    plan.planes = planesOf(kind);
    plan.planeCells = static_cast<std::size_t>(columns) * static_cast<std::size_t>(bins);
    // §4.3: two sub-histograms when at least two input samples fall into one column.
    plan.twoSubs = width / columns >= 2;
    plan.columnOf.resize(static_cast<std::size_t>(width));
    std::vector<int> col(static_cast<std::size_t>(width));
    for (int x = 0; x < width; ++x)
    {
        col[static_cast<std::size_t>(x)] = static_cast<int>((static_cast<std::int64_t>(x) * columns) / width);
        plan.columnOf[static_cast<std::size_t>(x)] = static_cast<std::uint32_t>(col[static_cast<std::size_t>(x)]);
    }
    for (int v = 0; v < 1024; ++v)
    {
        plan.binOf[static_cast<std::size_t>(v)] = static_cast<std::uint16_t>((v * bins) >> 10);
        plan.rowOffset[static_cast<std::size_t>(v)] = static_cast<std::uint32_t>(plan.binOf[static_cast<std::size_t>(v)]) * static_cast<std::uint32_t>(columns);
    }
    int const groups = (width + 5) / 6;
    plan.fullGroups = width / 6;
    for (int g = 0; g < groups;)
    {
        AnalysePlan::Strip strip;
        strip.g0 = g;
        strip.col0 = col[static_cast<std::size_t>(6 * g)];
        // Whole quads of groups, so the AVX2 engine (pairs) and the AVX-512 engine (quads) take
        // every strip in full loads.
        int end = std::min(groups, g + 4);
        auto lastCol = [&](int group) { return col[static_cast<std::size_t>(std::min(width - 1, 6 * group + 5))]; };
        while (end < groups && lastCol(std::min(groups, end + 4) - 1) < strip.col0 + stripColumns)
        {
            end = std::min(groups, end + 4);
        }
        // End the strip where a new histogram column starts, so no column spans two strips and
        // strip workers can split between any two strips (SCOPES_THREADS): a whole quad of
        // groups if one fits, else any group.
        auto const clean = [&](int e) { return e >= groups || col[static_cast<std::size_t>(6 * e - 1)] < col[static_cast<std::size_t>(6 * e)]; };
        int cut = end;
        while (cut - 4 > g && !clean(cut))
        {
            cut -= 4;
        }
        if (!clean(cut))
        {
            cut = end;
            while (cut - 1 > g && !clean(cut))
            {
                --cut;
            }
            if (!clean(cut))
            {
                cut = end; // one column wider than the strip: shared (workers do not split here)
            }
        }
        end = cut;
        strip.g1 = end;
        strip.col1 = lastCol(end - 1);
        plan.strips.push_back(strip);
        g = end;
    }
    plan.simdOffA.assign(static_cast<std::size_t>(groups) * 4u + 16u, 0);
    plan.simdOffB.assign(static_cast<std::size_t>(groups) * 4u + 16u, 0);
    for (int g = 0; g < plan.fullGroups; ++g)
    {
        auto const off = [&](int i) { return plan.columnOf[static_cast<std::size_t>(6 * g + i)]; };
        auto* a = plan.simdOffA.data() + 4 * static_cast<std::size_t>(g);
        auto* b = plan.simdOffB.data() + 4 * static_cast<std::size_t>(g);
        a[0] = off(0);
        a[1] = off(1);
        a[2] = off(3);
        a[3] = off(4);
        b[1] = off(2);
        b[3] = off(5);
    }
    if (kind == ScopeKind::Ycbcr)
    {
        auto const cb = static_cast<std::uint32_t>(plan.planeCells);
        auto const cr = static_cast<std::uint32_t>(2 * plan.planeCells);
        plan.simdOffW0.assign(plan.simdOffA.size(), 0);
        plan.simdOffW10.assign(plan.simdOffA.size(), 0);
        plan.simdOffW20.assign(plan.simdOffA.size(), 0);
        for (int g = 0; g < plan.fullGroups; ++g)
        {
            auto const off = [&](int i) { return plan.columnOf[static_cast<std::size_t>(6 * g + i)]; };
            auto* w0 = plan.simdOffW0.data() + 4 * static_cast<std::size_t>(g);
            auto* w10 = plan.simdOffW10.data() + 4 * static_cast<std::size_t>(g);
            auto* w20 = plan.simdOffW20.data() + 4 * static_cast<std::size_t>(g);
            // Word 0: Cb0 Y0 Cr0; word 1: Y1 Cb1 Y2; word 2: Cr1 Y3 Cb2; word 3: Y4 Cr2 Y5.
            w0[0] = cb + off(0);
            w10[0] = off(0);
            w20[0] = cr + off(0);
            w0[1] = off(1);
            w10[1] = cb + off(2);
            w20[1] = off(2);
            w0[2] = cr + off(2);
            w10[2] = off(3);
            w20[2] = cb + off(4);
            w0[3] = off(4);
            w10[3] = cr + off(4);
            w20[3] = off(5);
        }
    }
    // Samples per frame and per column, for each plane. Chroma sample k of a group (x = 6g + 2k)
    // is taken when its co-sited luma sample is (§4.2).
    auto const rows = static_cast<std::uint64_t>((height + lineStep - 1) / lineStep);
    std::vector<std::uint32_t> perColumn(static_cast<std::size_t>(columns), 0);
    std::vector<std::uint32_t> perColumnChroma(static_cast<std::size_t>(columns), 0);
    std::uint64_t perRow = 0;
    std::uint64_t perRowChroma = 0;
    for (int x = 0; x < width; ++x)
    {
        if ((x % 6) % pixelStep == 0)
        {
            ++perRow;
            ++perColumn[static_cast<std::size_t>(col[static_cast<std::size_t>(x)])];
            if ((x % 2) == 0)
            {
                ++perRowChroma;
                ++perColumnChroma[static_cast<std::size_t>(col[static_cast<std::size_t>(x)])];
            }
        }
    }
    auto const maxLuma = static_cast<std::uint64_t>(*std::max_element(perColumn.begin(), perColumn.end())) * rows;
    auto const maxChroma = static_cast<std::uint64_t>(*std::max_element(perColumnChroma.begin(), perColumnChroma.end())) * rows;
    auto const maxUpdate = maxLuma * static_cast<std::uint64_t>(grains);
    if (maxUpdate > 0xffffffffull)
    {
        throw CellOverflow("a histogram cell could exceed 32 bits (" + std::to_string(maxUpdate) + ")");
    }
    plan.wide = maxUpdate > 65535;
    for (int p = 0; p < plan.planes; ++p)
    {
        bool const chroma = kind == ScopeKind::Ycbcr && p > 0;
        plan.planeSamples[static_cast<std::size_t>(p)] = (chroma ? perRowChroma : perRow) * rows;
        plan.planeMaxCell[static_cast<std::size_t>(p)] = static_cast<std::uint32_t>(chroma ? maxChroma : maxLuma);
    }
    plan.samples = plan.planeSamples[0];
    plan.maxCellCount = *std::max_element(plan.planeMaxCell.begin(), plan.planeMaxCell.begin() + plan.planes);
    return plan;
}

namespace
{
template <class C>
std::uint64_t analyseScalarCells(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    std::uint64_t out = 0;
    auto* counts = hist.data<C>();
    auto* scratch = hist.scratchData<C>();
    for (auto const& strip : stripsOf(plan, range))
    {
        for (int y = 0; y < plan.height; y += plan.lineStep)
        {
            auto const* row = v210 + static_cast<std::size_t>(y) * plan.stride;
            cpu::prefetchStrip(plan, strip, v210, y);
            for (int g = strip.g0; g < strip.g1; ++g)
            {
                out += static_cast<std::uint64_t>(cpu::analyseGroupAny(plan, row, g, counts, scratch));
            }
        }
        if (plan.twoSubs)
        {
            cpu::foldStrip(plan, strip, counts, scratch);
        }
    }
    return out;
}
} // namespace

std::uint64_t analyseScalar(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist, StripRange range)
{
    return plan.wide ? analyseScalarCells<std::uint32_t>(plan, v210, hist, range) : analyseScalarCells<std::uint16_t>(plan, v210, hist, range);
}
} // namespace scopes
