#include "media/png.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb/stb_image_write.h>

#include <algorithm>
#include <vector>

namespace scopes
{
namespace
{
void append(void* context, void* data, int size)
{
    static_cast<std::string*>(context)->append(static_cast<char const*>(data), static_cast<std::size_t>(size));
}

std::uint8_t clamp8(int value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}
} // namespace

std::string encodePng(Frame422 const& frame, int scaleDown)
{
    scaleDown = std::max(1, scaleDown);
    int const w = frame.width / scaleDown;
    int const h = frame.height / scaleDown;
    if (w < 1 || h < 1)
    {
        return {};
    }
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    int const cw = frame.chromaWidth();
    for (int row = 0; row < h; ++row)
    {
        for (int col = 0; col < w; ++col)
        {
            // Smaller pictures keep the brightest pixel of each block, so one-pixel traces and
            // graticule lines stay visible.
            int sx = col * scaleDown;
            int sy = row * scaleDown;
            int best = -1;
            for (int dy = 0; dy < scaleDown; ++dy)
            {
                for (int dx = 0; dx < scaleDown; ++dx)
                {
                    int const x = col * scaleDown + dx;
                    int const y = row * scaleDown + dy;
                    int const luma = frame.y[static_cast<std::size_t>(y) * frame.width + x];
                    if (luma > best)
                    {
                        best = luma;
                        sx = x;
                        sy = y;
                    }
                }
            }
            // BT.709 narrow range, in 1/1024ths: R = Y' + 1.5748 Cr, G = Y' - 0.1873 Cb - 0.4681 Cr, B = Y' + 1.8556 Cb.
            int const y = (frame.y[static_cast<std::size_t>(sy) * frame.width + sx] - 64) * 1024 / 876;
            int const cb = (frame.cb[static_cast<std::size_t>(sy) * cw + sx / 2] - 512) * 1024 / 896;
            int const cr = (frame.cr[static_cast<std::size_t>(sy) * cw + sx / 2] - 512) * 1024 / 896;
            int const r = y + (1613 * cr) / 1024;
            int const g = y - (192 * cb + 479 * cr) / 1024;
            int const b = y + (1900 * cb) / 1024;
            auto* px = rgb.data() + (static_cast<std::size_t>(row) * w + col) * 3;
            px[0] = clamp8(r * 255 / 1024);
            px[1] = clamp8(g * 255 / 1024);
            px[2] = clamp8(b * 255 / 1024);
        }
    }
    std::string out;
    // Fast deflate: the picture is mostly flat background; level 1 is several times quicker
    // than the default and the file only a little larger.
    stbi_write_png_compression_level = 1;
    stbi_write_force_png_filter = 0;
    stbi_write_png_to_func(append, &out, w, h, 3, rgb.data(), w * 3);
    return out;
}
} // namespace scopes
