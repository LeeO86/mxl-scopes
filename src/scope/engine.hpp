#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
    // Lines of one grain: the frame height, or half of it for an interlaced flow, whose MXL
    // grains are single fields.
    int height = 0;
    std::size_t stride = 0;
    // Grains analysed into one update: 2 for interlaced (both fields of a frame), else 1.
    int fields = 1;
    // Frames added into one update: SCOPES_ACCUMULATE=all takes every frame between two updates
    // (input rate / update rate, rounded); `latest` 1. Capped so that no cell can pass 16 bits.
    int frames = 1;
    [[nodiscard]] bool operator==(InputFormat const&) const = default;
};

// Everything the render step needs besides the counts. It travels with a histogram from the
// thread that analyses to the thread that renders (the output writer, SPECIFICATION §2.4).
struct PaintParams
{
    DisplaySettings settings;
    int outputWidth = 0;
    int outputHeight = 0;
    int columns = 0;
    PlaneCounts counts;
    bool hasInput = false;
    [[nodiscard]] bool operator==(PaintParams const&) const = default;
};

// The render side of the engine: histogram -> packed v210 picture.
class ScopePainter
{
public:
    // Rebuilds the render plan when the parameters changed; true when it did.
    bool configure(PaintParams const& params);
    // Writes the packed picture (packedSize() bytes) into `dst` and clears the counts. With a
    // state text (NOT ROUTED, NO SIGNAL, ...) or without an input the plot shows no trace.
    void render(Histogram& hist, std::string const& caption, std::string const& state, std::uint8_t* dst);
    // The same picture as the still picture plus trace groups: the output writer rewrites only
    // the groups that differ from what an output grain already holds (§2.4).
    void trace(Histogram& hist, std::string const& caption, std::string const& state, std::vector<TraceGroup>& trace);
    [[nodiscard]] std::uint8_t const* stillPacked() const
    {
        return stillPacked_->data();
    }
    // The packed still picture as a shared, never changed buffer: a new picture is a new buffer,
    // so another thread can keep using the one it has (the output writer, §2.4).
    [[nodiscard]] std::shared_ptr<std::vector<std::uint8_t> const> stillShared() const
    {
        return stillPacked_;
    }
    // Changes whenever the still picture changes (settings, caption, state).
    [[nodiscard]] std::uint64_t stillVersion() const
    {
        return stillVersion_;
    }
    [[nodiscard]] bool configured() const
    {
        return configured_;
    }
    [[nodiscard]] RenderPlan const& renderPlan() const
    {
        return render_;
    }
    // The picture without a trace, with the last caption and state.
    [[nodiscard]] Frame422 const& still() const
    {
        return still_;
    }
    [[nodiscard]] std::size_t packedSize() const
    {
        return v210RowBytes(params_.outputWidth) * static_cast<std::size_t>(params_.outputHeight);
    }

private:
    PaintParams params_;
    bool configured_ = false;
    RenderPlan render_;
    Frame422 still_;
    std::shared_ptr<std::vector<std::uint8_t>> stillPacked_ = std::make_shared<std::vector<std::uint8_t>>();
    std::string caption_;
    std::string state_;
    bool stillValid_ = false;
    std::uint64_t stillVersion_ = 0;
    std::vector<TraceGroup> trace_;
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
    // Adds one v210 frame (the configured input format) to the engine's histograms, or to
    // `hist`, which is resized (and cleared) when it does not fit the plan.
    void analyse(std::uint8_t const* v210);
    void analyse(std::uint8_t const* v210, Histogram& hist) const;
    // Sizes `hist` for the plan; clears it when it had another size.
    void fit(Histogram& hist) const;
    // Histograms -> packed v210 picture in `dst` (packedSize() bytes); the histograms are clear
    // afterwards. With a state text (NOT ROUTED, NO SIGNAL, ...) the plot shows no trace.
    void render(std::string const& caption, std::string const& state, std::uint8_t* dst);
    // The same picture as the still picture (stillShared(), stillVersion()) plus trace groups;
    // the histograms are clear afterwards.
    void trace(std::string const& caption, std::string const& state, std::vector<TraceGroup>& out);
    [[nodiscard]] std::shared_ptr<std::vector<std::uint8_t> const> stillShared() const
    {
        return painter_.stillShared();
    }
    [[nodiscard]] std::uint64_t stillVersion() const
    {
        return painter_.stillVersion();
    }
    // Drops the counts of an analysed frame (a torn read, §2.2).
    void clear();

    [[nodiscard]] PaintParams paintParams() const;
    [[nodiscard]] Frame422 const& still() const
    {
        return painter_.still();
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
        return painter_.renderPlan();
    }
    [[nodiscard]] SimdLevel level() const
    {
        return level_;
    }
    // Frames per update after the 16-bit cap (InputFormat::frames).
    [[nodiscard]] int framesPerUpdate() const
    {
        return frames_;
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
    int frames_ = 1;
    DisplaySettings settings_;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    AnalysePlan analyse_;
    Histogram hist_;
    ScopePainter painter_;
};
} // namespace scopes
