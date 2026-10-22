// The key output (SPECIFICATION §7, video/v210a): the key plane after the fill, three 10-bit
// samples per little-endian word; background at SCOPES_BG_ALPHA, a trace at max(BG_ALPHA, I),
// the graticule at its alpha, texts opaque; the fill is the picture without a key.
#include <doctest/doctest.h>

#include "media/patterns.hpp"
#include "scope/engine.hpp"

#include <cmath>
#include <cstring>
#include <vector>

using namespace scopes;

namespace
{
std::uint16_t keyAt(std::vector<std::uint8_t> const& packed, int width, int height, int x, int y)
{
    auto const at = v210RowBytes(width) * static_cast<std::size_t>(height) + static_cast<std::size_t>(y) * alphaRowBytes(width) + 4 * static_cast<std::size_t>(x / 3);
    std::uint32_t word = 0;
    std::memcpy(&word, packed.data() + at, 4);
    return static_cast<std::uint16_t>((word >> (10 * (x % 3))) & 0x3ffu);
}

std::vector<std::uint8_t> renderOnce(ScopeEngine& engine, std::vector<std::uint8_t> const& frame, std::string const& state = "")
{
    std::vector<std::uint8_t> out(engine.packedSize());
    engine.analyse(frame.data());
    engine.render("Caption", state, out.data());
    return out;
}
} // namespace

TEST_CASE("key plane packing: three samples per word, MXL layout")
{
    Frame422 frame;
    frame.allocate(8, 2);
    frame.a.assign(16, 0);
    for (int i = 0; i < 16; ++i)
    {
        frame.a[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(100 * i + 1);
    }
    CHECK(alphaRowBytes(8) == 12);
    CHECK(alphaRowBytes(1920) == 2560);
    std::vector<std::uint8_t> out(2 * alphaRowBytes(8), 0xff);
    packAlpha(frame, out.data());
    std::uint32_t w0 = 0;
    std::memcpy(&w0, out.data(), 4);
    CHECK(w0 == (1u | (101u << 10) | (201u << 20)));
    std::uint32_t last = 0;
    std::memcpy(&last, out.data() + 8, 4); // x = 6, 7 and padding
    CHECK(last == (601u | (701u << 10)));
    std::uint32_t second = 0;
    std::memcpy(&second, out.data() + 12, 4); // row 1
    CHECK(second == (801u | (901u << 10) | (1001u << 20)));
}

TEST_CASE("key output: fill unchanged, background, trace, graticule and texts in the key")
{
    auto const grey = packPattern(makePattern("flat", 1920, 1080, 512));
    auto const scene = packPattern(makePattern("scene", 1920, 1080, 512));
    for (auto const* mode : {"luma", "ycbcr-parade", "rgb-parade", "rgb-overlay"})
    {
        for (int bg : {0, 300})
        {
            CAPTURE(mode);
            CAPTURE(bg);
            DisplaySettings settings;
            settings.mode = mode;
            settings.bgAlpha = bg;
            ScopeEngine plain(SimdLevel::Scalar);
            ScopeEngine keyed(SimdLevel::Scalar);
            plain.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080);
            keyed.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080, true);
            auto const fillBytes = v210RowBytes(1920) * 1080u;
            REQUIRE(keyed.packedSize() == fillBytes + alphaRowBytes(1920) * 1080u);
            REQUIRE(plain.packedSize() == fillBytes);
            for (auto const* frame : {&grey, &scene})
            {
                auto const a = renderOnce(plain, *frame);
                auto const b = renderOnce(keyed, *frame);
                CHECK(std::memcmp(a.data(), b.data(), fillBytes) == 0);
            }
            auto const out = renderOnce(keyed, grey);
            auto const& plan = keyed.renderPlan();
            auto const& layout = plan.layout;
            // Background: outside the plot (right margin) and inside it away from lines.
            CHECK(keyAt(out, 1920, 1080, 1918, 540) == bg);
            // The grey's trace row: max(bg, intensity) where every cell of a column is hit.
            int const row = rowOfCode(plan, 512);
            int const x = layout.panelX[0] + layout.panelW / 2 + 1;
            auto const level = plan.planeIntensity[0].back();
            CHECK(keyAt(out, 1920, 1080, x, row) == std::max<int>(bg, level));
            // A solid graticule line (code 64 in luma, the chroma ends in a chroma panel).
            int const line = plan.lines.front().row;
            CHECK(keyAt(out, 1920, 1080, x, line) >= bg);
            (void)line;
            // The caption is opaque.
            bool opaqueText = false;
            for (int yy = 0; yy < layout.plotY && !opaqueText; ++yy)
            {
                for (int xx = layout.plotX; xx < layout.plotX + 200 && !opaqueText; ++xx)
                {
                    opaqueText = keyAt(out, 1920, 1080, xx, yy) == 1023;
                }
            }
            CHECK(opaqueText);
            // NO SIGNAL: no trace in the key either.
            auto const state = renderOnce(keyed, grey, "NO SIGNAL");
            CHECK(keyAt(state, 1920, 1080, x, row) == bg);
        }
    }
}

TEST_CASE("key output: graticule alpha follows the level; the copy-free trace equals a render")
{
    DisplaySettings settings;
    settings.graticuleLevel = 60;
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080, true);
    auto const blank = packPattern(makePattern("flat", 1920, 1080, 940));
    auto const out = renderOnce(engine, blank);
    auto const& plan = engine.renderPlan();
    // The solid 0 mV line (code 64) away from the trace: alpha × level.
    int const row = rowOfCode(plan, 64);
    int const x = plan.layout.panelX[0] + 7;
    CHECK(keyAt(out, 1920, 1080, x, row) == static_cast<int>(std::lround(1023.0 * 60 / 100)));
    // Trace groups over the still picture give the rendered bytes, key included.
    auto const scene = packPattern(makePattern("scene", 1920, 1080, 512));
    std::vector<TraceGroup> trace;
    engine.analyse(scene.data());
    engine.trace("Caption", "", trace);
    std::vector<std::uint8_t> composed(engine.stillShared()->begin(), engine.stillShared()->end());
    applyTrace(trace, composed.data());
    auto const rendered = renderOnce(engine, scene);
    CHECK(composed == rendered);
    for (auto const& group : trace)
    {
        REQUIRE(group.keyOffset >= v210RowBytes(1920) * 1080u);
    }
    // Strip workers: the same bytes with a key.
    ScopeEngine threaded(SimdLevel::Scalar, kDefaultStripColumns, 3);
    threaded.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080, true);
    CHECK(renderOnce(threaded, scene) == rendered);
}
