// The YCbCr and RGB modes (SPECIFICATION §5.1, §4.8): AVX2 and AVX-512 counts and pictures equal
// the scalar reference, R′G′B′ against the double formula, and golden pictures of colour bars.
#include <doctest/doctest.h>

#include "media/patterns.hpp"
#include "scope/analyse.hpp"
#include "scope/cpu/dispatch.hpp"
#include "scope/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

std::vector<std::uint8_t> randomFrame(int width, int height, std::uint32_t seed)
{
    return packPattern(makePattern("random", width, height, 0, seed));
}

// Every SIMD path gives the scalar counts, with 16-bit cells and with 32-bit cells (the plan's
// own width first).
void compareCounts(AnalysePlan const& given, std::vector<std::uint8_t> const& frame)
{
    for (bool const wideCells : {given.wide, !given.wide})
    {
        CAPTURE(wideCells);
        auto plan = given;
        plan.wide = wideCells;
        Histogram scalar;
        scalar.reset(plan);
        analyseScalar(plan, frame.data(), scalar);
        REQUIRE(scalar.wide == wideCells);
        for (int p = 0; p < plan.planes; ++p)
        {
            std::uint64_t total = 0;
            for (std::size_t i = 0; i < plan.planeCells; ++i)
            {
                auto const at = plan.planeCells * static_cast<std::size_t>(p) + i;
                total += wideCells ? scalar.counts32[at] : scalar.counts[at];
            }
            CAPTURE(p);
            CHECK(total == plan.planeSamples[static_cast<std::size_t>(p)]);
        }
        auto const clean = [](Histogram const& h) {
            return std::all_of(h.scratch.begin(), h.scratch.end(), [](std::uint16_t c) { return c == 0; }) &&
                   std::all_of(h.scratch32.begin(), h.scratch32.end(), [](std::uint32_t c) { return c == 0; });
        };
        CHECK(clean(scalar));
        if (haveAvx2())
        {
            Histogram simd;
            simd.reset(plan);
            analyseAvx2(plan, frame.data(), simd);
            REQUIRE(simd.counts == scalar.counts);
            REQUIRE(simd.counts32 == scalar.counts32);
            CHECK(clean(simd));
        }
        if (haveAvx512())
        {
            Histogram simd;
            simd.reset(plan);
            analyseAvx512(plan, frame.data(), simd);
            REQUIRE(simd.counts == scalar.counts);
            REQUIRE(simd.counts32 == scalar.counts32);
            CHECK(clean(simd));
        }
    }
}

constexpr ScopeKind kKinds[] = {ScopeKind::Ycbcr, ScopeKind::Rgb};
constexpr char const* kModes[] = {"ycbcr-parade", "rgb-parade", "rgb-overlay"};
} // namespace

TEST_CASE("modes: kinds, panels and planes")
{
    CHECK(scopeKindOf("luma") == ScopeKind::Luma);
    CHECK(scopeKindOf("ycbcr-parade") == ScopeKind::Ycbcr);
    CHECK(scopeKindOf("rgb-parade") == ScopeKind::Rgb);
    CHECK(scopeKindOf("rgb-overlay") == ScopeKind::Rgb);
    CHECK(panelCount("luma") == 1);
    CHECK(panelCount("rgb-overlay") == 1);
    CHECK(panelCount("ycbcr-parade") == 3);
    CHECK(panelCount("rgb-parade") == 3);
    for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{960, 540}, std::pair{320, 180}, std::pair{3840, 2160}})
    {
        CAPTURE(w);
        auto const one = makeLayout(w, h);
        auto const three = makeLayout(w, h, 3);
        CHECK(three.panels == 3);
        CHECK(three.panelW % 6 == 0);
        CHECK(three.panelX[0] == one.plotX);
        CHECK(three.panelX[1] > three.panelX[0] + three.panelW);
        CHECK(three.panelX[2] % 6 == 0);
        CHECK(three.panelX[2] + three.panelW <= one.plotX + one.plotW);
        CHECK(three.plotH == one.plotH);
    }
}

TEST_CASE("R′G′B′: the Q13 formula is within one code of the double formula (§4.8)")
{
    int worst = 0;
    for (int y = 0; y < 1024; y += 7)
    {
        for (int cb = 0; cb < 1024; cb += 31)
        {
            for (int cr = 0; cr < 1024; cr += 29)
            {
                int r = 0;
                int g = 0;
                int b = 0;
                rgbOfYcbcr(y, cb, cr, r, g, b);
                double const dr = y + 1.539648 * (cr - 512);
                double const dg = y - 0.183143 * (cb - 512) - 0.457675 * (cr - 512);
                double const db = y + 1.814180 * (cb - 512);
                worst = std::max({worst, static_cast<int>(std::lround(std::fabs(r - dr))), static_cast<int>(std::lround(std::fabs(g - dg))),
                    static_cast<int>(std::lround(std::fabs(b - db)))});
            }
        }
    }
    CHECK(worst <= 1);
    // Corners: neutral stays neutral, 75 % red is R 721, G and B 64.
    int r = 0;
    int g = 0;
    int b = 0;
    rgbOfYcbcr(502, 512, 512, r, g, b);
    CHECK(r == 502);
    CHECK(g == 502);
    CHECK(b == 502);
    auto const red = rgbToYcbcr709(0.75, 0, 0);
    rgbOfYcbcr(red.y, red.cb, red.cr, r, g, b);
    CHECK(std::abs(r - 721) <= 1);
    CHECK(std::abs(g - 64) <= 1);
    CHECK(std::abs(b - 64) <= 1);
}

TEST_CASE("modes: SIMD equals scalar, random frames, every step")
{
    for (auto kind : kKinds)
    {
        for (int lineStep : {1, 2, 3})
        {
            for (int pixelStep : {1, 2, 3, 6})
            {
                CAPTURE(static_cast<int>(kind));
                CAPTURE(lineStep);
                CAPTURE(pixelStep);
                compareCounts(makeAnalysePlan(1920, 32, v210RowBytes(1920), lineStep, pixelStep, 522, 994, kDefaultStripColumns, kind), randomFrame(1920, 32, 3));
                compareCounts(makeAnalysePlan(1920, 32, v210RowBytes(1920), lineStep, pixelStep, 1728, 994, kDefaultStripColumns, kind), randomFrame(1920, 32, 4));
                compareCounts(makeAnalysePlan(3840, 16, v210RowBytes(3840), lineStep, pixelStep, 522, 994, kDefaultStripColumns, kind), randomFrame(3840, 16, 5));
            }
        }
    }
}

TEST_CASE("modes: SIMD equals scalar, odd widths, column counts, bins and strips")
{
    for (auto kind : kKinds)
    {
        for (int width : {1, 5, 6, 7, 12, 13, 23, 24, 30, 47, 48, 49, 95, 96, 97, 720, 1001, 1280})
        {
            auto const frame = randomFrame(width, 7, static_cast<std::uint32_t>(width) + 17);
            for (int columns : {1, 3, 64, width})
            {
                if (columns > width)
                {
                    continue;
                }
                for (int bins : {1, 37, 1024})
                {
                    for (int strip : {1, 64, 100000})
                    {
                        CAPTURE(static_cast<int>(kind));
                        CAPTURE(width);
                        CAPTURE(columns);
                        CAPTURE(bins);
                        CAPTURE(strip);
                        compareCounts(makeAnalysePlan(width, 7, v210RowBytes(width), 1, 1, columns, bins, strip, kind), frame);
                        compareCounts(makeAnalysePlan(width, 7, v210RowBytes(width), 2, 3, columns, bins, strip, kind), frame);
                    }
                }
            }
        }
    }
}

TEST_CASE("modes: SIMD equals scalar, edge frames")
{
    for (auto kind : kKinds)
    {
        for (int level : {0, 4, 64, 512, 940, 1019, 1023})
        {
            CAPTURE(level);
            auto picture = makePattern("flat", 1924, 8, level);
            // Chroma at the extremes too: the R′G′B′ values leave 0..1023 and are clamped.
            std::fill(picture.cb.begin(), picture.cb.end(), static_cast<std::uint16_t>(1023 - level));
            std::fill(picture.cr.begin(), picture.cr.end(), static_cast<std::uint16_t>(level));
            auto const frame = packPattern(picture);
            compareCounts(makeAnalysePlan(1924, 8, v210RowBytes(1924), 1, 1, 522, 1024, kDefaultStripColumns, kind), frame);
            compareCounts(makeAnalysePlan(1924, 8, v210RowBytes(1924), 1, 1, 1728, 994, kDefaultStripColumns, kind), frame);
        }
    }
}

TEST_CASE("modes: chroma samples follow their co-sited luma sample")
{
    auto const plan = makeAnalysePlan(1920, 10, v210RowBytes(1920), 1, 3, 522, 994, kDefaultStripColumns, ScopeKind::Ycbcr);
    // Pixel step 3 takes luma 0 and 3 of each group; only luma 0 has a co-sited chroma pair.
    CHECK(plan.planeSamples[0] == 640ull * 10);
    CHECK(plan.planeSamples[1] == 320ull * 10);
    CHECK(plan.planeSamples[2] == 320ull * 10);
    auto const rgb = makeAnalysePlan(1920, 10, v210RowBytes(1920), 1, 1, 522, 994, kDefaultStripColumns, ScopeKind::Rgb);
    CHECK(rgb.planeSamples[1] == 1920ull * 10);
}

TEST_CASE("modes: AVX2 and AVX-512 pictures equal the scalar picture byte for byte")
{
    for (auto const* mode : kModes)
    {
        for (auto const* pattern : {"random", "bars75", "scene"})
        {
            for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{1998, 50}, std::pair{3840, 2160}})
            {
                if (w == 3840 && std::string(pattern) != "scene")
                {
                    continue;
                }
                CAPTURE(mode);
                CAPTURE(pattern);
                CAPTURE(w);
                auto const frame = packPattern(makePattern(pattern, w, h, 512, 11));
                DisplaySettings settings;
                settings.mode = mode;
                ScopeEngine scalar(SimdLevel::Scalar);
                scalar.configure({w, h, v210RowBytes(w)}, settings, 1920, 1080);
                std::vector<std::uint8_t> reference(scalar.packedSize());
                scalar.analyse(frame.data());
                scalar.render(modeTitle(mode), "", reference.data());
                for (auto level : {SimdLevel::Avx2, SimdLevel::Avx512})
                {
                    if ((level == SimdLevel::Avx2 && !haveAvx2()) || (level == SimdLevel::Avx512 && !haveAvx512()))
                    {
                        continue;
                    }
                    CAPTURE(simdName(level));
                    ScopeEngine simd(level);
                    simd.configure({w, h, v210RowBytes(w)}, settings, 1920, 1080);
                    std::vector<std::uint8_t> out(simd.packedSize());
                    simd.analyse(frame.data());
                    simd.render(modeTitle(mode), "", out.data());
                    CHECK(out == reference);
                }
            }
        }
    }
}

namespace
{
// The rendered picture of a mode, graticule off (trace only).
Frame422 renderMode(std::string const& mode, std::string const& pattern, RenderPlan* planOut = nullptr)
{
    auto const frame = packPattern(makePattern(pattern, 1920, 1080, 512));
    DisplaySettings settings;
    settings.mode = mode;
    settings.graticuleLevel = 0;
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080);
    std::vector<std::uint8_t> out(engine.packedSize());
    engine.analyse(frame.data());
    engine.render("", "", out.data());
    if (planOut != nullptr)
    {
        *planOut = engine.renderPlan();
    }
    Frame422 picture;
    picture.allocate(1920, 1080);
    unpackV210(out.data(), v210RowBytes(1920), picture);
    return picture;
}

// The rows of the plot column `x` (absolute) whose luma is above the background.
std::vector<int> litRows(Frame422 const& picture, RenderPlan const& plan, int x)
{
    std::vector<int> rows;
    for (int r = plan.layout.plotY; r < plan.layout.plotY + plan.layout.plotH; ++r)
    {
        if (picture.y[static_cast<std::size_t>(r) * picture.width + x] > kBackgroundY + 8)
        {
            rows.push_back(r);
        }
    }
    return rows;
}
} // namespace

TEST_CASE("golden: colour bars in the parades are horizontal segments at their component codes")
{
    double const rgb[8][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0}};
    for (auto const* mode : {"ycbcr-parade", "rgb-parade"})
    {
        CAPTURE(mode);
        RenderPlan plan;
        auto const picture = renderMode(mode, "bars75", &plan);
        REQUIRE(plan.panels.size() == 3);
        for (int bar = 0; bar < 8; ++bar)
        {
            auto const c = rgbToYcbcr709(0.75 * rgb[bar][0], 0.75 * rgb[bar][1], 0.75 * rgb[bar][2]);
            int codes[3] = {c.y, c.cb, c.cr};
            if (std::string(mode) == "rgb-parade")
            {
                rgbOfYcbcr(c.y, c.cb, c.cr, codes[0], codes[1], codes[2]);
            }
            for (int k = 0; k < 3; ++k)
            {
                CAPTURE(bar);
                CAPTURE(k);
                int const x = plan.panels[static_cast<std::size_t>(k)].x + (2 * bar + 1) * plan.layout.panelW / 16;
                auto const rows = litRows(picture, plan, x);
                REQUIRE(rows.size() == 1);
                CHECK(rows[0] == rowOfCode(plan, std::clamp(codes[k], 0, 1023)));
            }
        }
    }
}

TEST_CASE("golden: the RGB overlay of grey is white, of 75 % red is red only")
{
    RenderPlan plan;
    auto const flat = renderMode("rgb-overlay", "flat", &plan);
    int const x = plan.layout.plotX + 600;
    auto const rows = litRows(flat, plan, x);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0] == rowOfCode(plan, 512));
    auto const at = static_cast<std::size_t>(rows[0]) * 1920 + static_cast<std::size_t>(x);
    CHECK(flat.y[at] == 940);
    CHECK(std::abs(flat.cb[at / 2] - 512) <= 1);
    CHECK(std::abs(flat.cr[at / 2] - 512) <= 1);
    // Bars: in the red bar the R′ trace sits at 721 and is red; G′ and B′ share the 64 row.
    auto const bars = renderMode("rgb-overlay", "bars75", &plan);
    int const red = plan.layout.plotX + 11 * plan.layout.plotW / 16;
    auto const redRows = litRows(bars, plan, red);
    REQUIRE(redRows.size() == 2);
    CHECK(redRows[0] == rowOfCode(plan, 721));
    CHECK(redRows[1] == rowOfCode(plan, 64));
    CHECK(bars.cr[(static_cast<std::size_t>(redRows[0]) * 1920 + static_cast<std::size_t>(red)) / 2] > 700);
}

TEST_CASE("golden hashes of the other modes")
{
    // As "golden hashes of the default picture": a change here must be deliberate;
    // `mxl-scopes-bench --golden DIR` writes the PNGs.
    struct Golden
    {
        char const* mode;
        char const* pattern;
        std::uint64_t hash;
    };
    for (auto const& golden : {Golden{"ycbcr-parade", "bars75", 4875393164406064390ull}, Golden{"ycbcr-parade", "scene", 15272075516455124313ull},
             Golden{"rgb-parade", "bars75", 5859775929604755087ull}, Golden{"rgb-parade", "scene", 3552925228055185350ull},
             Golden{"rgb-overlay", "bars75", 8764227533769970585ull}, Golden{"rgb-overlay", "scene", 5015253149129364499ull}})
    {
        DisplaySettings settings;
        settings.mode = golden.mode;
        auto const frame = packPattern(makePattern(golden.pattern, 1920, 1080, 512));
        ScopeEngine engine(SimdLevel::Scalar);
        engine.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080);
        std::vector<std::uint8_t> out(engine.packedSize());
        engine.analyse(frame.data());
        engine.render(modeTitle(golden.mode), "", out.data());
        auto const hash = fnv1a(out);
        std::string const note = std::string(golden.mode) + " " + golden.pattern + " " + std::to_string(hash);
        MESSAGE(note);
        if (golden.hash != 0)
        {
            CHECK(hash == golden.hash);
        }
    }
}

TEST_CASE("accumulate: the intensity follows the grains analysed; 32-bit cells when 16 bits do not hold the update")
{
    DisplaySettings settings;
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920), 1, 2}, settings, 1920, 1080);
    CHECK(engine.lineStep() == 1);
    CHECK_FALSE(engine.analysePlan().wide);
    CHECK(engine.maxGrains() >= 2);
    // 51 frames (1 Hz from 50p, plus one) do not fit 16-bit cells: 32-bit cells, every line.
    engine.configure({1920, 1080, v210RowBytes(1920), 1, 51}, settings, 1920, 1080);
    CHECK(engine.lineStep() == 1);
    CHECK(engine.analysePlan().wide);
    CHECK(engine.histogram().wide);
    CHECK(engine.maxGrains() >= 51);
    CHECK(static_cast<std::uint64_t>(engine.analysePlan().maxCellCount) * static_cast<std::uint64_t>(engine.maxGrains()) <= 0xffffffffull);
    // One, two or three frames of a flat field render alike: normalised to the grains analysed
    // (a 50p input at 12.5 Hz gives 4, at 20 Hz 2 or 3 frames per update; no flicker).
    auto const frame = packPattern(makePattern("flat", 1920, 1080, 512));
    std::vector<std::uint8_t> one(engine.packedSize());
    engine.configure({1920, 1080, v210RowBytes(1920), 1, 4}, settings, 1920, 1080);
    engine.analyse(frame.data());
    engine.render("", "", one.data());
    for (int n : {2, 3, 4})
    {
        CAPTURE(n);
        std::vector<std::uint8_t> more(engine.packedSize());
        for (int i = 0; i < n; ++i)
        {
            engine.analyse(frame.data());
        }
        CHECK(engine.grains() == n);
        engine.render("", "", more.data());
        CHECK(more == one);
    }
}

TEST_CASE("modes: a UHD input on a small output never overflows a cell (every mode, 32-bit cells, every line)")
{
    // A parade panel of a 384×216 output has 66 columns: 3840 / 66 × 2160 samples per cell
    // would pass 16 bits. The engine takes 32-bit cells and keeps every line (deviation 23 closed).
    auto const uhd = packPattern(makePattern("random", 3840, 2160, 0, 21));
    auto const fields = packPattern(makePattern("random", 1920, 540, 0, 22));
    for (auto const* mode : {"luma", "ycbcr-parade", "rgb-parade", "rgb-overlay"})
    {
        for (auto [w, h] : {std::pair{320, 180}, std::pair{384, 216}, std::pair{640, 360}, std::pair{1920, 1080}})
        {
            for (int frames : {1, 61})
            {
                CAPTURE(mode);
                CAPTURE(w);
                CAPTURE(frames);
                DisplaySettings settings;
                settings.mode = mode;
                ScopeEngine engine(SimdLevel::Scalar);
                REQUIRE_NOTHROW(engine.configure({3840, 2160, v210RowBytes(3840), 1, frames}, settings, w, h));
                auto const& plan = engine.analysePlan();
                CHECK(engine.lineStep() == 1);
                CHECK(static_cast<std::uint64_t>(plan.maxCellCount) * static_cast<std::uint64_t>(engine.maxGrains()) <= (plan.wide ? 0xffffffffull : 65535ull));
                CHECK(engine.maxGrains() >= frames);
                compareCounts(plan, uhd);
                std::vector<std::uint8_t> out(engine.packedSize());
                engine.analyse(uhd.data());
                engine.render(modeTitle(mode), "", out.data());
                // Interlaced: two fields per update.
                ScopeEngine fieldEngine(SimdLevel::Scalar);
                REQUIRE_NOTHROW(fieldEngine.configure({1920, 540, v210RowBytes(1920), 2, frames}, settings, w, h));
                CHECK(fieldEngine.maxGrains() >= 2 * frames);
            }
        }
    }
}

TEST_CASE("modes: 32-bit cells give the picture of 16-bit cells")
{
    // The same grain in an engine sized for one frame per update (16-bit cells) and in one sized
    // for 200 (32-bit cells): normalised to the grains analysed, the pictures are equal.
    auto const frame = packPattern(makePattern("scene", 1920, 1080, 512));
    for (auto const* mode : {"luma", "ycbcr-parade", "rgb-parade", "rgb-overlay"})
    {
        CAPTURE(mode);
        DisplaySettings settings;
        settings.mode = mode;
        ScopeEngine narrow(SimdLevel::Scalar);
        ScopeEngine wide(chooseSimd("auto").level);
        narrow.configure({1920, 1080, v210RowBytes(1920), 1, 1}, settings, 1920, 1080);
        wide.configure({1920, 1080, v210RowBytes(1920), 1, 200}, settings, 1920, 1080);
        REQUIRE_FALSE(narrow.analysePlan().wide);
        REQUIRE(wide.analysePlan().wide);
        std::vector<std::uint8_t> a(narrow.packedSize());
        std::vector<std::uint8_t> b(wide.packedSize());
        narrow.analyse(frame.data());
        wide.analyse(frame.data());
        narrow.render("", "", a.data());
        wide.render("", "", b.data());
        CHECK(a == b);
    }
}
