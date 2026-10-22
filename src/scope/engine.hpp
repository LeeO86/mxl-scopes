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
#include "scope/workers.hpp"

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
    // Frames one update must be able to hold: SCOPES_ACCUMULATE=all adds every frame between two
    // updates; `latest` 1. The engine takes 32-bit cells when they would not fit 16 bits.
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
    // Per grain (one frame, or one field of an interlaced flow); the painter scales them by the
    // grains an update holds.
    PlaneCounts counts;
    bool hasInput = false;
    // video/v210a: a key plane after the fill (SPECIFICATION §7).
    bool key = false;
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
    // `grains` is the number of grains in the counts; the intensity is normalised to it (§4.4).
    bool render(Histogram& hist, std::string const& caption, std::string const& state, std::uint8_t* dst, int grains = 1, BinTotals* totals = nullptr);
    // The same picture as the still picture plus trace groups: the output writer rewrites only
    // the groups that differ from what an output grain already holds (§2.4).
    // With `totals`, the bin totals of the counts for the measurements (§4.7); returns true when
    // the counts were drawn (an input and no state text), so `totals` holds them.
    bool trace(Histogram& hist, std::string const& caption, std::string const& state, std::vector<TraceGroup>& trace, int grains = 1, BinTotals* totals = nullptr);
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
        return (v210RowBytes(params_.outputWidth) + (params_.key ? alphaRowBytes(params_.outputWidth) : 0)) * static_cast<std::size_t>(params_.outputHeight);
    }
    // Strip workers for the trace step (plot rows split between them); null: one thread.
    void setWorkers(WorkerPool* pool)
    {
        pool_ = pool;
        configured_ = false;
    }

private:
    WorkerPool* pool_ = nullptr;
    std::vector<int> rowSplit_;
    // One trace per worker, each on its own cache lines: the vector's end pointer changes with
    // every group, and two workers on one line would slow each other down (false sharing).
    struct alignas(64) Part
    {
        std::vector<TraceGroup> groups;
    };
    std::vector<Part> parts_;
    PaintParams params_;
    int grains_ = 1;
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
    // `threads` > 1: the renderer and threads − 1 strip workers share the analyse and trace
    // steps (SCOPES_THREADS, §4.3); the result is the same for any thread count.
    explicit ScopeEngine(SimdLevel level, int stripColumns = kDefaultStripColumns, int threads = 1);

    // Rebuilds the tables when the input format, the settings or the raster changed.
    // Returns true when it rebuilt them.
    // `key`: the output carries a key plane (video/v210a, §7).
    bool configure(InputFormat const& input, DisplaySettings const& settings, int outputWidth, int outputHeight, bool key = false);
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
    // The measurements of the last render or trace that drew counts (§4.7); `update` is 0 and
    // every value empty before the first.
    [[nodiscard]] Measurements const& measurements() const
    {
        return measurements_;
    }

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
    // Grains (frames, or fields of an interlaced flow) one update can hold without a cell
    // overflowing; at least InputFormat::frames × fields.
    [[nodiscard]] int maxGrains() const
    {
        return maxGrains_;
    }
    // Grains analysed since the last render or trace.
    [[nodiscard]] int grains() const
    {
        return grains_;
    }
    // The line step in use: SCOPES_LINE_STEP (32-bit cells take what does not fit 16 bits).
    [[nodiscard]] int lineStep() const
    {
        return lineStep_;
    }
    // The packed picture: the v210 fill, and the key plane after it when the output has a key.
    [[nodiscard]] std::size_t packedSize() const
    {
        return (v210RowBytes(outputWidth_) + (key_ ? alphaRowBytes(outputWidth_) : 0)) * static_cast<std::size_t>(outputHeight_);
    }
    [[nodiscard]] int threads() const
    {
        return pool_ ? pool_->threads() : 1;
    }
    // Strip boundaries of the workers (threads() + 1 indices into AnalysePlan::strips).
    [[nodiscard]] std::vector<int> const& stripSplit() const
    {
        return stripSplit_;
    }

private:
    std::uint64_t analyseRange(std::uint8_t const* v210, Histogram& hist, StripRange range) const;

    SimdLevel level_;
    int stripColumns_;
    std::unique_ptr<WorkerPool> pool_;
    std::vector<int> stripSplit_;
    bool configured_ = false;
    InputFormat input_;
    int maxGrains_ = 1;
    int grains_ = 0;
    int lineStep_ = 1;
    DisplaySettings settings_;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    bool key_ = false;
    AnalysePlan analyse_;
    Histogram hist_;
    ScopePainter painter_;
    BinTotals totals_;
    Measurements measurements_;
    std::uint64_t measuredUpdates_ = 0;
    void measureUpdate(int grains);
};
} // namespace scopes
