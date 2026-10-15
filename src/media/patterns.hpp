#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media/v210.hpp"

namespace scopes
{
struct Ycbcr10
{
    int y = 64;
    int cb = 512;
    int cr = 512;
};

// BT.709 narrow range, R′G′B′ in 0..1.
Ycbcr10 rgbToYcbcr709(double r, double g, double b);

// Test patterns for the unit tests, the benchmark and the pattern writer (SPECIFICATION §14):
// `bars75`, `bars100` (eight vertical bars, white to black), `ramp` (luma 64..940 left to
// right, neutral chroma), `flat` (luma `level`), `random` (every code 0..1023, seeded), `camera`
// (smooth picture-like luma).
Frame422 makePattern(std::string const& name, int width, int height, int level = 512, std::uint32_t seed = 1);
bool knownPattern(std::string const& name);
// The packed frame, `v210RowBytes(width)` per line.
std::vector<std::uint8_t> packPattern(Frame422 const& frame);
} // namespace scopes
