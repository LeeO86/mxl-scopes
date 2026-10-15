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
    render_ = makeRenderPlan(makeLayout(params.outputWidth, params.outputHeight), params.settings, params.columns, params.samples, params.maxCount);
    params_ = params;
    configured_ = true;
    stillValid_ = false;
    ++stillVersion_;
    return true;
}

void ScopePainter::trace(Histogram& hist, std::string const& caption, std::string const& state, std::vector<TraceGroup>& out)
{
    if (!stillValid_ || caption != caption_ || state != state_)
    {
        // The parts that change only with the settings, the caption or the state.
        still_ = render_.base;
        renderEmptyPlot(render_, still_);
        renderTexts(render_, still_, caption, state);
        stillPacked_.resize(packedSize());
        packV210(still_, stillPacked_.data(), v210RowBytes(still_.width));
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
    collectTrace(render_, hist, out);
}

void ScopePainter::render(Histogram& hist, std::string const& caption, std::string const& state, std::uint8_t* dst)
{
    trace(hist, caption, state, trace_);
    std::memcpy(dst, stillPacked_.data(), stillPacked_.size());
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
    auto const layout = makeLayout(outputWidth, outputHeight);
    int const columns = histogramColumns(layout, input.width);
    int const bins = histogramBins(layout);
    if (input.width > 0)
    {
        analyse_ = makeAnalysePlan(input.width, input.height, input.stride, settings.lineStep, settings.pixelStep, columns, bins, stripColumns_);
        if (static_cast<std::uint64_t>(analyse_.maxCellCount) * static_cast<std::uint64_t>(std::max(1, input.fields)) > 65535)
        {
            throw std::invalid_argument("a histogram cell could exceed 16 bits");
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
    auto const layout = makeLayout(outputWidth_, outputHeight_);
    // Both fields of an interlaced frame go into one update: twice the samples per update.
    auto const fields = static_cast<std::uint32_t>(std::max(1, input_.fields));
    return PaintParams{settings_, outputWidth_, outputHeight_, histogramColumns(layout, input_.width), analyse_.samples * fields,
        analyse_.maxCellCount * fields, input_.width > 0};
}

void ScopeEngine::fit(Histogram& hist) const
{
    bool const sized = hist.columns == analyse_.columns && hist.bins == analyse_.bins && hist.scratch.size() == (analyse_.twoSubs ? hist.counts.size() : 0) &&
                       hist.counts.size() == static_cast<std::size_t>(analyse_.columns) * static_cast<std::size_t>(analyse_.bins);
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
    analyse(v210, hist_);
}

void ScopeEngine::clear()
{
    std::fill(hist_.counts.begin(), hist_.counts.end(), std::uint16_t{0});
    std::fill(hist_.scratch.begin(), hist_.scratch.end(), std::uint16_t{0});
}

void ScopeEngine::render(std::string const& caption, std::string const& state, std::uint8_t* dst)
{
    painter_.render(hist_, caption, state, dst);
}
} // namespace scopes
