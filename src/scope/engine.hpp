#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scope/analyse.hpp"
#include "scope/cpu/dispatch.hpp"
#include "scope/render.hpp"
#include "scope/settings.hpp"

namespace scopes
{
struct InputFormat
{
    int width = 0;  // 0: no input
    int height = 0;
    std::size_t stride = 0;
    [[nodiscard]] bool operator==(InputFormat const&) const = default;
};

// The CPU engine behind the interface of SPECIFICATION §3.2: configure, analyse, render.
// A later CUDA backend implements the same steps with the same integer tables.
class ScopeEngine
{
public:
    explicit ScopeEngine(SimdLevel level, int stripColumns = kDefaultStripColumns);

    // Rebuilds the tables when the input format, the settings or the raster changed.
    // Returns true when it rebuilt them.
    bool configure(InputFormat const& input, DisplaySettings const& settings, int outputWidth, int outputHeight);
    // Adds one v210 frame (the configured input format) to the histograms.
    void analyse(std::uint8_t const* v210);
    // Histograms -> packed v210 picture in `dst` (packedSize() bytes); the histograms are clear
    // afterwards. With a state text (NOT ROUTED, NO SIGNAL, ...) the plot shows no trace.
    void render(std::string const& caption, std::string const& state, std::uint8_t* dst);

    // The picture without a trace, with the last caption and state.
    [[nodiscard]] Frame422 const& still() const
    {
        return still_;
    }
    [[nodiscard]] Histogram const& histogram() const
    {
        return hist_;
    }
    [[nodiscard]] AnalysePlan const& analysePlan() const
    {
        return analyse_;
    }
    [[nodiscard]] RenderPlan const& renderPlan() const
    {
        return render_;
    }
    [[nodiscard]] SimdLevel level() const
    {
        return level_;
    }
    [[nodiscard]] std::size_t packedSize() const
    {
        return v210RowBytes(outputWidth_) * static_cast<std::size_t>(outputHeight_);
    }

private:
    SimdLevel level_;
    int stripColumns_;
    bool configured_ = false;
    InputFormat input_;
    DisplaySettings settings_;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    AnalysePlan analyse_;
    Histogram hist_;
    RenderPlan render_;
    Frame422 still_;
    std::vector<std::uint8_t> stillPacked_;
    std::string caption_;
    std::string state_;
    bool stillValid_ = false;
};
} // namespace scopes
