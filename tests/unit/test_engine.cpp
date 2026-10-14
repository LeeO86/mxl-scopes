// Bit-exact AVX2 against the scalar reference (SPECIFICATION §14.1), and the golden pictures.
#include <doctest/doctest.h>

#include "media/patterns.hpp"
#include "scope/analyse.hpp"
#include "scope/cpu/dispatch.hpp"
#include "scope/engine.hpp"

#include <algorithm>
#include <cstdio>
#include <ios>
#include <string>
#include <vector>

using namespace scopes;

namespace
{
bool haveAvx2()
{
    static bool const have = avx2Compiled() && cpuHasAvx2();
    static bool said = false;
    if (!have && !said)
    {
        said = true;
        std::fprintf(stderr, "note: this CPU or build has no AVX2; the AVX2 engine tests are skipped\n");
    }
    return have;
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

// Every code 0..1023 in the frame, so the reserved codes 0-3 and 1020-1023 are covered.
std::vector<std::uint8_t> randomFrame(int width, int height, std::uint32_t seed)
{
    return packPattern(makePattern("random", width, height, 0, seed));
}

void compareCounts(AnalysePlan const& plan, std::vector<std::uint8_t> const& frame)
{
    Histogram scalar;
    scalar.reset(plan);
    analyseScalar(plan, frame.data(), scalar);
    std::uint64_t total = 0;
    for (auto c : scalar.counts)
    {
        total += c;
    }
    CHECK(total == plan.samples);
    CHECK(std::all_of(scalar.scratch.begin(), scalar.scratch.end(), [](std::uint16_t c) { return c == 0; }));
    if (haveAvx2())
    {
        Histogram simd;
        simd.reset(plan);
        analyseAvx2(plan, frame.data(), simd);
        REQUIRE(simd.counts == scalar.counts);
        CHECK(std::all_of(simd.scratch.begin(), simd.scratch.end(), [](std::uint16_t c) { return c == 0; }));
    }
}
} // namespace

TEST_CASE("analyse plan: columns cover the width, bins cover the codes")
{
    auto const plan = makeAnalysePlan(1920, 1080, v210RowBytes(1920), 1, 1, 1728, 994);
    CHECK(plan.columnOf.front() == 0);
    CHECK(plan.columnOf.back() == 1727);
    for (std::size_t x = 1; x < plan.columnOf.size(); ++x)
    {
        CHECK(plan.columnOf[x] - plan.columnOf[x - 1] <= 1);
    }
    CHECK(plan.binOf[0] == 0);
    CHECK(plan.binOf[1023] == 993);
    CHECK(plan.samples == 1920ull * 1080ull);
    CHECK_FALSE(plan.twoSubs);
    auto const wide = makeAnalysePlan(3840, 2160, v210RowBytes(3840), 1, 1, 1728, 994);
    CHECK(wide.twoSubs);
    CHECK(wide.strips.size() >= 2);
    for (std::size_t i = 1; i < wide.strips.size(); ++i)
    {
        CHECK(wide.strips[i].g0 == wide.strips[i - 1].g1);
    }
}

TEST_CASE("AVX2 equals scalar: random frames, every step")
{
    for (int lineStep : {1, 2, 3, 8})
    {
        for (int pixelStep : {1, 2, 3, 6})
        {
            CAPTURE(lineStep);
            CAPTURE(pixelStep);
            compareCounts(makeAnalysePlan(1920, 64, v210RowBytes(1920), lineStep, pixelStep, 1728, 994), randomFrame(1920, 64, 3));
            compareCounts(makeAnalysePlan(3840, 32, v210RowBytes(3840), lineStep, pixelStep, 1728, 994), randomFrame(3840, 32, 5));
        }
    }
}

TEST_CASE("AVX2 equals scalar: odd widths, column counts, bins and strips")
{
    for (int width : {1, 5, 6, 7, 12, 13, 47, 48, 49, 720, 1001, 1280})
    {
        auto const frame = randomFrame(width, 9, static_cast<std::uint32_t>(width));
        for (int columns : {1, 3, 64, width})
        {
            if (columns > width)
            {
                continue;
            }
            for (int bins : {1, 37, 994, 1024})
            {
                for (int strip : {1, 64, 512, 100000})
                {
                    CAPTURE(width);
                    CAPTURE(columns);
                    CAPTURE(bins);
                    CAPTURE(strip);
                    compareCounts(makeAnalysePlan(width, 9, v210RowBytes(width), 1, 1, columns, bins, strip), frame);
                    compareCounts(makeAnalysePlan(width, 9, v210RowBytes(width), 2, 2, columns, bins, strip), frame);
                }
            }
        }
    }
}

TEST_CASE("AVX2 equals scalar: edge frames")
{
    for (int level : {0, 4, 64, 512, 940, 1019, 1023})
    {
        CAPTURE(level);
        auto const frame = packPattern(makePattern("flat", 1924, 16, level));
        compareCounts(makeAnalysePlan(1924, 16, v210RowBytes(1924), 1, 1, 962, 1024), frame);
        compareCounts(makeAnalysePlan(1924, 16, v210RowBytes(1924), 1, 1, 1728, 994), frame);
    }
}

TEST_CASE("a padded stride is read like a tight one")
{
    int const width = 200;
    auto const tight = randomFrame(width, 10, 9);
    auto const stride = v210RowBytes(width) + 256;
    std::vector<std::uint8_t> padded(stride * 10, 0xff);
    for (int row = 0; row < 10; ++row)
    {
        std::copy_n(tight.begin() + static_cast<std::ptrdiff_t>(row * v210RowBytes(width)), v210RowBytes(width), padded.begin() + static_cast<std::ptrdiff_t>(row * stride));
    }
    auto const a = makeAnalysePlan(width, 10, v210RowBytes(width), 1, 1, 150, 300);
    auto const b = makeAnalysePlan(width, 10, stride, 1, 1, 150, 300);
    Histogram ha;
    ha.reset(a);
    analyseScalar(a, tight.data(), ha);
    Histogram hb;
    hb.reset(b);
    analyseScalar(b, padded.data(), hb);
    CHECK(ha.counts == hb.counts);
}

TEST_CASE("engine output: AVX2 picture equals scalar picture byte for byte")
{
    if (!haveAvx2())
    {
        return;
    }
    for (auto const* pattern : {"random", "bars75", "ramp"})
    {
        for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{3840, 2160}})
        {
            CAPTURE(pattern);
            CAPTURE(w);
            auto const frame = packPattern(makePattern(pattern, w, h, 512, 11));
            DisplaySettings settings;
            ScopeEngine scalar(SimdLevel::Scalar);
            ScopeEngine simd(SimdLevel::Avx2);
            scalar.configure({w, h, v210RowBytes(w)}, settings, 1920, 1080);
            simd.configure({w, h, v210RowBytes(w)}, settings, 1920, 1080);
            std::vector<std::uint8_t> a(scalar.packedSize());
            std::vector<std::uint8_t> b(simd.packedSize());
            scalar.analyse(frame.data());
            simd.analyse(frame.data());
            scalar.render("Luma waveform", "", a.data());
            simd.render("Luma waveform", "", b.data());
            CHECK(a == b);
        }
    }
}

namespace
{
// The brightest Y of the plot rows in column `px` of the plot, and the row where it is.
std::pair<int, int> brightest(Frame422 const& frame, RenderPlan const& plan, int px)
{
    int best = -1;
    int row = -1;
    for (int r = plan.layout.plotY; r < plan.layout.plotY + plan.layout.plotH; ++r)
    {
        int const y = frame.y[static_cast<std::size_t>(r) * frame.width + plan.layout.plotX + px];
        if (y > best)
        {
            best = y;
            row = r;
        }
    }
    return {best, row};
}

Frame422 renderPattern(std::string const& pattern, int level, std::vector<std::uint8_t>* packed = nullptr)
{
    auto const frame = packPattern(makePattern(pattern, 1920, 1080, level));
    DisplaySettings settings;
    settings.graticuleLevel = 0; // trace only
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080);
    std::vector<std::uint8_t> out(engine.packedSize());
    engine.analyse(frame.data());
    engine.render("", "", out.data());
    Frame422 picture;
    picture.allocate(1920, 1080);
    unpackV210(out.data(), v210RowBytes(1920), picture);
    if (packed != nullptr)
    {
        *packed = out;
    }
    return picture;
}
} // namespace

TEST_CASE("golden: a flat grey is one line at its code")
{
    for (int level : {64, 512, 940})
    {
        CAPTURE(level);
        auto const picture = renderPattern("flat", level);
        auto const plan = makeRenderPlan(makeLayout(1920, 1080), DisplaySettings{}, 1728, 1920ull * 1080, 2);
        int const row = rowOfCode(plan, level);
        for (int px : {0, 100, 864, 1727})
        {
            auto const [y, at] = brightest(picture, plan, px);
            CHECK(y > 600);
            CHECK(at == row);
        }
        // Everything else in the plot is background.
        int lit = 0;
        for (int r = plan.layout.plotY; r < plan.layout.plotY + plan.layout.plotH; ++r)
        {
            lit += picture.y[static_cast<std::size_t>(r) * 1920 + plan.layout.plotX + 500] != kBackgroundY ? 1 : 0;
        }
        CHECK(lit == 1);
    }
}

TEST_CASE("golden: a luma ramp is a diagonal")
{
    auto const picture = renderPattern("ramp", 0);
    auto const plan = makeRenderPlan(makeLayout(1920, 1080), DisplaySettings{}, 1728, 1920ull * 1080, 2);
    int previous = 1 << 30;
    for (int px = 0; px < 1728; px += 96)
    {
        auto const [y, row] = brightest(picture, plan, px);
        CHECK(y > 600);
        CHECK(row <= previous); // the trace climbs left to right
        previous = row;
    }
    CHECK(brightest(picture, plan, 0).second == rowOfCode(plan, 64));
    CHECK(brightest(picture, plan, 1727).second == rowOfCode(plan, 940));
}

TEST_CASE("golden: colour bars are horizontal segments at their luma")
{
    for (auto const* pattern : {"bars75", "bars100"})
    {
        CAPTURE(pattern);
        double const a = std::string(pattern) == "bars75" ? 0.75 : 1.0;
        auto const picture = renderPattern(pattern, 0);
        auto const plan = makeRenderPlan(makeLayout(1920, 1080), DisplaySettings{}, 1728, 1920ull * 1080, 2);
        double const rgb[8][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0}};
        for (int bar = 0; bar < 8; ++bar)
        {
            CAPTURE(bar);
            auto const c = rgbToYcbcr709(a * rgb[bar][0], a * rgb[bar][1], a * rgb[bar][2]);
            int const px = (2 * bar + 1) * 1728 / 16; // the middle of the bar
            CHECK(brightest(picture, plan, px).second == rowOfCode(plan, c.y));
        }
    }
}

TEST_CASE("golden hashes of the default picture")
{
    // The packed output of the default settings; a change here is a visible change and must
    // be deliberate. `mxl-scopes-bench --golden DIR` writes the PNGs of these for review.
    struct Golden
    {
        char const* pattern;
        std::uint64_t hash;
    };
    for (auto const& golden : {Golden{"ramp", 12643153030590494626ull}, Golden{"bars75", 5444655765699388903ull}, Golden{"bars100", 13853864728963787911ull}, Golden{"flat", 2022167930782572679ull}})
    {
        auto const frame = packPattern(makePattern(golden.pattern, 1920, 1080, 512));
        ScopeEngine engine(SimdLevel::Scalar);
        engine.configure({1920, 1080, v210RowBytes(1920)}, DisplaySettings{}, 1920, 1080);
        std::vector<std::uint8_t> out(engine.packedSize());
        engine.analyse(frame.data());
        engine.render("Luma waveform", "", out.data());
        auto const hash = fnv1a(out);
        std::string const note = std::string(golden.pattern) + " " + std::to_string(hash);
        MESSAGE(note);
        if (golden.hash != 0)
        {
            CHECK(hash == golden.hash);
        }
    }
}
