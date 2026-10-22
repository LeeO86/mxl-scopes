// mxl-scopes-bench: analyse, render and pack time of the scope engine per raster, SIMD level,
// strip width and sampling step, from generated frames (IMPLEMENTATION_PLAN.md M0).
// No libmxl. Pin it to one core when measuring: taskset -c N mxl-scopes-bench ...
#include "media/patterns.hpp"
#include "media/png.hpp"
#include "scope/cpu/dispatch.hpp"
#include "scope/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace scopes;

namespace
{
std::vector<std::string> split(std::string const& text)
{
    std::vector<std::string> out;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        if (!item.empty())
        {
            out.push_back(item);
        }
    }
    return out;
}

std::string arg(int argc, char** argv, char const* name, std::string const& fallback)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], name) == 0)
        {
            return argv[i + 1];
        }
    }
    return fallback;
}

struct Stats
{
    double median = 0;
    double p99 = 0;
};

Stats stats(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    Stats s;
    s.median = values[values.size() / 2];
    s.p99 = values[std::min(values.size() - 1, (values.size() * 99) / 100)];
    return s;
}

double ms(std::chrono::steady_clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--help") == 0)
        {
            std::cout << "mxl-scopes-bench [--rasters 1920x1080,3840x2160] [--output 1920x1080] [--simd scalar,avx2,avx512]\n"
                         "                 [--strips 32,64,128] [--steps 1/1,2/1,2/2] [--pattern random|bars75|ramp|flat]\n"
                         "                 [--iterations 200] [--modes luma,ycbcr-parade,rgb-parade,rgb-overlay] [--threads 1]\n"
                         "mxl-scopes-bench --golden DIR   (writes the golden pictures as PNG)\n";
            return 0;
        }
    }
    if (auto const dir = arg(argc, argv, "--golden", ""); !dir.empty())
    {
        // The golden pictures of the unit tests as PNGs for review (SPECIFICATION §14.1).
        for (auto const* pattern : {"ramp", "bars75", "bars100", "flat", "camera"})
        {
            ScopeEngine engine(SimdLevel::Scalar);
            engine.configure({1920, 1080, v210RowBytes(1920)}, DisplaySettings{}, 1920, 1080);
            auto const frame = packPattern(makePattern(pattern, 1920, 1080, 512));
            std::vector<std::uint8_t> out(engine.packedSize());
            engine.analyse(frame.data());
            engine.render("Luma waveform", "", out.data());
            Frame422 picture;
            picture.allocate(1920, 1080);
            unpackV210(out.data(), v210RowBytes(1920), picture);
            std::ofstream(dir + "/" + pattern + ".png", std::ios::binary) << encodePng(picture, 1);
            std::printf("%s/%s.png\n", dir.c_str(), pattern);
        }
        // The other modes: colour bars and the colour scene.
        for (auto const* mode : {"ycbcr-parade", "rgb-parade", "rgb-overlay"})
        {
            for (auto const* pattern : {"bars75", "scene"})
            {
                DisplaySettings settings;
                settings.mode = mode;
                ScopeEngine engine(SimdLevel::Scalar);
                engine.configure({1920, 1080, v210RowBytes(1920)}, settings, 1920, 1080);
                auto const frame = packPattern(makePattern(pattern, 1920, 1080, 512));
                std::vector<std::uint8_t> out(engine.packedSize());
                engine.analyse(frame.data());
                engine.render(modeTitle(mode), "", out.data());
                Frame422 picture;
                picture.allocate(1920, 1080);
                unpackV210(out.data(), v210RowBytes(1920), picture);
                std::ofstream(dir + "/" + mode + "-" + pattern + ".png", std::ios::binary) << encodePng(picture, 1);
                std::printf("%s/%s-%s.png\n", dir.c_str(), mode, pattern);
            }
        }
        return 0;
    }
    auto const rasters = split(arg(argc, argv, "--rasters", "1920x1080,3840x2160"));
    auto const output = arg(argc, argv, "--output", "1920x1080");
    auto simds = split(arg(argc, argv, "--simd", "scalar,avx2,avx512"));
    auto const strips = split(arg(argc, argv, "--strips", "32,64,128"));
    auto const steps = split(arg(argc, argv, "--steps", "1/1,2/1,2/2"));
    auto const pattern = arg(argc, argv, "--pattern", "random");
    auto const modes = split(arg(argc, argv, "--modes", "luma"));
    int const threads = std::clamp(std::stoi(arg(argc, argv, "--threads", "1")), 1, 8);
    std::vector<std::pair<std::string, std::string>> runs;
    int const iterations = std::max(5, std::stoi(arg(argc, argv, "--iterations", "200")));
    // --frames N cycles through N copies of the picture (N × 5.5 MB at 1080p beats the caches,
    // as a ring of MXL grains does); --interval-ms M sleeps between updates (a 25 Hz renderer).
    int const copies = std::max(1, std::stoi(arg(argc, argv, "--frames", "1")));
    int const intervalMs = std::max(0, std::stoi(arg(argc, argv, "--interval-ms", "0")));
    // --spin waits the interval busy instead of sleeping (the core stays awake and clocked up).
    bool const spin = arg(argc, argv, "--spin", "0") == "1";
    // --nap-ms K sleeps the interval in K ms pieces (short idles, shallow C-states).
    int const napMs = std::max(0, std::stoi(arg(argc, argv, "--nap-ms", "0")));
    // --warm-us U spins U µs after the sleep, before the timed update (clock ramp-up test).
    int const warmUs = std::max(0, std::stoi(arg(argc, argv, "--warm-us", "0")));
    for (auto const& mode : modes)
    {
        for (auto const& simd : simds)
        {
            runs.emplace_back(mode, simd);
        }
    }
    int const outW = std::stoi(output.substr(0, output.find('x')));
    int const outH = std::stoi(output.substr(output.find('x') + 1));
    bool const avx2 = cpuHasAvx2() && avx2Compiled();
    bool const avx512 = cpuHasAvx512() && avx512Compiled();
    std::printf("# cpu avx512=%s%s ", avx512 ? "yes" : "no", cpuIsSkylakeServer() ? " (skylake-sp)" : "");
    std::printf("avx2=%s pattern=%s output=%dx%d iterations=%d\n", avx2 ? "yes" : "no", pattern.c_str(), outW, outH, iterations);
    std::printf("%-13s %-10s %-7s %-6s %-6s %10s %10s %10s %10s %10s %10s\n", "mode", "raster", "simd", "strip", "step", "analyse", "an_p99", "render", "-", "total",
        "total_p99");
    for (auto const& raster : rasters)
    {
        int const w = std::stoi(raster.substr(0, raster.find('x')));
        int const h = std::stoi(raster.substr(raster.find('x') + 1));
        std::vector<std::vector<std::uint8_t>> frames(static_cast<std::size_t>(copies), packPattern(makePattern(pattern, w, h, 512, 7)));
        for (auto const& [mode, simd] : runs)
        {
            if ((simd == "avx2" && !avx2) || (simd == "avx512" && !avx512))
            {
                std::printf("%-10s %-7s (not on this CPU)\n", raster.c_str(), simd.c_str());
                continue;
            }
            for (auto const& strip : strips)
            {
                for (auto const& step : steps)
                {
                    DisplaySettings settings;
                    settings.mode = mode;
                    settings.lineStep = std::stoi(step.substr(0, step.find('/')));
                    settings.pixelStep = std::stoi(step.substr(step.find('/') + 1));
                    ScopeEngine engine(simd == "avx512" ? SimdLevel::Avx512 : simd == "avx2" ? SimdLevel::Avx2 : SimdLevel::Scalar, std::stoi(strip), threads);
                    engine.configure({w, h, v210RowBytes(w)}, settings, outW, outH);
                    std::vector<std::uint8_t> out(engine.packedSize());
                    std::vector<double> analyse;
                    std::vector<double> render;
                    std::vector<double> pack;
                    std::vector<double> total;
                    for (int i = 0; i < iterations + 3; ++i)
                    {
                        if (intervalMs > 0 && spin)
                        {
                            auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs);
                            while (std::chrono::steady_clock::now() < until)
                            {
                            }
                        }
                        else if (intervalMs > 0 && napMs > 0)
                        {
                            for (int slept = 0; slept < intervalMs; slept += napMs)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(std::min(napMs, intervalMs - slept)));
                            }
                        }
                        else if (intervalMs > 0)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
                        }
                        if (warmUs > 0)
                        {
                            auto const until = std::chrono::steady_clock::now() + std::chrono::microseconds(warmUs);
                            while (std::chrono::steady_clock::now() < until)
                            {
                            }
                        }
                        auto const& frame = frames[static_cast<std::size_t>(i) % frames.size()];
                        auto const t0 = std::chrono::steady_clock::now();
                        engine.analyse(frame.data());
                        auto const t1 = std::chrono::steady_clock::now();
                        engine.render(modeTitle(mode), "", out.data());
                        auto const t2 = std::chrono::steady_clock::now();
                        auto const t3 = t2;
                        if (i >= 3)
                        {
                            analyse.push_back(ms(t1 - t0));
                            render.push_back(ms(t2 - t1));
                            pack.push_back(ms(t3 - t2));
                            total.push_back(ms(t3 - t0));
                        }
                    }
                    auto const a = stats(analyse);
                    auto const t = stats(total);
                    std::printf("%-13s %-10s %-7s %-6s %-6s %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f\n", mode.c_str(), raster.c_str(), simd.c_str(), strip.c_str(), step.c_str(),
                        a.median, a.p99, stats(render).median, stats(pack).median, t.median, t.p99);
                    std::fflush(stdout);
                }
            }
        }
    }
    return 0;
}
