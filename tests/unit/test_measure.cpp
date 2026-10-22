// Measurements (SPECIFICATION §4.7): from the bins of the histograms per mode, the RGB gamut share
// counted exactly per pixel by every analyse path, and the same values for any thread count.
#include <doctest/doctest.h>

#include "media/patterns.hpp"
#include "scope/cpu/dispatch.hpp"
#include "scope/engine.hpp"
#include "scope/measure.hpp"

#include <string>
#include <vector>

using namespace scopes;

namespace
{
std::vector<SimdLevel> levels()
{
    std::vector<SimdLevel> have{SimdLevel::Scalar};
    if (avx2Compiled() && cpuHasAvx2())
    {
        have.push_back(SimdLevel::Avx2);
    }
    if (avx512Compiled() && cpuHasAvx512())
    {
        have.push_back(SimdLevel::Avx512);
    }
    return have;
}

std::vector<std::uint8_t> flat(int y, int cb, int cr, int width = 1920, int height = 1080)
{
    Frame422 frame;
    frame.allocate(width, height);
    frame.fill(static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(cb), static_cast<std::uint16_t>(cr));
    return packPattern(frame);
}

Measurements measureOnce(std::string const& mode, std::vector<std::uint8_t> const& frame, SimdLevel level = SimdLevel::Scalar, int threads = 1, int w = 1920, int h = 1080,
    int pixelStep = 1)
{
    DisplaySettings settings;
    settings.mode = mode;
    settings.pixelStep = pixelStep;
    ScopeEngine engine(level, kDefaultStripColumns, threads);
    engine.configure({1920, 1080, v210RowBytes(1920)}, settings, w, h);
    std::vector<TraceGroup> trace;
    engine.analyse(frame.data());
    engine.trace("", "", trace);
    return engine.measurements();
}

// Pixels with an R′G′B′ component outside 20..984, by the §4.8 formula, at a pixel step.
std::uint64_t referenceOutOfGamut(Frame422 const& frame, int pixelStep, std::uint64_t& pixels)
{
    std::uint64_t out = 0;
    pixels = 0;
    for (int y = 0; y < frame.height; ++y)
    {
        for (int x = 0; x < frame.width; ++x)
        {
            if ((x % 6) % pixelStep != 0)
            {
                continue;
            }
            auto const c = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.chromaWidth()) + static_cast<std::size_t>(x / 2);
            int r = 0;
            int g = 0;
            int b = 0;
            rgbOfYcbcr(frame.y[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)], frame.cb[c], frame.cr[c], r, g, b);
            auto const outside = [](int v) { return v < 20 || v > 984; };
            out += outside(r) || outside(g) || outside(b) ? 1 : 0;
            ++pixels;
        }
    }
    return out;
}
} // namespace

TEST_CASE("bin centre codes")
{
    for (int b = 0; b < 1024; ++b)
    {
        CHECK(binCentreCode(b, 1024) == b);
    }
    // 994 bins (a 1080-line output): every code's bin centre is within one code of it.
    for (int v = 0; v < 1024; ++v)
    {
        int const bin = (v * 994) >> 10;
        CHECK(std::abs(binCentreCode(bin, 994) - v) <= 1);
    }
}

TEST_CASE("luma measurements: flat fields, legal limits, per mode")
{
    auto const grey = measureOnce("luma", flat(512, 512, 512));
    REQUIRE(grey.lumaMin);
    CHECK(std::abs(*grey.lumaMin - 512) <= 1);
    CHECK(*grey.lumaMax == *grey.lumaMin);
    CHECK(*grey.lumaMean == doctest::Approx(*grey.lumaMin));
    CHECK(*grey.lumaBelow == 0);
    CHECK(*grey.lumaAbove == 0);
    CHECK_FALSE(grey.chromaOut);
    CHECK_FALSE(grey.rgbOutOfGamut);
    CHECK(grey.update == 1);
    CHECK(grey.mode == "luma");
    auto const dark = measureOnce("luma", flat(40, 512, 512));
    CHECK(*dark.lumaBelow == 1.0);
    auto const bright = measureOnce("luma", flat(1000, 512, 512));
    CHECK(*bright.lumaAbove == 1.0);
    // Half black (64), half white (940): the mean in the middle, nothing outside.
    auto bars = makePattern("flat", 1920, 1080, 64);
    for (int y = 0; y < 1080; ++y)
    {
        for (int x = 960; x < 1920; ++x)
        {
            bars.y[static_cast<std::size_t>(y) * 1920 + static_cast<std::size_t>(x)] = 940;
        }
    }
    auto const half = measureOnce("luma", packPattern(bars));
    CHECK(std::abs(*half.lumaMin - 64) <= 1);
    CHECK(std::abs(*half.lumaMax - 940) <= 1);
    CHECK(*half.lumaMean == doctest::Approx(502).epsilon(0.003));
    CHECK(*half.lumaBelow == 0);
    CHECK(*half.lumaAbove == 0);
    // A small output: coarser bins, documented in codes_per_bin.
    auto const small = measureOnce("luma", flat(512, 512, 512), SimdLevel::Scalar, 1, 384, 216);
    CHECK(small.codesPerBin > 5);
    CHECK(std::abs(*small.lumaMin - 512) <= small.codesPerBin);
}

TEST_CASE("chroma measurements in the YCbCr modes, none in the others")
{
    auto const legal = measureOnce("ycbcr-parade", flat(512, 512, 512));
    REQUIRE(legal.chromaOut);
    CHECK(*legal.chromaOut == 0);
    REQUIRE(legal.lumaMin);
    auto const cbHigh = measureOnce("ycbcr-parade", flat(512, 1000, 512));
    CHECK(*cbHigh.chromaOut == doctest::Approx(0.5)); // Cb out, Cr in: half the chroma samples
    auto const bothLow = measureOnce("ycbcr-parade", flat(512, 30, 30));
    CHECK(*bothLow.chromaOut == 1.0);
    CHECK_FALSE(measureOnce("luma", flat(512, 1000, 512)).chromaOut);
    auto const rgb = measureOnce("rgb-parade", flat(512, 1000, 512));
    CHECK_FALSE(rgb.chromaOut);
    CHECK_FALSE(rgb.lumaMin);
    CHECK(rgb.rgbOutOfGamut);
}

TEST_CASE("RGB gamut: exact per pixel on every path, pixel step and thread count")
{
    auto const picture = makePattern("random", 1920, 1080, 0, 51);
    auto const frame = packPattern(picture);
    for (int pixelStep : {1, 2, 3, 6})
    {
        std::uint64_t pixels = 0;
        auto const expected = referenceOutOfGamut(picture, pixelStep, pixels);
        REQUIRE(expected > 0);
        REQUIRE(expected < pixels);
        for (auto level : levels())
        {
            for (int threads : {1, 3})
            {
                for (auto const* mode : {"rgb-parade", "rgb-overlay"})
                {
                    CAPTURE(pixelStep);
                    CAPTURE(simdName(level));
                    CAPTURE(threads);
                    CAPTURE(mode);
                    auto const m = measureOnce(mode, frame, level, threads, 1920, 1080, pixelStep);
                    REQUIRE(m.rgbOutOfGamut);
                    CHECK(*m.rgbOutOfGamut == doctest::Approx(static_cast<double>(expected) / static_cast<double>(pixels)).epsilon(1e-12));
                }
            }
        }
    }
    // Colour bars at 75 % are in gamut; a flat grey too.
    CHECK(*measureOnce("rgb-parade", packPattern(makePattern("bars75", 1920, 1080))).rgbOutOfGamut == 0);
    CHECK(*measureOnce("rgb-parade", flat(512, 512, 512)).rgbOutOfGamut == 0);
}

TEST_CASE("measurements: the same for any thread count, json, nothing before an update or with a state")
{
    auto const frame = packPattern(makePattern("scene", 1920, 1080, 512));
    for (auto const* mode : {"luma", "ycbcr-parade", "rgb-parade", "rgb-overlay", "ycbcr-overlay"})
    {
        CAPTURE(mode);
        auto const one = measureOnce(mode, frame);
        auto const three = measureOnce(mode, frame, SimdLevel::Scalar, 3);
        CHECK(one == three);
    }
    DisplaySettings settings;
    ScopeEngine engine(SimdLevel::Scalar);
    engine.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080);
    CHECK(engine.measurements().update == 0);
    CHECK(measurementsJson(engine.measurements()).find("\"luma\":null") != std::string::npos);
    std::vector<TraceGroup> trace;
    engine.analyse(frame.data());
    engine.trace("", "NO SIGNAL", trace);
    CHECK(engine.measurements().update == 0);
    engine.analyse(frame.data());
    engine.trace("", "", trace);
    CHECK(engine.measurements().update == 1);
    auto const json = measurementsJson(engine.measurements());
    CHECK(json.find("\"min_code\":") != std::string::npos);
    CHECK(json.find("\"chroma\":null") != std::string::npos);
    CHECK(json.find("\"rgb\":null") != std::string::npos);
}
