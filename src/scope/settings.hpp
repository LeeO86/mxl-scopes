#pragma once

#include <string>

namespace scopes
{
// The live display settings (SPECIFICATION §8.1). Immutable once built: REST and the UI
// build a new one and swap it, so one scope frame never mixes two settings (§2.3).
struct DisplaySettings
{
    std::string mode = "luma";
    // Updates per second; 0 is `frame` (every input grain).
    double updateRate = 25;
    std::string accumulate = "latest";
    int lineStep = 1;
    int pixelStep = 1;
    // BT.709 narrow range when true, else full range.
    bool limitedRange = true;
    std::string graticuleUnits = "mv";
    int graticuleLevel = 60;
    int intensity = 100;
    int traceMin = 96;
    std::string traceColor = "green";
    bool caption = true;
    std::string layout = "single";
    int bgAlpha = 0;
    int inputOffsetGrains = 0;
    int holdMs = 1000;
    int previewFps = 25;

    [[nodiscard]] bool operator==(DisplaySettings const&) const = default;
};

// Display title of a mode ("Luma waveform"), also the output flow's description (§7.1).
inline std::string modeTitle(std::string const& mode)
{
    if (mode == "luma")
    {
        return "Luma waveform";
    }
    if (mode == "ycbcr-parade")
    {
        return "YCbCr parade";
    }
    if (mode == "rgb-parade")
    {
        return "RGB parade";
    }
    if (mode == "rgb-overlay")
    {
        return "RGB overlay";
    }
    if (mode == "ycbcr-overlay")
    {
        return "YCbCr overlay";
    }
    return mode;
}
} // namespace scopes
