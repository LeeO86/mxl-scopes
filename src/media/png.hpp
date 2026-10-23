#pragma once

#include <cstdint>
#include <string>

#include "media/v210.hpp"

namespace scopes
{
// The picture as an 8-bit RGB PNG (BT.709 narrow range to full-range R′G′B′), for
// GET /api/v1/snapshot.png. `scaleDown` 1 keeps the raster, 2 halves it (brightest pixel of
// each 2×2 block), ... With a key plane (`frame.a`) an RGBA PNG, the key as straight alpha.
std::string encodePng(Frame422 const& frame, int scaleDown = 1);
} // namespace scopes
