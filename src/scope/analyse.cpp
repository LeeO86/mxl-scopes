#include "scope/analyse.hpp"

#include "scope/cpu/group.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace scopes
{
void Histogram::reset(AnalysePlan const& plan)
{
    columns = plan.columns;
    bins = plan.bins;
    auto const cells = static_cast<std::size_t>(plan.columns) * static_cast<std::size_t>(plan.bins);
    counts.assign(cells, 0);
    scratch.assign(plan.twoSubs ? cells : 0, 0);
}

AnalysePlan makeAnalysePlan(int width, int height, std::size_t stride, int lineStep, int pixelStep, int columns, int bins, int stripColumns)
{
    if (width < 1 || height < 1 || columns < 1 || bins < 1 || bins > 1024 || stride < v210RowBytes(width) || lineStep < 1 || pixelStep < 1 ||
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
        // Whole pairs of groups, so the AVX2 engine takes every strip in 32-byte loads.
        int end = std::min(groups, g + 2);
        auto lastCol = [&](int group) { return col[static_cast<std::size_t>(std::min(width - 1, 6 * group + 5))]; };
        while (end < groups && lastCol(std::min(groups, end + 2) - 1) < strip.col0 + stripColumns)
        {
            end = std::min(groups, end + 2);
        }
        strip.g1 = end;
        strip.col1 = lastCol(end - 1);
        plan.strips.push_back(strip);
        g = end;
    }
    plan.simdOffA.assign(static_cast<std::size_t>(groups) * 4u + 8u, 0);
    plan.simdOffB.assign(static_cast<std::size_t>(groups) * 4u + 8u, 0);
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
    int perRow = 0;
    std::vector<std::uint32_t> perColumn(static_cast<std::size_t>(columns), 0);
    for (int x = 0; x < width; ++x)
    {
        if ((x % 6) % pixelStep == 0)
        {
            ++perRow;
            ++perColumn[static_cast<std::size_t>(col[static_cast<std::size_t>(x)])];
        }
    }
    auto const rows = static_cast<std::uint64_t>((height + lineStep - 1) / lineStep);
    plan.samples = static_cast<std::uint64_t>(perRow) * rows;
    auto const maxCell = static_cast<std::uint64_t>(*std::max_element(perColumn.begin(), perColumn.end())) * rows;
    if (maxCell > 65535)
    {
        throw std::invalid_argument("a histogram cell could exceed 16 bits (" + std::to_string(maxCell) + ")");
    }
    plan.maxCellCount = static_cast<std::uint32_t>(maxCell);
    return plan;
}

void analyseScalar(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist)
{
    auto* counts = hist.counts.data();
    auto* scratch = hist.scratch.data();
    for (auto const& strip : plan.strips)
    {
        for (int y = 0; y < plan.height; y += plan.lineStep)
        {
            auto const* row = v210 + static_cast<std::size_t>(y) * plan.stride;
            cpu::prefetchStrip(plan, strip, v210, y);
            for (int g = strip.g0; g < strip.g1; ++g)
            {
                cpu::analyseGroup(plan, row, g, counts, scratch);
            }
        }
        if (plan.twoSubs)
        {
            cpu::foldStrip(plan, strip, counts, scratch);
        }
    }
}
} // namespace scopes
