#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scopes
{
// Row stride of a v210 line: ceil(width / 48) × 128 bytes (SPECIFICATION §4.1).
constexpr std::size_t v210RowBytes(int width)
{
    return static_cast<std::size_t>((width + 47) / 48) * 128u;
}

// Planar 10-bit 4:2:2 picture: `width` luma and width/2 chroma samples per line.
struct Frame422
{
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> y;
    std::vector<std::uint16_t> cb;
    std::vector<std::uint16_t> cr;

    void allocate(int w, int h);
    void fill(std::uint16_t yValue, std::uint16_t cbValue, std::uint16_t crValue);
    [[nodiscard]] int chromaWidth() const
    {
        return width / 2;
    }
};

// The six luma samples of one 16-byte v210 group.
inline void unpackGroupLuma(std::uint8_t const* group, std::uint16_t out[6])
{
    std::uint32_t w[4];
    for (int i = 0; i < 4; ++i)
    {
        w[i] = static_cast<std::uint32_t>(group[4 * i]) | (static_cast<std::uint32_t>(group[4 * i + 1]) << 8) |
               (static_cast<std::uint32_t>(group[4 * i + 2]) << 16) | (static_cast<std::uint32_t>(group[4 * i + 3]) << 24);
    }
    out[0] = static_cast<std::uint16_t>((w[0] >> 10) & 0x3ffu);
    out[1] = static_cast<std::uint16_t>(w[1] & 0x3ffu);
    out[2] = static_cast<std::uint16_t>((w[1] >> 20) & 0x3ffu);
    out[3] = static_cast<std::uint16_t>((w[2] >> 10) & 0x3ffu);
    out[4] = static_cast<std::uint16_t>(w[3] & 0x3ffu);
    out[5] = static_cast<std::uint16_t>((w[3] >> 20) & 0x3ffu);
}

// Packs one line of `width` luma and width/2 chroma samples; the padding up to rowBytes is zero.
void packV210Line(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int width, std::uint8_t* line, std::size_t rowBytes);
// Packs a whole frame; the padding of each line is zero.
void packV210(Frame422 const& src, std::uint8_t* dst, std::size_t dstRowBytes);
void unpackV210(std::uint8_t const* src, std::size_t srcRowBytes, Frame422& dst);
} // namespace scopes
