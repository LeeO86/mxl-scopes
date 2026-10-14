#include "media/patterns.hpp"

#include <algorithm>
#include <cmath>

namespace scopes
{
Ycbcr10 rgbToYcbcr709(double r, double g, double b)
{
    double const y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    double const cb = (b - y) / 1.8556;
    double const cr = (r - y) / 1.5748;
    auto const clamp10 = [](double v) { return std::clamp(static_cast<int>(std::lround(v)), 0, 1023); };
    return {clamp10(64.0 + 876.0 * y), clamp10(512.0 + 896.0 * cb), clamp10(512.0 + 896.0 * cr)};
}

bool knownPattern(std::string const& name)
{
    return name == "bars75" || name == "bars100" || name == "ramp" || name == "flat" || name == "random";
}

Frame422 makePattern(std::string const& name, int width, int height, int level, std::uint32_t seed)
{
    Frame422 frame;
    frame.allocate(width, height);
    int const cw = frame.chromaWidth();
    if (name == "bars75" || name == "bars100")
    {
        double const a = name == "bars75" ? 0.75 : 1.0;
        // White, yellow, cyan, green, magenta, red, blue, black.
        double const rgb[8][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0}};
        for (int x = 0; x < width; ++x)
        {
            int const bar = std::min(7, x * 8 / std::max(1, width));
            auto const c = rgbToYcbcr709(a * rgb[bar][0], a * rgb[bar][1], a * rgb[bar][2]);
            for (int row = 0; row < height; ++row)
            {
                frame.y[static_cast<std::size_t>(row) * width + x] = static_cast<std::uint16_t>(c.y);
                if (x % 2 == 0 && x / 2 < cw)
                {
                    frame.cb[static_cast<std::size_t>(row) * cw + x / 2] = static_cast<std::uint16_t>(c.cb);
                    frame.cr[static_cast<std::size_t>(row) * cw + x / 2] = static_cast<std::uint16_t>(c.cr);
                }
            }
        }
    }
    else if (name == "ramp")
    {
        for (int x = 0; x < width; ++x)
        {
            auto const v = static_cast<std::uint16_t>(width > 1 ? 64 + (876 * x) / (width - 1) : 64);
            for (int row = 0; row < height; ++row)
            {
                frame.y[static_cast<std::size_t>(row) * width + x] = v;
            }
        }
    }
    else if (name == "flat")
    {
        frame.fill(static_cast<std::uint16_t>(std::clamp(level, 0, 1023)), 512, 512);
    }
    else if (name == "random")
    {
        std::uint32_t state = seed == 0 ? 1u : seed;
        auto next = [&] {
            // xorshift32: reproducible on every platform.
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return static_cast<std::uint16_t>(state & 0x3ffu);
        };
        for (auto& v : frame.y)
        {
            v = next();
        }
        for (auto& v : frame.cb)
        {
            v = next();
        }
        for (auto& v : frame.cr)
        {
            v = next();
        }
    }
    return frame;
}

std::vector<std::uint8_t> packPattern(Frame422 const& frame)
{
    std::vector<std::uint8_t> out(v210RowBytes(frame.width) * static_cast<std::size_t>(frame.height));
    packV210(frame, out.data(), v210RowBytes(frame.width));
    return out;
}
} // namespace scopes
