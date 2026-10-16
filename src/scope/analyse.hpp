#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "media/v210.hpp"

namespace scopes
{
// Column-strip width in plot columns (SPECIFICATION §4.3), fixed at 64 by the M0 numbers
// (IMPLEMENTATION_PLAN.md): best or within 6 % of the best of 64, 128 and 512 in every case.
inline constexpr int kDefaultStripColumns = 64;

// What the analyse step counts (SPECIFICATION §5.1). `Luma`: one plane, Y′. `Ycbcr`: three
// planes, Y′, Cb, Cr (the chroma samples at their co-sited luma x). `Rgb`: three planes, R′, G′,
// B′ from each luma sample and its co-sited chroma pair (§4.8). A parade shows the planes side
// by side, an overlay in one panel; the counts are the same.
enum class ScopeKind
{
    Luma,
    Ycbcr,
    Rgb,
};

ScopeKind scopeKindOf(std::string const& mode);
inline int planesOf(ScopeKind kind)
{
    return kind == ScopeKind::Luma ? 1 : 3;
}

// §4.8: BT.709 narrow range Y′CbCr -> R′G′B′ in the Y code scale, Q13 coefficients, rounded,
// not clamped (the unclamped value is the gamut measurement's). The SIMD code uses the same
// integer formula, so every path is bit-exact.
inline constexpr int kQ13CrR = 12613;
inline constexpr int kQ13CbG = 1500;
inline constexpr int kQ13CrG = 3749;
inline constexpr int kQ13CbB = 14862;
static inline void rgbOfYcbcr(int y, int cb, int cr, int& r, int& g, int& b)
{
    int const yy = y << 13;
    int const db = cb - 512;
    int const dr = cr - 512;
    r = (yy + kQ13CrR * dr + 4096) >> 13;
    g = (yy - kQ13CbG * db - kQ13CrG * dr + 4096) >> 13;
    b = (yy + kQ13CbB * db + 4096) >> 13;
}

// Everything the analyse step needs, built once per input format and settings change.
// Every backend uses the same plan, so the counts are the same (§3.2).
struct AnalysePlan
{
    int width = 0;
    int height = 0;
    std::size_t stride = 0;
    int lineStep = 1;
    int pixelStep = 1;
    ScopeKind kind = ScopeKind::Luma;
    // Planes of `columns` × `bins` cells each, one after the other (Y′ or Y′ Cb Cr or R′ G′ B′).
    int planes = 1;
    std::size_t planeCells = 0;
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
    // SIMD order of the columns: per group, lanes {0, 1, 3, 4} and {-, 2, -, 5} (AVX2 loads two
    // groups, AVX-512 four).
    std::vector<std::uint32_t> simdOffA;
    std::vector<std::uint32_t> simdOffB;
    // `Ycbcr`: per group and v210 word (4 entries a group) the plane offset plus the column of
    // the sample at bit 0, 10 and 20 of the word (Cb0 Y1 Cr1 Y4, Y0 Cb1 Y3 Cr2, Cr0 Y2 Cb2 Y5).
    std::vector<std::uint32_t> simdOffW0;
    std::vector<std::uint32_t> simdOffW10;
    std::vector<std::uint32_t> simdOffW20;
    // Samples analysed per frame and the largest possible count of one cell, per plane; `samples`
    // and `maxCellCount` are plane 0's and the largest of all planes.
    std::array<std::uint64_t, 3> planeSamples{};
    std::array<std::uint32_t, 3> planeMaxCell{};
    std::uint64_t samples = 0;
    std::uint32_t maxCellCount = 0;
};

// Integer counts only (§3.2). `scratch` is the odd-x sub-histogram, all zero between calls.
struct Histogram
{
    int columns = 0;
    int bins = 0;
    int planes = 1;
    std::vector<std::uint16_t> counts;
    std::vector<std::uint16_t> scratch;

    void reset(AnalysePlan const& plan);
    [[nodiscard]] std::uint16_t at(int column, int bin, int plane = 0) const
    {
        return counts[(static_cast<std::size_t>(plane) * static_cast<std::size_t>(bins) + static_cast<std::size_t>(bin)) * static_cast<std::size_t>(columns) +
                      static_cast<std::size_t>(column)];
    }
};

// Throws std::invalid_argument when the counts could overflow 16 bits.
AnalysePlan makeAnalysePlan(int width, int height, std::size_t stride, int lineStep, int pixelStep, int columns, int bins,
    int stripColumns = kDefaultStripColumns, ScopeKind kind = ScopeKind::Luma);

// Rows ahead of the current one that the analyse loops prefetch: a strip reads a few cache
// lines per row with a stride above 4 KiB, which the hardware prefetcher does not follow.
inline constexpr int kPrefetchRows = 6;

// Adds one frame to `hist` (every kind; the plan says which). The counts must be zero before the first frame of an update.
void analyseScalar(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist);
// AVX2 version, byte-identical counts. Only call it when the CPU has AVX2 (cpu/dispatch.hpp).
void analyseAvx2(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist);
bool avx2Compiled();
// AVX-512 (F) version, four groups per load, byte-identical counts. Only with AVX-512.
void analyseAvx512(AnalysePlan const& plan, std::uint8_t const* v210, Histogram& hist);
bool avx512Compiled();
} // namespace scopes
