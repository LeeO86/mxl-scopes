#include <doctest/doctest.h>

#include "app/triplebuffer.hpp"
#include "media/patterns.hpp"
#include "media/png.hpp"
#include "media/v210.hpp"
#include "scope/engine.hpp"
#include "scope/render.hpp"

#include <algorithm>

using namespace scopes;

TEST_CASE("layout: margins, even plot, text scale")
{
    auto const l = makeLayout(1920, 1080);
    CHECK(l.plotX % 2 == 0);
    CHECK(l.plotW % 2 == 0);
    CHECK(l.plotX >= 150);
    CHECK(l.plotX + l.plotW <= 1920);
    CHECK(l.plotY >= 40);
    CHECK(l.plotY + l.plotH <= 1080 - 40);
    CHECK(l.textScale == 2);
    CHECK(histogramBins(l) == l.plotH);
    auto const small = makeLayout(320, 180);
    CHECK(small.textScale == 1);
    CHECK(small.plotY >= 14);
    auto const uhd = makeLayout(3840, 2160);
    CHECK(histogramBins(uhd) == 1024);
    CHECK(histogramColumns(uhd, 1920) == 1920);
}

TEST_CASE("v210 pack and unpack round trip")
{
    for (int width : {2, 6, 8, 48, 50, 1280, 1920})
    {
        auto const frame = makePattern("random", width, 5, 0, 99);
        auto const packed = packPattern(frame);
        Frame422 back;
        back.allocate(width, 5);
        unpackV210(packed.data(), v210RowBytes(width), back);
        CHECK(back.y == frame.y);
        CHECK(back.cb == frame.cb);
        CHECK(back.cr == frame.cr);
    }
}

TEST_CASE("bins: codes 64 and 940 land on the 0 % and 100 % graticule rows")
{
    for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{960, 540}, std::pair{320, 180}, std::pair{3840, 2160}})
    {
        CAPTURE(h);
        DisplaySettings settings;
        auto const plan = makeRenderPlan(makeLayout(w, h), settings, makeLayout(w, h).plotW, 1000, 10);
        auto const find = [&](int code) {
            return std::find_if(plan.lines.begin(), plan.lines.end(), [&](GraticuleLine const& line) { return line.code == code; });
        };
        REQUIRE(find(64) != plan.lines.end());
        REQUIRE(find(940) != plan.lines.end());
        int const row64 = find(64)->row;
        int const row940 = find(940)->row;
        CHECK(plan.binOfRow[static_cast<std::size_t>(row64 - plan.layout.plotY)] == (64 * plan.bins) >> 10);
        CHECK(plan.binOfRow[static_cast<std::size_t>(row940 - plan.layout.plotY)] == (940 * plan.bins) >> 10);
        CHECK(row940 < row64);
        // Every plot row shows a bin, and every bin is shown.
        std::vector<int> shown(static_cast<std::size_t>(plan.bins), 0);
        for (auto bin : plan.binOfRow)
        {
            ++shown[bin];
        }
        CHECK(std::count(shown.begin(), shown.end(), 0) == 0);
        // Columns cover the plot without gaps.
        for (std::size_t px = 1; px < plan.columnOf.size(); ++px)
        {
            CHECK(plan.columnOf[px] - plan.columnOf[px - 1] <= 1);
        }
    }
}

TEST_CASE("graticule: rows for mV, percent, code and full range; labels in the margin")
{
    for (auto const* units : {"mv", "percent", "code"})
    {
        for (bool limited : {true, false})
        {
            CAPTURE(units);
            CAPTURE(limited);
            DisplaySettings settings;
            settings.graticuleUnits = units;
            settings.limitedRange = limited;
            auto const plan = makeRenderPlan(makeLayout(1920, 1080), settings, 1728, 1000, 10);
            CHECK(plan.lines.size() >= 5);
            for (auto const& line : plan.lines)
            {
                CHECK(line.row == rowOfCode(plan, line.code));
                CHECK(line.row >= plan.layout.plotY);
                CHECK(line.row < plan.layout.plotY + plan.layout.plotH);
            }
            // Labels are drawn only left of the plot.
            int outside = 0;
            int labels = 0;
            for (int row = 0; row < 1080; ++row)
            {
                for (int x = 0; x < 1920; ++x)
                {
                    bool const lit = plan.base.y[static_cast<std::size_t>(row) * 1920 + x] != kBackgroundY;
                    outside += lit && x >= plan.layout.plotX ? 1 : 0;
                    labels += lit && x < plan.layout.plotX ? 1 : 0;
                }
            }
            CHECK(outside == 0);
            CHECK(labels > 100);
        }
    }
    DisplaySettings off;
    off.graticuleLevel = 0;
    CHECK(makeRenderPlan(makeLayout(1920, 1080), off, 1728, 1000, 10).plotGraticule.empty());
}

TEST_CASE("intensity: monotonic, background at zero, at least trace_min")
{
    DisplaySettings settings;
    settings.intensity = 25;
    auto const table = intensityTable(settings, 50.0, 60000);
    CHECK(table[0] == 0);
    CHECK(table[1] >= settings.traceMin);
    for (std::size_t c = 1; c < table.size(); ++c)
    {
        CHECK(table[c] >= table[c - 1]);
    }
    CHECK(table.back() == 1023);
}

TEST_CASE("intensity: a flat field keeps its brightness across line and pixel steps")
{
    // The normalisation divides by the samples taken (§4.4): every step gives the same intensity
    // within one code for a flat field, a column's samples in one cell.
    int reference = -1;
    for (auto [ls, ps] : {std::pair{1, 1}, std::pair{2, 1}, std::pair{2, 2}, std::pair{4, 3}})
    {
        DisplaySettings settings;
        settings.lineStep = ls;
        settings.pixelStep = ps;
        settings.intensity = 1; // far from saturation
        auto const plan = makeAnalysePlan(1920, 1080, v210RowBytes(1920), ls, ps, 1728, 994);
        auto const render = makeRenderPlan(makeLayout(1920, 1080), settings, 1728, plan.samples, plan.maxCellCount);
        // A flat column holds samples/columns counts in one cell.
        auto const count = static_cast<std::uint32_t>(plan.samples / 1728);
        int const level = render.intensity[std::min(count, render.maxCount)];
        if (reference < 0)
        {
            reference = level;
        }
        CAPTURE(ls);
        CAPTURE(ps);
        CHECK(std::abs(level - reference) <= 1 + reference / 20);
    }
}

TEST_CASE("state text replaces the trace; the picture is the still")
{
    auto const frame = packPattern(makePattern("flat", 1920, 1080, 512));
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920)}, DisplaySettings{}, 1920, 1080);
    std::vector<std::uint8_t> out(engine.packedSize());
    engine.analyse(frame.data());
    engine.render("Luma waveform", "NO SIGNAL", out.data());
    Frame422 picture;
    picture.allocate(1920, 1080);
    unpackV210(out.data(), v210RowBytes(1920), picture);
    CHECK(picture.y == engine.still().y);
    // The counts were cleared: the next render without a state shows no trace.
    engine.render("Luma waveform", "", out.data());
    unpackV210(out.data(), v210RowBytes(1920), picture);
    auto const& plan = engine.renderPlan();
    int const row = rowOfCode(plan, 512);
    CHECK(picture.y[static_cast<std::size_t>(row) * 1920 + plan.layout.plotX + 101] == kBackgroundY);
}

TEST_CASE("snapshot PNG has the PNG signature")
{
    Frame422 frame;
    frame.allocate(64, 36);
    auto const png = encodePng(frame);
    REQUIRE(png.size() > 8);
    CHECK(static_cast<unsigned char>(png[0]) == 0x89);
    CHECK(png.substr(1, 3) == "PNG");
}

TEST_CASE("triple buffer: the writer gets the newest published frame, never the renderer's")
{
    TripleBuffer buffers;
    buffers.allocate(4);
    auto const* first = buffers.front();
    buffers.back()[0] = 1;
    buffers.publish();
    auto const* a = buffers.front();
    CHECK(a[0] == 1);
    CHECK(a != first);
    CHECK(buffers.front() == a); // nothing new: the same buffer again
    buffers.back()[0] = 2;
    CHECK(buffers.back() != a);
    buffers.publish();
    buffers.back()[0] = 3;
    buffers.publish();
    CHECK(buffers.front()[0] == 3);
}

TEST_CASE("a narrow input fills the whole plot width without gaps")
{
    auto const frame = packPattern(makePattern("flat", 320, 180, 512));
    DisplaySettings settings;
    settings.graticuleLevel = 0;
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({320, 180, v210RowBytes(320)}, settings, 1920, 1080);
    CHECK(engine.analysePlan().columns == 320);
    std::vector<std::uint8_t> out(engine.packedSize());
    engine.analyse(frame.data());
    engine.render("", "", out.data());
    Frame422 picture;
    picture.allocate(1920, 1080);
    unpackV210(out.data(), v210RowBytes(1920), picture);
    auto const& plan = engine.renderPlan();
    int const row = rowOfCode(plan, 512);
    int dark = 0;
    for (int px = 0; px < plan.layout.plotW; ++px)
    {
        dark += picture.y[static_cast<std::size_t>(row) * 1920 + plan.layout.plotX + px] == kBackgroundY ? 1 : 0;
    }
    CHECK(dark == 0);
}

TEST_CASE("snapshot scaling keeps one-pixel lines")
{
    Frame422 frame;
    frame.allocate(64, 36);
    for (int x = 0; x < 64; ++x)
    {
        frame.y[static_cast<std::size_t>(7) * 64 + x] = 940; // an odd row
    }
    auto const full = encodePng(frame, 1);
    auto const half = encodePng(frame, 2);
    CHECK(half.size() > 8);
    CHECK(half.size() < full.size());
}

TEST_CASE("intensity: brightness grows with the hits per cell (§4.4)")
{
    // 1080p: about 1200 samples per column; a few hits are dim, many bright.
    DisplaySettings settings;
    double const cRef = 1920.0 * 1080.0 / 1722.0;
    auto const table = intensityTable(settings, cRef, 65535);
    auto at = [&](std::size_t c) { return static_cast<int>(table[std::min(c, table.size() - 1)]); };
    CHECK(at(1) == settings.traceMin);
    CHECK(at(10) > at(1));
    CHECK(at(10) < 300);
    CHECK(at(60) > 550);
    CHECK(at(60) < 750);
    CHECK(at(300) > 980);
    CHECK(at(static_cast<std::size_t>(cRef)) == 1023);
    // SCOPES_INTENSITY scales the gain: 200 % is brighter at the same count.
    settings.intensity = 200;
    CHECK(static_cast<int>(intensityTable(settings, cRef, 65535)[30]) > at(30));
}
