// mxl-scopes-writer: writes a test pattern (bars75, bars100, ramp, flat, random) as an MXL
// video/v210 flow at its grain rate, TAI paced. For the integration tests and the lab.
#include "config/config.hpp"
#include "media/patterns.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
std::atomic<bool> gRun{true};
void onSignal(int)
{
    gRun = false;
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
} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    auto const domain = arg(argc, argv, "--domain", "");
    auto const flow = arg(argc, argv, "--flow", "");
    auto const pattern = arg(argc, argv, "--pattern", "bars75");
    int const width = std::stoi(arg(argc, argv, "--width", "1920"));
    int const height = std::stoi(arg(argc, argv, "--height", "1080"));
    int const level = std::stoi(arg(argc, argv, "--level", "512"));
    auto const rate = scopes::parseRateToken(arg(argc, argv, "--rate", "50"));
    auto const label = arg(argc, argv, "--label", "Pattern " + pattern);
    // --interlace tff|bff: an interlaced flow; MXL grains are then fields of height/2 lines at
    // twice the grain_rate (FlowParser at 218ddaa). --invalid: every grain is committed with the
    // invalid flag (a source without signal). --partial: each grain is committed in two halves.
    auto const interlace = arg(argc, argv, "--interlace", "");
    bool const invalid = arg(argc, argv, "--invalid", "0") == "1";
    bool const partial = arg(argc, argv, "--partial", "0") == "1";
    bool const interlaced = interlace == "tff" || interlace == "bff";
    if (domain.empty() || flow.empty() || !rate || !scopes::knownPattern(pattern) || width < 1 || height < 1 || (!interlace.empty() && !interlaced) ||
        (interlaced && height % 2 != 0))
    {
        std::cerr << "usage: mxl-scopes-writer --domain DIR --flow UUID [--pattern bars75|bars100|ramp|flat|random|camera] [--level CODE]\n"
                     "                         [--width 1920 --height 1080 --rate 50|5994|N/D] [--label TEXT]\n"
                     "                         [--interlace tff|bff (rate 25 or 2997)] [--invalid 1] [--partial 1]\n";
        return 2;
    }
    auto* instance = mxlCreateInstance(domain.c_str(), nullptr);
    if (instance == nullptr)
    {
        std::cerr << "cannot open the domain " << domain << "\n";
        return 1;
    }
    std::string const def = std::string("{\"id\":\"") + flow + "\",\"format\":\"urn:x-nmos:format:video\",\"label\":\"" + label +
                            "\",\"description\":\"mxl-scopes test pattern\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"Pattern:Video\"]},\"parents\":[]," +
                            "\"media_type\":\"video/v210\",\"grain_rate\":{\"numerator\":" + std::to_string(rate->num) + ",\"denominator\":" +
                            std::to_string(rate->den) + "},\"frame_width\":" + std::to_string(width) + ",\"frame_height\":" + std::to_string(height) +
                            ",\"interlace_mode\":\"" + (interlaced ? "interlaced_" + interlace : std::string("progressive")) + "\",\"colorspace\":\"BT709\",\"components\":[{\"name\":\"Y\",\"width\":" + std::to_string(width) +
                            ",\"height\":" + std::to_string(height) + ",\"bit_depth\":10},{\"name\":\"Cb\",\"width\":" + std::to_string(width / 2) + ",\"height\":" +
                            std::to_string(height) + ",\"bit_depth\":10},{\"name\":\"Cr\",\"width\":" + std::to_string(width / 2) + ",\"height\":" +
                            std::to_string(height) + ",\"bit_depth\":10}]}";
    mxlFlowWriter writer = nullptr;
    mxlFlowConfigInfo flowConfig{};
    if (mxlCreateFlowWriter(instance, def.c_str(), "{\"maxCommitBatchSizeHint\":1}", &writer, &flowConfig, nullptr) != MXL_STATUS_OK)
    {
        std::cerr << "cannot create the flow writer\n";
        return 1;
    }
    auto const frame = scopes::packPattern(scopes::makePattern(pattern, width, height, level, 1));
    auto const rowBytes = scopes::v210RowBytes(width);
    // MXL's grain rate: the field rate for an interlaced flow.
    mxlRational const grainRate = flowConfig.common.grainRate.numerator > 0 ? flowConfig.common.grainRate : mxlRational{rate->num, rate->den};
    std::uint64_t index = mxlGetCurrentIndex(&grainRate);
    while (gRun.load())
    {
        auto const now = mxlGetCurrentIndex(&grainRate);
        if (now > index)
        {
            index = now;
        }
        mxlSleepUntil(mxlIndexToTimestamp(&grainRate, index));
        mxlGrainInfo grain{};
        std::uint8_t* payload = nullptr;
        if (mxlFlowWriterOpenGrain(writer, index, &grain, &payload) == MXL_STATUS_OK && payload != nullptr)
        {
            if (interlaced)
            {
                // Field of this grain: tff starts with the top field (even lines) on even indexes.
                int const first = static_cast<int>((index & 1u) ^ (interlace == "bff" ? 1u : 0u));
                for (int line = 0; line < height / 2 && static_cast<std::size_t>(line + 1) * rowBytes <= grain.grainSize; ++line)
                {
                    std::memcpy(payload + static_cast<std::size_t>(line) * rowBytes, frame.data() + static_cast<std::size_t>(2 * line + first) * rowBytes, rowBytes);
                }
            }
            else
            {
                std::memcpy(payload, frame.data(), std::min<std::size_t>(frame.size(), grain.grainSize));
            }
            grain.flags = invalid ? MXL_GRAIN_FLAG_INVALID : 0;
            if (partial && grain.totalSlices > 1)
            {
                // Half the slices first, the rest 5 ms later: the head is a partial grain meanwhile.
                grain.validSlices = static_cast<std::uint16_t>(grain.totalSlices / 2);
                mxlFlowWriterCommitGrain(writer, &grain);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (mxlFlowWriterOpenGrain(writer, index, &grain, &payload) != MXL_STATUS_OK)
                {
                    ++index;
                    continue;
                }
                grain.flags = invalid ? MXL_GRAIN_FLAG_INVALID : 0;
            }
            grain.validSlices = grain.totalSlices;
            mxlFlowWriterCommitGrain(writer, &grain);
        }
        ++index;
    }
    mxlReleaseFlowWriter(instance, writer);
    mxlDestroyInstance(instance);
    return 0;
}
