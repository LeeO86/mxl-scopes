#include "scope/engine.hpp"

#include <algorithm>
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
    render_ = makeRenderPlan(makeLayout(params.outputWidth, params.outputHeight, panelCount(params.settings.mode)), params.settings, params.columns, params.counts);
    params_ = params;
    grains_ = 1;
    configured_ = true;
    stillValid_ = false;
    ++stillVersion_;
    return true;
}

void ScopePainter::trace(Histogram& hist, std::string const& caption, std::string const& state, std::vector<TraceGroup>& out, int grains)
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
        stillPacked_ = std::move(packed);
        caption_ = caption;
        state_ = state;
        stillValid_ = true;
        ++stillVersion_;
    }
    if (!state.empty() || !params_.hasInput)
    {
        out.clear();
        std::fill(hist.counts.begin(), hist.counts.end(), std::uint16_t{0});
        return;
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
    collectTrace(render_, hist, out);
}

void ScopePainter::render(Histogram& hist, std::string const& caption, std::string const& state, std::uint8_t* dst, int grains)
{
    trace(hist, caption, state, trace_, grains);
    std::memcpy(dst, stillPacked_->data(), stillPacked_->size());
    applyTrace(trace_, dst);
}

ScopeEngine::ScopeEngine(SimdLevel level, int stripColumns)
    : level_(level)
    , stripColumns_(stripColumns)
{
}

bool ScopeEngine::configure(InputFormat const& input, DisplaySettings const& settings, int outputWidth, int outputHeight)
{
    if (configured_ && input == input_ && settings == settings_ && outputWidth == outputWidth_ && outputHeight == outputHeight_)
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
        // The counts of an update must fit 16-bit cells (§4.3): `frames` frames of `fields` grains.
        // Many samples per column (a UHD input on a small output, a parade's narrow panels) can
        // pass that; then every n-th line is analysed instead, raising the line step until it
        // fits (the intensity is normalised to the samples taken, so the picture stays the same).
        auto const grainsWanted = static_cast<std::uint64_t>(std::max(1, input.fields)) * static_cast<std::uint64_t>(std::max(1, input.frames));
        auto const kind = scopeKindOf(settings.mode);
        for (int step = std::max(1, settings.lineStep);; ++step)
        {
            try
            {
                auto plan = makeAnalysePlan(input.width, input.height, input.stride, step, settings.pixelStep, columns, bins, stripColumns_, kind);
                auto const perGrain = std::max<std::uint64_t>(1, plan.maxCellCount);
                bool const fits = perGrain * grainsWanted <= 65535;
                if (fits || step >= input.height)
                {
                    analyse_ = std::move(plan);
                    lineStep_ = step;
                    maxGrains_ = static_cast<int>(std::max<std::uint64_t>(1, 65535 / perGrain));
                    break;
                }
            }
            catch (CellOverflow const&)
            {
                if (step >= input.height)
                {
                    throw;
                }
            }
        }
        hist_.reset(analyse_);
    }
    else
    {
        analyse_ = AnalysePlan{};
        hist_ = Histogram{};
    }
    input_ = input;
    settings_ = settings;
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;
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
    return PaintParams{settings_, outputWidth_, outputHeight_, histogramColumns(layout, input_.width), counts, input_.width > 0};
}

void ScopeEngine::fit(Histogram& hist) const
{
    bool const sized = hist.columns == analyse_.columns && hist.bins == analyse_.bins && hist.planes == analyse_.planes &&
                       hist.scratch.size() == (analyse_.twoSubs ? hist.counts.size() : 0) &&
                       hist.counts.size() == analyse_.planeCells * static_cast<std::size_t>(analyse_.planes);
    if (!sized)
    {
        hist.reset(analyse_);
    }
}

void ScopeEngine::analyse(std::uint8_t const* v210, Histogram& hist) const
{
    if (input_.width <= 0)
    {
        return;
    }
    fit(hist);
    if (level_ == SimdLevel::Avx512)
    {
        analyseAvx512(analyse_, v210, hist);
    }
    else if (level_ == SimdLevel::Avx2)
    {
        analyseAvx2(analyse_, v210, hist);
    }
    else
    {
        analyseScalar(analyse_, v210, hist);
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
    std::fill(hist_.counts.begin(), hist_.counts.end(), std::uint16_t{0});
    std::fill(hist_.scratch.begin(), hist_.scratch.end(), std::uint16_t{0});
}

void ScopeEngine::render(std::string const& caption, std::string const& state, std::uint8_t* dst)
{
    painter_.render(hist_, caption, state, dst, grains_);
    grains_ = 0;
}

void ScopeEngine::trace(std::string const& caption, std::string const& state, std::vector<TraceGroup>& out)
{
    painter_.trace(hist_, caption, state, out, grains_);
    grains_ = 0;
}
} // namespace scopes
