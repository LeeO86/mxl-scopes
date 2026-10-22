// mxl-scopes-sample: prints the 10-bit luma of output pixels of the newest grain of a flow,
// one line per --at X,Y (x,y in pixels); with --key-height H the key sample of a video/v210a
// grain of H lines instead. For the integration tests.
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::uint16_t sample10(std::uint32_t word, int shift)
{
    return static_cast<std::uint16_t>((word >> shift) & 0x3ffu);
}
} // namespace

int main(int argc, char** argv)
{
    std::string domain;
    std::string flow;
    int width = 1920;
    int keyHeight = 0;
    std::vector<std::pair<int, int>> points;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        std::string const name = argv[i];
        std::string const value = argv[i + 1];
        if (name == "--domain")
        {
            domain = value;
        }
        else if (name == "--flow")
        {
            flow = value;
        }
        else if (name == "--width")
        {
            width = std::stoi(value);
        }
        else if (name == "--key-height")
        {
            keyHeight = std::stoi(value);
        }
        else if (name == "--at")
        {
            auto const comma = value.find(',');
            points.emplace_back(std::stoi(value.substr(0, comma)), std::stoi(value.substr(comma + 1)));
        }
    }
    if (domain.empty() || flow.empty() || points.empty())
    {
        std::cerr << "usage: mxl-scopes-sample --domain DIR --flow UUID --width W [--key-height H] --at X,Y [--at X,Y ...]\n";
        return 2;
    }
    auto* instance = mxlCreateInstance(domain.c_str(), nullptr);
    if (instance == nullptr)
    {
        return 1;
    }
    mxlFlowReader reader = nullptr;
    for (int attempt = 0; attempt < 50 && mxlCreateFlowReader(instance, flow.c_str(), nullptr, &reader) != MXL_STATUS_OK; ++attempt)
    {
        reader = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (reader == nullptr)
    {
        std::cerr << "no reader\n";
        return 1;
    }
    mxlGrainInfo grain{};
    std::uint8_t* payload = nullptr;
    mxlStatus status = MXL_ERR_FLOW_NOT_FOUND;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        mxlFlowInfo info{};
        if (mxlFlowReaderGetInfo(reader, &info) == MXL_STATUS_OK)
        {
            status = mxlFlowReaderGetGrain(reader, info.runtime.headIndex, 40000000, &grain, &payload);
            if (status == MXL_STATUS_OK && payload != nullptr && (grain.flags & MXL_GRAIN_FLAG_INVALID) == 0)
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    if (status != MXL_STATUS_OK || payload == nullptr)
    {
        std::cerr << "no grain\n";
        return 1;
    }
    std::size_t const rowBytes = static_cast<std::size_t>((width + 47) / 48) * 128u;
    for (auto const& [x, y] : points)
    {
        if (keyHeight > 0)
        {
            // The key plane follows the fill: ceil(width / 3) words per line.
            std::size_t const keyRow = static_cast<std::size_t>((width + 2) / 3) * 4u;
            std::uint32_t word = 0;
            std::memcpy(&word, payload + rowBytes * static_cast<std::size_t>(keyHeight) + static_cast<std::size_t>(y) * keyRow + 4u * static_cast<std::size_t>(x / 3), 4);
            std::cout << sample10(word, 10 * (x % 3)) << "\n";
            continue;
        }
        auto const* line = payload + static_cast<std::size_t>(y) * rowBytes;
        std::uint32_t words[4] = {};
        std::memcpy(words, line + static_cast<std::size_t>(x / 6) * 16u, sizeof(words));
        std::uint16_t const ys[6] = {sample10(words[0], 10), sample10(words[1], 0), sample10(words[1], 20), sample10(words[2], 10), sample10(words[3], 0),
            sample10(words[3], 20)};
        std::cout << ys[x % 6] << "\n";
    }
    mxlReleaseFlowReader(instance, reader);
    mxlDestroyInstance(instance);
    return 0;
}
