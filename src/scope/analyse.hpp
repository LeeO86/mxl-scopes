#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "media/v210.hpp"

namespace scopes
{
// Column-strip width in plot columns (SPECIFICATION §4.3). 512, not 64: the M0 numbers
// (IMPLEMENTATION_PLAN.md) show narrow strips cost more than their cache gain on real pictures.
inline constexpr int kDefaultStripColumns = 512;

// Everything the analyse step needs, built once per input format and settings change.
// Every backend uses the same plan, so the counts are the same (§3.2).
struct AnalysePlan
{
    int width = 0;
    int height = 0;
    std::size_t stride = 0;
    int lineStep = 1;
    int pixelStep = 1;
    // Histogram size: `columns` × `bins`, bin-major: the columns of one bin are contiguous, so
    // the renderer reads it line by line and a strip touches few cache lines per bin.
    int columns = 0;
    int bins = 0;
    // Two sub-histograms (even and odd input x) against store-forwarding stalls (§4.3).
    bool twoSubs = false;
    // Per input x: the column, floor(x × columns / width).
    std::vector<std::uint32_t> columnOf;
    // Per 10-bit code: the bin (code × bins) >> 10 (codes 0..1023 are the visible range,
    // §4.3) and its offset bin × columns. A cell is rowOffset[code] + columnOf[x].
    std::array<std::uint16_t, 1024> binOf{};
    std::array<std::uint32_t, 1024> rowOffset{};
    struct Strip
    {
        int g0 = 0;     // first v210 group
        int g1 = 0;     // one past the last group
        int col0 = 0;   // first column touched
        int col1 = 0;   // last column touched
    };
    std::vector<Strip> strips;
    // v210 groups whose six samples all lie inside the width.
    int fullGroups = 0;
    // AVX2 order of the columns: per group, lanes {0, 1, 3, 4} and {-, 2, -, 5}.
    std::vector<std::uint32_t> simdOffA;
    std::vector<std::uint32_t> simdOffB;
    // Samples analysed per frame and the largest possible count of one cell.
    std::uint64_t samples = 0;
    std::uint32_t maxCellCount = 0;
};

// Integer counts only (§3.2). `scratch` is the odd-x sub-histogram, all zero between calls.
struct Histogram
{
    int columns = 0;
    int bins = 0;
    std::vector<std::uint16_t> counts;
    std::vector<std::uint16_t> scratch;

    void reset(AnalysePlan const& plan);
    [[nodiscard]] std::uint16_t at(int column, int bin) const
    {
        return counts[static_cast<std::size_t>(bin) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column)];
    }
};

// Throws std::invalid_argument when the counts could overflow 16 bits.
AnalysePlan makeAnalysePlan(int width, int height, std::size_t stride, int lineStep, int pixelStep, int columns, int bins,
    int stripColumns = kDefaultStripColumns);

// Rows ahead of the current one that the analyse loops prefetch: a strip reads a few cache
// lines per row with a stride above 4 KiB, which the hardware prefetcher does not follow.
inline constexpr int kPrefetchRows = 6;

// Adds one frame to `hist`. The counts must be zero before the first frame of an update.
void analyseScalar(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist);
// AVX2 version, byte-identical counts. Only call it when the CPU has AVX2 (cpu/dispatch.hpp).
void analyseAvx2(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist);
bool avx2Compiled();
} // namespace scopes
