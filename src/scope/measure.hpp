#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "scope/analyse.hpp"

namespace scopes
{
// Per histogram plane: the samples of each bin over all columns, summed by the trace step from
// the counts it draws (no extra pass over the picture, SPECIFICATION §4.7).
struct BinTotals
{
    int bins = 0;
    int planes = 0;
    std::array<std::vector<std::uint64_t>, 3> plane;
    void reset(int binCount, int planeCount);
};

// The measurements of one update (§4.7). A value the mode has no plane for is empty (null in the
// API): luma in `luma` and the YCbCr modes, chroma in the YCbCr modes, the gamut in the RGB
// modes. Luma and chroma come from the bins: a bin's value is its centre code, so they are exact
// to `codesPerBin` codes (1024 / bins: about 1 at 1080 lines, 6 at 180).
struct Measurements
{
    std::uint64_t update = 0;
    std::string mode;
    double codesPerBin = 1;
    std::optional<int> lumaMin;
    std::optional<int> lumaMax;
    std::optional<double> lumaMean;
    // Shares of the luma samples below 64 and above 940.
    std::optional<double> lumaBelow;
    std::optional<double> lumaAbove;
    // Share of the Cb and Cr samples outside 64..960.
    std::optional<double> chromaOut;
    // Share of the pixels with an R′G′B′ component outside codes 20..984 (−5 % to 105 %, EBU R 103),
    // counted exactly per pixel by the RGB analyse step.
    std::optional<double> rgbOutOfGamut;
    [[nodiscard]] bool operator==(Measurements const&) const = default;
};

// The centre code of bin `bin` of `bins` (codes v with (v × bins) >> 10 == bin).
int binCentreCode(int bin, int bins);
Measurements measure(ScopeKind kind, BinTotals const& totals, std::uint64_t outOfGamut, std::uint64_t pixels);
// {"update":..,"mode":..,"codes_per_bin":..,"luma":{..}|null,"chroma":{..}|null,"rgb":{..}|null}
std::string measurementsJson(Measurements const& m);
} // namespace scopes
