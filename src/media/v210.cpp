#include "media/v210.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace scopes
{
namespace
{
inline void put32(std::uint8_t* dst, std::uint32_t word)
{
    if constexpr (std::endian::native == std::endian::little)
    {
        std::memcpy(dst, &word, sizeof(word));
    }
    else
    {
        dst[0] = static_cast<std::uint8_t>(word & 0xff);
        dst[1] = static_cast<std::uint8_t>((word >> 8) & 0xff);
        dst[2] = static_cast<std::uint8_t>((word >> 16) & 0xff);
        dst[3] = static_cast<std::uint8_t>((word >> 24) & 0xff);
    }
}

inline std::uint32_t get32(std::uint8_t const* src)
{
    if constexpr (std::endian::native == std::endian::little)
    {
        std::uint32_t word;
        std::memcpy(&word, src, sizeof(word));
        return word;
    }
    return static_cast<std::uint32_t>(src[0]) | (static_cast<std::uint32_t>(src[1]) << 8) | (static_cast<std::uint32_t>(src[2]) << 16) |
           (static_cast<std::uint32_t>(src[3]) << 24);
}

inline std::uint32_t word3(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    return (a & 0x3ffu) | ((b & 0x3ffu) << 10) | ((c & 0x3ffu) << 20);
}
} // namespace

void Frame422::allocate(int w, int h)
{
    width = w;
    height = h;
    y.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 64);
    cb.assign(static_cast<std::size_t>(w / 2) * static_cast<std::size_t>(h), 512);
    cr.assign(static_cast<std::size_t>(w / 2) * static_cast<std::size_t>(h), 512);
}

void Frame422::fill(std::uint16_t yValue, std::uint16_t cbValue, std::uint16_t crValue)
{
    std::fill(y.begin(), y.end(), yValue);
    std::fill(cb.begin(), cb.end(), cbValue);
    std::fill(cr.begin(), cr.end(), crValue);
}

void packV210Line(std::uint16_t const* ys, std::uint16_t const* cbs, std::uint16_t const* crs, int width, std::uint8_t* line, std::size_t rowBytes)
{
    int const full = width / 6;
    std::uint8_t* out = line;
    for (int g = 0; g < full; ++g, out += 16)
    {
        auto const* y = ys + 6 * g;
        auto const* b = cbs + 3 * g;
        auto const* r = crs + 3 * g;
        put32(out, word3(b[0], y[0], r[0]));
        put32(out + 4, word3(y[1], b[1], y[2]));
        put32(out + 8, word3(r[1], y[3], b[2]));
        put32(out + 12, word3(y[4], r[2], y[5]));
    }
    if (width % 6 != 0)
    {
        // The short last group: missing samples are zero.
        std::uint16_t y[6] = {};
        std::uint16_t b[3] = {};
        std::uint16_t r[3] = {};
        for (int i = 0; i < width % 6; ++i)
        {
            y[i] = ys[6 * full + i];
        }
        for (int i = 0; i < (width % 6) / 2; ++i)
        {
            b[i] = cbs[3 * full + i];
            r[i] = crs[3 * full + i];
        }
        put32(out, word3(b[0], y[0], r[0]));
        put32(out + 4, word3(y[1], b[1], y[2]));
        put32(out + 8, word3(r[1], y[3], b[2]));
        put32(out + 12, word3(y[4], r[2], y[5]));
        out += 16;
    }
    auto const used = static_cast<std::size_t>(out - line);
    if (used < rowBytes)
    {
        std::memset(out, 0, rowBytes - used);
    }
}

void packV210(Frame422 const& src, std::uint8_t* dst, std::size_t dstRowBytes)
{
    int const cw = src.chromaWidth();
    for (int row = 0; row < src.height; ++row)
    {
        packV210Line(src.y.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(src.width),
            src.cb.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(cw), src.cr.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(cw),
            src.width, dst + static_cast<std::size_t>(row) * dstRowBytes, dstRowBytes);
    }
}

void unpackV210(std::uint8_t const* src, std::size_t srcRowBytes, Frame422& dst)
{
    int const width = dst.width;
    int const cw = dst.chromaWidth();
    for (int row = 0; row < dst.height; ++row)
    {
        auto const* line = src + static_cast<std::size_t>(row) * srcRowBytes;
        auto* ys = dst.y.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
        auto* cbs = dst.cb.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(cw);
        auto* crs = dst.cr.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(cw);
        int const groups = (width + 5) / 6;
        for (int g = 0; g < groups; ++g)
        {
            auto const* in = line + 16 * g;
            std::uint32_t const w0 = get32(in);
            std::uint32_t const w1 = get32(in + 4);
            std::uint32_t const w2 = get32(in + 8);
            std::uint32_t const w3 = get32(in + 12);
            std::uint16_t const y[6] = {static_cast<std::uint16_t>((w0 >> 10) & 0x3ff), static_cast<std::uint16_t>(w1 & 0x3ff),
                static_cast<std::uint16_t>((w1 >> 20) & 0x3ff), static_cast<std::uint16_t>((w2 >> 10) & 0x3ff), static_cast<std::uint16_t>(w3 & 0x3ff),
                static_cast<std::uint16_t>((w3 >> 20) & 0x3ff)};
            std::uint16_t const b[3] = {static_cast<std::uint16_t>(w0 & 0x3ff), static_cast<std::uint16_t>((w1 >> 10) & 0x3ff),
                static_cast<std::uint16_t>((w2 >> 20) & 0x3ff)};
            std::uint16_t const r[3] = {static_cast<std::uint16_t>((w0 >> 20) & 0x3ff), static_cast<std::uint16_t>(w2 & 0x3ff),
                static_cast<std::uint16_t>((w3 >> 10) & 0x3ff)};
            for (int i = 0; i < 6 && 6 * g + i < width; ++i)
            {
                ys[6 * g + i] = y[i];
            }
            for (int i = 0; i < 3 && 3 * g + i < cw; ++i)
            {
                cbs[3 * g + i] = b[i];
                crs[3 * g + i] = r[i];
            }
        }
    }
}
} // namespace scopes
