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
        int const sy = row * scaleDown;
        for (int col = 0; col < w; ++col)
        {
            int const sx = col * scaleDown;
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
    stbi_write_png_to_func(append, &out, w, h, 3, rgb.data(), w * 3);
    return out;
}
} // namespace scopes
