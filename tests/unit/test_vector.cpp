// The vectorscope (SPECIFICATION §5.3) and the picture trace colour (§4.4): layout, the Cb/Cr
// cells and the zoom, SIMD against scalar (counts, sums, pictures), the graticule, the colour
// rule, the key and the golden pictures.
#include <doctest/doctest.h>

#include "media/patterns.hpp"
#include "scope/analyse.hpp"
#include "scope/cpu/dispatch.hpp"
#include "scope/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace scopes;

namespace
{
bool haveAvx2()
{
    return avx2Compiled() && cpuHasAvx2();
}

bool haveAvx512()
{
    return avx512Compiled() && cpuHasAvx512();
}

std::uint64_t fnv1a(std::vector<std::uint8_t> const& bytes)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (auto b : bytes)
    {
        hash = (hash ^ b) * 1099511628211ull;
    }
    return hash;
}

std::vector<std::uint8_t> flat(int y, int cb, int cr, int width = 1920, int height = 1080)
{
    Frame422 frame;
    frame.allocate(width, height);
    frame.fill(static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(cb), static_cast<std::uint16_t>(cr));
    return packPattern(frame);
}

DisplaySettings vectorSettings(int zoom = 1, char const* colour = "green", int level = 0)
{
    DisplaySettings settings;
    settings.mode = "vectorscope";
    settings.vectorZoom = zoom;
    settings.traceColor = colour;
    settings.graticuleLevel = level;
    return settings;
}

Frame422 renderWith(ScopeEngine& engine, std::vector<std::uint8_t> const& frame, int width = 1920, int height = 1080)
{
    std::vector<std::uint8_t> out(engine.packedSize());
    engine.analyse(frame.data());
    engine.render("", "", out.data());
    Frame422 picture;
    picture.allocate(width, height);
    unpackV210(out.data(), v210RowBytes(width), picture);
    return picture;
}

// The first output pixel (x, row) that shows the vectorscope cell of (cb, cr); false outside.
bool pixelOf(ScopeEngine const& engine, int cb, int cr, int& x, int& y)
{
    auto const& a = engine.analysePlan();
    auto const& r = engine.renderPlan();
    auto const cell = std::min(a.rowOffset[static_cast<std::size_t>(cr)] + a.chromaColumn[static_cast<std::size_t>(cb)], a.trashCell);
    if (cell == a.trashCell)
    {
        return false;
    }
    auto const column = static_cast<std::uint32_t>(cell % static_cast<std::uint32_t>(a.columns));
    auto const bin = static_cast<int>(cell / static_cast<std::uint32_t>(a.columns));
    x = -1;
    y = -1;
    for (int px = 0; px < r.layout.panelW && x < 0; ++px)
    {
        x = r.columnOf[static_cast<std::size_t>(px)] == column ? r.layout.plotX + px : -1;
    }
    for (int row = 0; row < r.layout.plotH && y < 0; ++row)
    {
        y = r.binOfRow[static_cast<std::size_t>(row)] == bin ? r.layout.plotY + row : -1;
    }
    return x >= 0 && y >= 0;
}

int lumaAt(Frame422 const& p, int x, int y)
{
    return p.y[static_cast<std::size_t>(y) * static_cast<std::size_t>(p.width) + static_cast<std::size_t>(x)];
}

int cbAt(Frame422 const& p, int x, int y)
{
    return p.cb[static_cast<std::size_t>(y) * static_cast<std::size_t>(p.chromaWidth()) + static_cast<std::size_t>(x / 2)];
}

int crAt(Frame422 const& p, int x, int y)
{
    return p.cr[static_cast<std::size_t>(y) * static_cast<std::size_t>(p.chromaWidth()) + static_cast<std::size_t>(x / 2)];
}

// Plot pixels above the background.
int litPixels(Frame422 const& p, Layout const& layout)
{
    int lit = 0;
    for (int y = layout.plotY; y < layout.plotY + layout.plotH; ++y)
    {
        for (int x = layout.plotX; x < layout.plotX + layout.plotW; ++x)
        {
            lit += lumaAt(p, x, y) > kBackgroundY + 8 ? 1 : 0;
        }
    }
    return lit;
}

// Every SIMD level gives the scalar counts and sums, with the plan's cells and the other width.
void compareVector(AnalysePlan const& given, std::vector<std::uint8_t> const& frame)
{
    for (bool const wideCells : {given.wide, !given.wide})
    {
        CAPTURE(wideCells);
        auto plan = given;
        plan.wide = wideCells;
        Histogram scalar;
        scalar.reset(plan);
        analyseScalar(plan, frame.data(), scalar);
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < scalar.cells() + scalar.extra; ++i)
        {
            total += wideCells ? scalar.counts32[i] : scalar.counts[i];
        }
        CHECK(total == plan.planeSamples[0]);
        for (auto level : {SimdLevel::Avx2, SimdLevel::Avx512})
        {
            if ((level == SimdLevel::Avx2 && !haveAvx2()) || (level == SimdLevel::Avx512 && !haveAvx512()))
            {
                continue;
            }
            Histogram simd;
            simd.reset(plan);
            if (level == SimdLevel::Avx2)
            {
                analyseAvx2(plan, frame.data(), simd);
            }
            else
            {
                analyseAvx512(plan, frame.data(), simd);
            }
            REQUIRE(simd.counts == scalar.counts);
            REQUIRE(simd.counts32 == scalar.counts32);
            REQUIRE(simd.sums == scalar.sums);
        }
    }
}
} // namespace

TEST_CASE("vectorscope: kind, title, a square plot in the middle, the caption at the margin")
{
    CHECK(scopeKindOf("vectorscope") == ScopeKind::Vector);
    CHECK(planesOf(ScopeKind::Vector) == 1);
    CHECK(panelCount("vectorscope") == 1);
    CHECK(modeTitle("vectorscope") == "Vectorscope");
    for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{960, 540}, std::pair{320, 180}, std::pair{3840, 2160}})
    {
        CAPTURE(w);
        auto const wave = makeLayout(w, h);
        auto const square = makeModeLayout(w, h, "vectorscope");
        CHECK(square.square);
        CHECK(square.plotW == square.plotH);
        CHECK(square.plotW % 6 == 0);
        CHECK(square.plotX % 6 == 0);
        CHECK(square.panelW == square.plotW);
        CHECK(std::abs(square.plotX + square.plotW / 2 - w / 2) <= 6);
        CHECK(square.plotY >= wave.plotY);
        CHECK(square.plotY + square.plotH <= wave.plotY + wave.plotH);
        CHECK(square.plotH > wave.plotH - 6);
        CHECK(square.captionX == wave.plotX);
        CHECK(histogramColumns(square, 1920) == std::min(square.plotW / 2, 1024));
        CHECK(histogramBins(square) == histogramColumns(square, 0));
        CHECK_FALSE(makeModeLayout(w, h, "luma").square);
    }
}

TEST_CASE("vectorscope: SIMD equals scalar, counts and sums, every step, zoom and width")
{
    for (int zoom : {1, 2, 5})
    {
        for (bool picture : {false, true})
        {
            for (int lineStep : {1, 2})
            {
                for (int pixelStep : {1, 2, 3, 6})
                {
                    CAPTURE(zoom);
                    CAPTURE(picture);
                    CAPTURE(lineStep);
                    CAPTURE(pixelStep);
                    auto const frame = packPattern(makePattern("random", 1920, 32, 0, 41));
                    compareVector(makeAnalysePlan(1920, 32, v210RowBytes(1920), lineStep, pixelStep, 495, 495, kDefaultStripColumns, ScopeKind::Vector, 1, 1, zoom, picture),
                        frame);
                }
            }
            for (int width : {1, 5, 7, 13, 47, 49, 1001})
            {
                CAPTURE(width);
                auto const frame = packPattern(makePattern("random", width, 9, 0, static_cast<std::uint32_t>(width)));
                compareVector(makeAnalysePlan(width, 9, v210RowBytes(width), 1, 1, 37, 37, 1, ScopeKind::Vector, 1, 1, zoom, picture), frame);
            }
        }
    }
    // The luma picture colour (the AVX2 step with its sums).
    for (int pixelStep : {1, 2, 3, 6})
    {
        for (int columns : {522, 1728})
        {
            CAPTURE(pixelStep);
            CAPTURE(columns);
            auto const plan = makeAnalysePlan(1920, 32, v210RowBytes(1920), 1, pixelStep, columns, 994, kDefaultStripColumns, ScopeKind::Luma, 1, 1, 1, true);
            REQUIRE(plan.sums == 2);
            Histogram scalar;
            scalar.reset(plan);
            auto const frame = packPattern(makePattern("random", 1920, 32, 0, 43));
            analyseScalar(plan, frame.data(), scalar);
            if (haveAvx2())
            {
                Histogram simd;
                simd.reset(plan);
                analyseAvx2(plan, frame.data(), simd);
                CHECK(simd.counts == scalar.counts);
                CHECK(simd.sums == scalar.sums);
            }
            if (haveAvx512())
            {
                Histogram simd;
                simd.reset(plan);
                analyseAvx512(plan, frame.data(), simd);
                CHECK(simd.counts == scalar.counts);
                CHECK(simd.sums == scalar.sums);
            }
        }
    }
}

TEST_CASE("vectorscope: every code fits at 1x; outside the zoomed plot a sample goes to the trash cell")
{
    auto const frame = packPattern(makePattern("random", 1920, 32, 0, 45));
    for (int zoom : {1, 5})
    {
        auto const plan = makeAnalysePlan(1920, 32, v210RowBytes(1920), 1, 1, 495, 495, kDefaultStripColumns, ScopeKind::Vector, 1, 1, zoom);
        CHECK(plan.trashCell == plan.planeCells);
        Histogram hist;
        hist.reset(plan);
        CHECK(hist.cells() == plan.planeCells);
        CHECK(hist.extra == 1);
        analyseScalar(plan, frame.data(), hist);
        auto const trash = plan.wide ? hist.counts32[plan.trashCell] : hist.counts[plan.trashCell];
        if (zoom == 1)
        {
            CHECK(trash == 0);
        }
        else
        {
            CHECK(trash > 0);
        }
    }
}

TEST_CASE("vectorscope: a flat colour is one 2x2 dot at its Cb and Cr; the zoom scales its distance")
{
    auto const frame = flat(500, 600, 450);
    int x1 = 0;
    int y1 = 0;
    int cx = 0;
    int cy = 0;
    for (int zoom : {1, 2, 5})
    {
        CAPTURE(zoom);
        ScopeEngine engine(SimdLevel::Scalar);
        engine.configure({1920, 1080, v210RowBytes(1920)}, vectorSettings(zoom), 1920, 1080);
        auto const picture = renderWith(engine, frame);
        int x = 0;
        int y = 0;
        REQUIRE(pixelOf(engine, 600, 450, x, y));
        CHECK(lumaAt(picture, x, y) > kBackgroundY + 8);
        CHECK(litPixels(picture, engine.renderPlan().layout) == 4);
        int zx = 0;
        int zy = 0;
        REQUIRE(pixelOf(engine, 512, 512, zx, zy));
        if (zoom == 1)
        {
            x1 = x;
            y1 = y;
            cx = zx;
            cy = zy;
            // Cb right, Cr up.
            CHECK(x > zx);
            CHECK(y > zy);
        }
        else
        {
            CHECK(zx == cx);
            CHECK(zy == cy);
            CHECK(std::abs((x - cx) - zoom * (x1 - cx)) <= 2 * zoom);
            CHECK(std::abs((y - cy) - zoom * (y1 - cy)) <= 2 * zoom);
        }
    }
    // Outside the plot at 5x: no trace at all, and the counts (trash included) are clear after it.
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920)}, vectorSettings(5), 1920, 1080);
    int x = 0;
    int y = 0;
    CHECK_FALSE(pixelOf(engine, 900, 512, x, y));
    auto const picture = renderWith(engine, flat(500, 900, 512));
    CHECK(litPixels(picture, engine.renderPlan().layout) == 0);
    auto const& hist = engine.histogram();
    CHECK(std::all_of(hist.counts32.begin(), hist.counts32.end(), [](std::uint32_t c) { return c == 0; }));
    CHECK(std::all_of(hist.counts.begin(), hist.counts.end(), [](std::uint16_t c) { return c == 0; }));
}

TEST_CASE("golden: colour bars in the vectorscope sit in their 75 % and 100 % target boxes")
{
    double const colours[6][3] = {{1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {1, 1, 0}};
    for (auto const* pattern : {"bars75", "bars100"})
    {
        CAPTURE(pattern);
        double const amount = std::string(pattern) == "bars75" ? 0.75 : 1.0;
        ScopeEngine engine(SimdLevel::Scalar);
        engine.configure({1920, 1080, v210RowBytes(1920)}, vectorSettings(1, "green", 60), 1920, 1080);
        auto const picture = renderWith(engine, packPattern(makePattern(pattern, 1920, 1080)));
        auto const& plan = engine.renderPlan();
        int const box = std::max(3, static_cast<int>(std::lround(plan.layout.panelW / 100.0)));
        auto const graticuleAt = [&](int row, int from, int to) {
            return std::any_of(plan.plotGraticule.begin(), plan.plotGraticule.end(),
                [&](GraticulePixel const& g) { return g.row == row && g.x >= from && g.x <= to; });
        };
        for (auto const& c : colours)
        {
            auto const code = rgbToYcbcr709(amount * c[0], amount * c[1], amount * c[2]);
            CAPTURE(code.cb);
            CAPTURE(code.cr);
            int x = 0;
            int y = 0;
            REQUIRE(pixelOf(engine, code.cb, code.cr, x, y));
            CHECK(lumaAt(picture, x, y) > kBackgroundY + 8);
            // The box's sides left and right of the dot, on its row.
            CHECK(graticuleAt(y, x - box - 2, x - 2));
            CHECK(graticuleAt(y, x + 2, x + box + 2));
        }
        // White and black: the centre.
        int x = 0;
        int y = 0;
        REQUIRE(pixelOf(engine, 512, 512, x, y));
        CHECK(lumaAt(picture, x, y) > kBackgroundY + 8);
    }
}

TEST_CASE("vectorscope graticule: the hue ring in the hue of its direction, level and width, the zoom text")
{
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920)}, vectorSettings(1, "green", 60), 1920, 1080);
    auto const& plan = engine.renderPlan();
    auto const& layout = plan.layout;
    auto const at = [&](int x, int row) {
        auto const it = std::find_if(plan.plotGraticule.begin(), plan.plotGraticule.end(), [&](GraticulePixel const& g) { return g.x == x && g.row == row; });
        REQUIRE(it != plan.plotGraticule.end());
        return *it;
    };
    int const mid = layout.plotX + layout.plotW / 2;
    int const middle = layout.plotY + layout.plotH / 2;
    // Up is +Cr (towards red), right +Cb (blue), down −Cr, left −Cb (yellow); all at the level.
    auto const top = at(mid, layout.plotY + 1);
    CHECK(top.cr > 600);
    auto const right = at(layout.plotX + layout.plotW - 2, middle);
    CHECK(right.cb > 600);
    auto const bottom = at(mid, layout.plotY + layout.plotH - 2);
    CHECK(bottom.cr < 424);
    auto const left = at(layout.plotX + 1, middle);
    CHECK(left.cb < 424);
    CHECK(top.alpha == std::lround(1023.0 * 60 / 100));
    // Sorted by row and x, inside the plot, one pixel each.
    for (std::size_t i = 1; i < plan.plotGraticule.size(); ++i)
    {
        auto const& a = plan.plotGraticule[i - 1];
        auto const& b = plan.plotGraticule[i];
        REQUIRE((a.row < b.row || (a.row == b.row && a.x < b.x)));
    }
    // Level 0 hides it; a wider graticule has more pixels.
    ScopeEngine hidden(SimdLevel::Scalar);
    hidden.configure({1920, 1080, v210RowBytes(1920)}, vectorSettings(1, "green", 0), 1920, 1080);
    CHECK(hidden.renderPlan().plotGraticule.empty());
    auto wide = vectorSettings(1, "green", 60);
    wide.graticuleWidth = 3;
    ScopeEngine thick(SimdLevel::Scalar);
    thick.configure({1920, 1080, v210RowBytes(1920)}, wide, 1920, 1080);
    CHECK(thick.renderPlan().plotGraticule.size() > plan.plotGraticule.size());
    // The zoom ("1×") right of the plot, in the still picture.
    bool text = false;
    for (int y = layout.plotY; y < layout.plotY + 20 && !text; ++y)
    {
        for (int x = layout.plotX + layout.plotW; x < layout.plotX + layout.plotW + 60 && !text; ++x)
        {
            text = lumaAt(plan.base, x, y) > kBackgroundY + 100;
        }
    }
    CHECK(text);
}

TEST_CASE("picture colour (luma): grey is white, a colour shows its hue, a dark colour stays visible, black noise stays grey")
{
    auto const render = [](std::vector<std::uint8_t> const& frame, char const* colour, int code, int& y, int& cb, int& cr) {
        DisplaySettings settings;
        settings.traceColor = colour;
        settings.graticuleLevel = 0;
        ScopeEngine engine(SimdLevel::Scalar);
        engine.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080);
        auto const picture = renderWith(engine, frame);
        int const x = engine.renderPlan().layout.plotX + 600;
        int const row = rowOfCode(engine.renderPlan(), code);
        y = lumaAt(picture, x, row);
        cb = cbAt(picture, x, row);
        cr = crAt(picture, x, row);
    };
    int y = 0;
    int cb = 0;
    int cr = 0;
    int wy = 0;
    int wcb = 0;
    int wcr = 0;
    render(flat(512, 512, 512), "picture", 512, y, cb, cr);
    render(flat(512, 512, 512), "white", 512, wy, wcb, wcr);
    CHECK(std::abs(y - wy) <= 1);
    CHECK(std::abs(cb - 512) <= 1);
    CHECK(std::abs(cr - 512) <= 1);
    auto const red = rgbToYcbcr709(0.75, 0, 0);
    render(flat(red.y, red.cb, red.cr), "picture", red.y, y, cb, cr);
    CHECK(cr > 800);
    CHECK(cb < 480);
    CHECK(y > 300);
    auto const blue = rgbToYcbcr709(0, 0, 0.2);
    render(flat(blue.y, blue.cb, blue.cr), "picture", blue.y, y, cb, cr);
    CHECK(y > 200);
    CHECK(cb > 700);
    render(flat(64, 515, 509), "picture", 64, y, cb, cr);
    CHECK(y > 700);
    CHECK(std::abs(cb - 512) <= 12);
    CHECK(std::abs(cr - 512) <= 12);
}

TEST_CASE("picture colour (vectorscope): the cell's luma and position give its colour; large updates shift the sums")
{
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920)}, vectorSettings(1, "picture"), 1920, 1080);
    auto const grey = renderWith(engine, flat(700, 512, 512));
    int x = 0;
    int y = 0;
    REQUIRE(pixelOf(engine, 512, 512, x, y));
    CHECK(lumaAt(grey, x, y) > 800);
    CHECK(std::abs(cbAt(grey, x, y) - 512) <= 6);
    CHECK(std::abs(crAt(grey, x, y) - 512) <= 6);
    auto const red = rgbToYcbcr709(0.75, 0, 0);
    auto const picture = renderWith(engine, flat(red.y, red.cb, red.cr));
    REQUIRE(pixelOf(engine, red.cb, red.cr, x, y));
    CHECK(crAt(picture, x, y) > 700);
    CHECK(lumaAt(picture, x, y) > 200);
    // A UHD input with 61 frames an update: the Y′ sums are shifted, the grey stays grey.
    ScopeEngine big(SimdLevel::Scalar);
    big.configure({3840, 2160, v210RowBytes(3840), 1, 61}, vectorSettings(1, "picture"), 1920, 1080);
    CHECK(big.analysePlan().sumShift > 0);
    CHECK(static_cast<std::uint64_t>(big.analysePlan().maxCellCount) * 61u * (1023u >> big.analysePlan().sumShift) <= 0xffffffffull);
    auto const uhd = flat(700, 512, 512, 3840, 2160);
    std::vector<std::uint8_t> out(big.packedSize());
    for (int i = 0; i < 3; ++i)
    {
        big.analyse(uhd.data());
    }
    big.render("", "", out.data());
    Frame422 shifted;
    shifted.allocate(1920, 1080);
    unpackV210(out.data(), v210RowBytes(1920), shifted);
    REQUIRE(pixelOf(big, 512, 512, x, y));
    CHECK(std::abs(lumaAt(shifted, x, y) - lumaAt(grey, x, y)) <= 2);
    CHECK(std::abs(cbAt(shifted, x, y) - 512) <= 6);
}

TEST_CASE("vectorscope and picture colour: AVX2 and AVX-512 pictures equal the scalar picture byte for byte")
{
    struct Case
    {
        char const* mode;
        char const* colour;
        int zoom;
    };
    for (auto const& c : {Case{"vectorscope", "green", 1}, Case{"vectorscope", "picture", 1}, Case{"vectorscope", "white", 2}, Case{"vectorscope", "picture", 5},
             Case{"luma", "picture", 1}})
    {
        for (auto const* pattern : {"random", "bars75", "scene"})
        {
            for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{1998, 50}, std::pair{3840, 2160}})
            {
                if (w == 3840 && std::string(pattern) != "scene")
                {
                    continue;
                }
                for (auto [ow, oh] : {std::pair{1920, 1080}, std::pair{640, 360}})
                {
                    CAPTURE(c.mode);
                    CAPTURE(c.colour);
                    CAPTURE(c.zoom);
                    CAPTURE(pattern);
                    CAPTURE(w);
                    CAPTURE(ow);
                    auto const frame = packPattern(makePattern(pattern, w, h, 512, 13));
                    DisplaySettings settings;
                    settings.mode = c.mode;
                    settings.traceColor = c.colour;
                    settings.vectorZoom = c.zoom;
                    ScopeEngine scalar(SimdLevel::Scalar);
                    scalar.configure({w, h, v210RowBytes(w)}, settings, ow, oh, true);
                    std::vector<std::uint8_t> reference(scalar.packedSize());
                    scalar.analyse(frame.data());
                    scalar.render(modeTitle(c.mode), "", reference.data());
                    for (auto level : {SimdLevel::Avx2, SimdLevel::Avx512})
                    {
                        if ((level == SimdLevel::Avx2 && !haveAvx2()) || (level == SimdLevel::Avx512 && !haveAvx512()))
                        {
                            continue;
                        }
                        CAPTURE(simdName(level));
                        ScopeEngine simd(level);
                        simd.configure({w, h, v210RowBytes(w)}, settings, ow, oh, true);
                        std::vector<std::uint8_t> out(simd.packedSize());
                        simd.analyse(frame.data());
                        simd.render(modeTitle(c.mode), "", out.data());
                        CHECK(out == reference);
                    }
                }
            }
        }
    }
}

TEST_CASE("vectorscope: key output; no measurements")
{
    auto const grey = flat(512, 512, 512);
    for (int bg : {0, 300})
    {
        CAPTURE(bg);
        auto settings = vectorSettings(1, "green", 60);
        settings.bgAlpha = bg;
        ScopeEngine plain(SimdLevel::Scalar);
        ScopeEngine keyed(SimdLevel::Scalar);
        plain.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080);
        keyed.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080, true);
        auto const fillBytes = v210RowBytes(1920) * 1080u;
        std::vector<std::uint8_t> a(plain.packedSize());
        std::vector<std::uint8_t> b(keyed.packedSize());
        plain.analyse(grey.data());
        plain.render("Vectorscope", "", a.data());
        keyed.analyse(grey.data());
        keyed.render("Vectorscope", "", b.data());
        REQUIRE(b.size() == fillBytes + alphaRowBytes(1920) * 1080u);
        CHECK(std::memcmp(a.data(), b.data(), fillBytes) == 0);
        auto const keyAt = [&](int x, int y) {
            std::uint32_t word = 0;
            std::memcpy(&word, b.data() + fillBytes + static_cast<std::size_t>(y) * alphaRowBytes(1920) + 4 * static_cast<std::size_t>(x / 3), 4);
            return static_cast<int>((word >> (10 * (x % 3))) & 0x3ffu);
        };
        auto const& plan = keyed.renderPlan();
        CHECK(keyAt(1918, 540) == bg);
        int x = 0;
        int y = 0;
        REQUIRE(pixelOf(keyed, 512, 512, x, y));
        CHECK(keyAt(x, y) == std::max<int>(bg, plan.planeIntensity[0].back()));
        // The hue ring at the top: its alpha.
        CHECK(keyAt(plan.layout.plotX + plan.layout.plotW / 2, plan.layout.plotY + 1) == std::max<int>(bg, std::lround(1023.0 * 60 / 100)));
        // The zoom text is opaque.
        bool opaque = false;
        for (int yy = plan.layout.plotY; yy < plan.layout.plotY + 20 && !opaque; ++yy)
        {
            for (int xx = plan.layout.plotX + plan.layout.plotW; xx < plan.layout.plotX + plan.layout.plotW + 60 && !opaque; ++xx)
            {
                opaque = keyAt(xx, yy) == 1023;
            }
        }
        CHECK(opaque);
    }
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920)}, vectorSettings(), 1920, 1080);
    std::vector<TraceGroup> trace;
    engine.analyse(grey.data());
    engine.trace("", "", trace);
    CHECK_FALSE(trace.empty());
    auto const& m = engine.measurements();
    CHECK(m.mode == "vectorscope");
    CHECK_FALSE(m.lumaMin.has_value());
    CHECK_FALSE(m.chromaOut.has_value());
    CHECK_FALSE(m.rgbOutOfGamut.has_value());
}

TEST_CASE("golden hashes of the vectorscope and the picture colour")
{
    // As "golden hashes of the default picture": a change here must be deliberate;
    // `mxl-scopes-bench --golden DIR` writes the PNGs (the same names).
    struct Golden
    {
        char const* name;
        char const* mode;
        char const* pattern;
        char const* colour;
        std::uint64_t hash;
    };
#ifdef SCOPES_WITH_BLEND2D
    std::initializer_list<Golden> const goldens = {Golden{"vectorscope-bars75", "vectorscope", "bars75", "green", 17380955653712384831ull},
        Golden{"vectorscope-bars100", "vectorscope", "bars100", "green", 15799791018308207453ull}, Golden{"vectorscope-scene", "vectorscope", "scene", "green", 9342981987738370560ull},
        Golden{"vectorscope-picture-scene", "vectorscope", "scene", "picture", 10197902513117317895ull}, Golden{"luma-picture-scene", "luma", "scene", "picture", 1995332149750024895ull}};
#else
    std::initializer_list<Golden> const goldens = {Golden{"vectorscope-bars75", "vectorscope", "bars75", "green", 11188988094391039281ull},
        Golden{"vectorscope-bars100", "vectorscope", "bars100", "green", 11879374138297924487ull}, Golden{"vectorscope-scene", "vectorscope", "scene", "green", 18206093701666561454ull},
        Golden{"vectorscope-picture-scene", "vectorscope", "scene", "picture", 15886855153057354733ull}, Golden{"luma-picture-scene", "luma", "scene", "picture", 3739531868033787636ull}};
#endif
    for (auto const& golden : goldens)
    {
        DisplaySettings settings;
        settings.mode = golden.mode;
        settings.traceColor = golden.colour;
        auto const frame = packPattern(makePattern(golden.pattern, 1920, 1080, 512));
        ScopeEngine engine(SimdLevel::Scalar);
        engine.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080);
        std::vector<std::uint8_t> out(engine.packedSize());
        engine.analyse(frame.data());
        engine.render(modeTitle(golden.mode), "", out.data());
        auto const hash = fnv1a(out);
        std::string const note = std::string(golden.name) + " " + std::to_string(hash);
        MESSAGE(note);
        if (golden.hash != 0)
        {
            CHECK(hash == golden.hash);
        }
    }
}
