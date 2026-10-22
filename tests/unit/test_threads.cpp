// SCOPES_THREADS (SPECIFICATION §4.3): the strip workers give the same counts and the same
// picture as one thread, for every mode, SIMD level, raster and sampling; a stress run with
// changing settings; the worker pool itself. CI also runs these under ThreadSanitizer.
#include <doctest/doctest.h>

#include "media/patterns.hpp"
#include "scope/cpu/dispatch.hpp"
#include "scope/engine.hpp"
#include "scope/workers.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <random>
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

// Faster in a sanitizer build: SCOPES_TEST_QUICK=1 takes fewer cases.
bool quick()
{
    char const* value = std::getenv("SCOPES_TEST_QUICK");
    return value != nullptr && std::string(value) == "1";
}

constexpr char const* kModes[] = {"luma", "ycbcr-parade", "rgb-parade", "rgb-overlay", "ycbcr-overlay"};
} // namespace

TEST_CASE("worker pool: every part runs once per step, the caller runs part 0")
{
    for (int threads : {1, 2, 3, 8})
    {
        WorkerPool pool(threads);
        CHECK(pool.threads() == threads);
        std::vector<std::atomic<int>> hits(static_cast<std::size_t>(threads));
        int const steps = quick() ? 200 : 2000;
        for (int step = 0; step < steps; ++step)
        {
            pool.run([&](int k) { hits[static_cast<std::size_t>(k)].fetch_add(1, std::memory_order_relaxed); });
        }
        for (auto const& h : hits)
        {
            CHECK(h.load() == steps);
        }
    }
}

TEST_CASE("strip workers: the ranges share no histogram column")
{
    for (auto const* mode : kModes)
    {
        for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{384, 216}, std::pair{640, 360}})
        {
            for (int threads : {2, 3, 8})
            {
                CAPTURE(mode);
                CAPTURE(w);
                CAPTURE(threads);
                DisplaySettings settings;
                settings.mode = mode;
                ScopeEngine engine(SimdLevel::Scalar, kDefaultStripColumns, threads);
                engine.configure({1920, 1080, v210RowBytes(1920)}, settings, w, h);
                auto const& strips = engine.analysePlan().strips;
                auto const& split = engine.stripSplit();
                REQUIRE(split.size() == static_cast<std::size_t>(threads) + 1);
                CHECK(split.front() == 0);
                CHECK(split.back() == static_cast<int>(strips.size()));
                for (std::size_t k = 1; k + 1 < split.size(); ++k)
                {
                    CHECK(split[k] >= split[k - 1]);
                    auto const i = static_cast<std::size_t>(split[k]);
                    if (i > 0 && i < strips.size())
                    {
                        CHECK(strips[i - 1].col1 < strips[i].col0);
                    }
                }
                // Real work for more than one thread at 1080p.
                if (w == 1920 && threads == 2)
                {
                    CHECK(split[1] > 0);
                    CHECK(split[1] < static_cast<int>(strips.size()));
                }
            }
        }
    }
}

TEST_CASE("strip workers: counts and picture equal one thread (every mode, SIMD level, raster, step)")
{
    auto const random = packPattern(makePattern("random", 1920, 1080, 0, 31));
    auto const scene = packPattern(makePattern("scene", 1920, 1080, 512));
    auto const uhd = packPattern(makePattern("random", 3840, 2160, 0, 32));
    struct Raster
    {
        int w;
        int h;
    };
    std::vector<Raster> rasters = {{1920, 1080}, {640, 360}, {384, 216}};
    std::vector<int> threadCounts = quick() ? std::vector<int>{3} : std::vector<int>{2, 3, 5, 8};
    for (auto level : levels())
    {
        for (auto const* mode : kModes)
        {
            for (auto const& raster : rasters)
            {
                for (int pixelStep : {1, 3})
                {
                    DisplaySettings settings;
                    settings.mode = mode;
                    settings.pixelStep = pixelStep;
                    for (bool big : {false, true})
                    {
                        if (big && (raster.w != 384 || pixelStep != 1))
                        {
                            continue; // UHD (32-bit cells for the parades) on one raster
                        }
                        auto const& frame = big ? uhd : (pixelStep == 1 ? scene : random);
                        int const width = big ? 3840 : 1920;
                        int const height = big ? 2160 : 1080;
                        InputFormat const input{width, height, v210RowBytes(width), 1, big ? 3 : 1};
                        ScopeEngine one(level);
                        one.configure(input, settings, raster.w, raster.h);
                        std::vector<std::uint8_t> expected(one.packedSize());
                        one.analyse(frame.data());
                        Histogram const counts = one.histogram();
                        one.render("caption", "", expected.data());
                        for (int threads : threadCounts)
                        {
                            CAPTURE(simdName(level));
                            CAPTURE(mode);
                            CAPTURE(raster.w);
                            CAPTURE(pixelStep);
                            CAPTURE(big);
                            CAPTURE(threads);
                            ScopeEngine many(level, kDefaultStripColumns, threads);
                            many.configure(input, settings, raster.w, raster.h);
                            many.analyse(frame.data());
                            REQUIRE(many.histogram().counts == counts.counts);
                            REQUIRE(many.histogram().counts32 == counts.counts32);
                            std::vector<std::uint8_t> got(many.packedSize());
                            many.render("caption", "", got.data());
                            REQUIRE(got == expected);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("strip workers: stress with changing settings, rasters and accumulated grains")
{
    std::mt19937 rng(20261022);
    auto const pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    std::vector<std::vector<std::uint8_t>> frames;
    for (std::uint32_t seed = 0; seed < 3; ++seed)
    {
        frames.push_back(packPattern(makePattern("random", 1920, 1080, 0, 40 + seed)));
    }
    frames.push_back(packPattern(makePattern("scene", 1920, 1080, 512)));
    frames.push_back(packPattern(makePattern("bars75", 1920, 1080, 512)));
    int const rasters[][2] = {{1920, 1080}, {1280, 720}, {640, 360}, {384, 216}};
    auto const level = chooseSimd("auto").level;
    ScopeEngine reference(level);
    ScopeEngine eight(level, kDefaultStripColumns, 8);
    ScopeEngine three(level, kDefaultStripColumns, 3);
    int const iterations = quick() ? 40 : 300;
    for (int it = 0; it < iterations; ++it)
    {
        CAPTURE(it);
        DisplaySettings settings;
        settings.mode = kModes[pick(5)];
        settings.pixelStep = std::array{1, 2, 3, 6}[static_cast<std::size_t>(pick(4))];
        settings.lineStep = 1 + pick(3);
        settings.intensity = 50 + pick(300);
        auto const& raster = rasters[pick(4)];
        InputFormat const input{1920, 1080, v210RowBytes(1920), 1, 1 + pick(4)};
        int const grains = 1 + pick(input.frames);
        std::vector<std::uint8_t> a;
        std::vector<std::uint8_t> b;
        std::vector<std::uint8_t> c;
        for (auto* engine : {&reference, &eight, &three})
        {
            engine->configure(input, settings, raster[0], raster[1]);
        }
        a.resize(reference.packedSize());
        b.resize(eight.packedSize());
        c.resize(three.packedSize());
        for (int g = 0; g < grains; ++g)
        {
            auto const& frame = frames[static_cast<std::size_t>(pick(static_cast<int>(frames.size())))];
            reference.analyse(frame.data());
            eight.analyse(frame.data());
            three.analyse(frame.data());
        }
        reference.render("", "", a.data());
        eight.render("", "", b.data());
        three.render("", "", c.data());
        REQUIRE(b == a);
        REQUIRE(c == a);
    }
}
