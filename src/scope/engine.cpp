#include "scope/engine.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace scopes
{
bool ScopePainter::configure(PaintParams const& params)
{
    if (configured_ && params == params_)
    {
        return false;
    }
    render_ = makeRenderPlan(makeLayout(params.outputWidth, params.outputHeight, panelCount(params.settings.mode)), params.settings, params.columns, params.counts,
        params.key);
    int const parts = pool_ ? pool_->threads() : 1;
    rowSplit_ = traceRowSplit(render_, parts);
    parts_.resize(static_cast<std::size_t>(parts));
    params_ = params;
    grains_ = 1;
    configured_ = true;
    stillValid_ = false;
    ++stillVersion_;
    return true;
}

bool ScopePainter::trace(Histogram& hist, std::string const& caption, std::string const& state, std::vector<TraceGroup>& out, int grains, BinTotals* totals)
{
    if (!stillValid_ || caption != caption_ || state != state_)
    {
        // The parts that change only with the settings, the caption or the state.
        still_ = render_.base;
        renderEmptyPlot(render_, still_);
        renderTexts(render_, still_, caption, state);
        // A new buffer: the old one may still be in use by the output writer.
        auto packed = std::make_shared<std::vector<std::uint8_t>>(packedSize());
        packV210(still_, packed->data(), v210RowBytes(still_.width));
        if (params_.key)
        {
            packAlpha(still_, packed->data() + render_.fillBytes);
        }
        stillPacked_ = std::move(packed);
        caption_ = caption;
        state_ = state;
        stillValid_ = true;
        ++stillVersion_;
    }
    if (!state.empty() || !params_.hasInput)
    {
        out.clear();
        hist.clear();
        return false;
    }
    grains = std::max(1, grains);
    if (grains != grains_)
    {
        // §4.4: normalised to the samples of the grains this update holds.
        PlaneCounts counts = params_.counts;
        for (std::size_t p = 0; p < 3; ++p)
        {
            counts.samples[p] *= static_cast<std::uint64_t>(grains);
            counts.maxCount[p] *= static_cast<std::uint32_t>(grains);
        }
        setIntensity(render_, params_.settings, counts);
        grains_ = grains;
    }
    if (totals != nullptr)
    {
        totals->reset(render_.bins, render_.planes);
    }
    if (pool_ == nullptr || pool_->threads() == 1)
    {
        collectTrace(render_, hist, out, 0, -1, totals);
        return true;
    }
    // Strip workers: each draws its share of the plot rows; in order, the parts are the trace.
    pool_->run([&](int k) {
        auto const i = static_cast<std::size_t>(k);
        collectTrace(render_, hist, parts_[i].groups, rowSplit_[i], rowSplit_[i + 1], totals);
    });
    out.clear();
    for (auto const& part : parts_)
    {
        out.insert(out.end(), part.groups.begin(), part.groups.end());
    }
    return true;
}

bool ScopePainter::render(Histogram& hist, std::string const& caption, std::string const& state, std::uint8_t* dst, int grains, BinTotals* totals)
{
    bool const drawn = trace(hist, caption, state, trace_, grains, totals);
    std::memcpy(dst, stillPacked_->data(), stillPacked_->size());
    applyTrace(trace_, dst);
    return drawn;
}

ScopeEngine::ScopeEngine(SimdLevel level, int stripColumns, int threads)
    : level_(level)
    , stripColumns_(stripColumns)
{
    if (threads > 1)
    {
        pool_ = std::make_unique<WorkerPool>(std::min(threads, 8));
        painter_.setWorkers(pool_.get());
    }
}


bool ScopeEngine::configure(InputFormat const& input, DisplaySettings const& settings, int outputWidth, int outputHeight, bool key)
{
    if (configured_ && input == input_ && settings == settings_ && outputWidth == outputWidth_ && outputHeight == outputHeight_ && key == key_)
    {
        return false;
    }
    auto const layout = makeLayout(outputWidth, outputHeight, panelCount(settings.mode));
    int const columns = histogramColumns(layout, input.width);
    int const bins = histogramBins(layout);
    maxGrains_ = 1;
    grains_ = 0;
    lineStep_ = settings.lineStep;
    if (input.width > 0)
    {
        // The counts of an update hold `frames` frames of `fields` grains (§4.3): 16-bit cells
        // when they fit, else 32-bit cells (AnalysePlan::wide).
        auto const grainsWanted = std::max(1, input.fields) * std::max(1, input.frames);
        analyse_ = makeAnalysePlan(input.width, input.height, input.stride, std::max(1, settings.lineStep), settings.pixelStep, columns, bins, stripColumns_,
            scopeKindOf(settings.mode), grainsWanted, threads());
        auto const perGrain = std::max<std::uint64_t>(1, analyse_.maxCellCount);
        maxGrains_ = static_cast<int>(std::min<std::uint64_t>(1u << 20, std::max<std::uint64_t>(1, (analyse_.wide ? 0xffffffffull : 65535ull) / perGrain)));
        hist_.reset(analyse_);
    }
    else
    {
        analyse_ = AnalysePlan{};
        hist_ = Histogram{};
    }
    stripSplit_ = analyse_.partStart.empty() ? std::vector<int>(static_cast<std::size_t>(threads()) + 1, 0) : analyse_.partStart;
    input_ = input;
    settings_ = settings;
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;
    key_ = key;
    configured_ = true;
    painter_.configure(paintParams());
    return true;
}

PaintParams ScopeEngine::paintParams() const
{
    auto const layout = makeLayout(outputWidth_, outputHeight_, panelCount(settings_.mode));
    // Per grain; the painter scales them by the grains of each update.
    PlaneCounts counts;
    for (std::size_t p = 0; p < 3; ++p)
    {
        auto const plane = static_cast<int>(p) < analyse_.planes ? p : 0;
        counts.samples[p] = analyse_.planeSamples[plane];
        counts.maxCount[p] = analyse_.planeMaxCell[plane];
    }
    return PaintParams{settings_, outputWidth_, outputHeight_, histogramColumns(layout, input_.width), counts, input_.width > 0, key_};
}

void ScopeEngine::fit(Histogram& hist) const
{
    bool const sized = hist.columns == analyse_.columns && hist.bins == analyse_.bins && hist.planes == analyse_.planes && hist.wide == analyse_.wide &&
                       hist.scratchCells() == (analyse_.twoSubs ? hist.cells() : 0) &&
                       hist.cells() == analyse_.planeCells * static_cast<std::size_t>(analyse_.planes);
    if (!sized)
    {
        hist.reset(analyse_);
    }
}

std::uint64_t ScopeEngine::analyseRange(std::uint8_t const* v210, Histogram& hist, StripRange range) const
{
    if (level_ == SimdLevel::Avx512)
    {
        return analyseAvx512(analyse_, v210, hist, range);
    }
    if (level_ == SimdLevel::Avx2)
    {
        return analyseAvx2(analyse_, v210, hist, range);
    }
    return analyseScalar(analyse_, v210, hist, range);
}

void ScopeEngine::analyse(std::uint8_t const* v210, Histogram& hist) const
{
    if (input_.width <= 0)
    {
        return;
    }
    fit(hist);
    if (pool_ == nullptr)
    {
        hist.outOfGamut += analyseRange(v210, hist, {});
        return;
    }
    // Strip workers: each analyses its strips; their columns are disjoint (splitStrips). Each
    // gamut count on its own cache line.
    struct alignas(64) Count
    {
        std::uint64_t value = 0;
    };
    std::array<Count, 8> outOfGamut{};
    pool_->run([&](int k) {
        auto const i = static_cast<std::size_t>(k);
        outOfGamut[i].value = analyseRange(v210, hist, {stripSplit_[i], stripSplit_[i + 1]});
    });
    for (auto const& count : outOfGamut)
    {
        hist.outOfGamut += count.value;
    }
}

void ScopeEngine::analyse(std::uint8_t const* v210)
{
    if (input_.width > 0)
    {
        analyse(v210, hist_);
        ++grains_;
    }
}

void ScopeEngine::clear()
{
    grains_ = 0;
    hist_.clear();
}

void ScopeEngine::measureUpdate(int grains)
{
    auto const pixels = analyse_.planeSamples[0] * static_cast<std::uint64_t>(std::max(1, grains));
    measurements_ = measure(analyse_.kind, totals_, hist_.outOfGamut, pixels);
    measurements_.update = ++measuredUpdates_;
    measurements_.mode = settings_.mode;
}

void ScopeEngine::render(std::string const& caption, std::string const& state, std::uint8_t* dst)
{
    if (painter_.render(hist_, caption, state, dst, grains_, analyse_.kind == ScopeKind::Rgb ? nullptr : &totals_))
    {
        measureUpdate(grains_);
    }
    hist_.outOfGamut = 0;
    grains_ = 0;
}

void ScopeEngine::trace(std::string const& caption, std::string const& state, std::vector<TraceGroup>& out)
{
    if (painter_.trace(hist_, caption, state, out, grains_, analyse_.kind == ScopeKind::Rgb ? nullptr : &totals_))
    {
        measureUpdate(grains_);
    }
    hist_.outOfGamut = 0;
    grains_ = 0;
}
} // namespace scopes
