#include "scope/engine.hpp"

#include <algorithm>
#include <cstring>

namespace scopes
{
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
        hist_.reset(analyse_);
    }
    else
    {
        analyse_ = AnalysePlan{};
        hist_ = Histogram{};
    }
    render_ = makeRenderPlan(layout, settings, columns, analyse_.samples, analyse_.maxCellCount);
    stillValid_ = false;
    input_ = input;
    settings_ = settings;
    outputWidth_ = outputWidth;
    outputHeight_ = outputHeight;
    configured_ = true;
    return true;
}

void ScopeEngine::analyse(std::uint8_t const* v210)
{
    if (input_.width <= 0)
    {
        return;
    }
    if (level_ == SimdLevel::Avx2)
    {
        analyseAvx2(analyse_, v210, hist_);
    }
    else
    {
        analyseScalar(analyse_, v210, hist_);
    }
}

void ScopeEngine::clear()
{
    std::fill(hist_.counts.begin(), hist_.counts.end(), std::uint16_t{0});
    std::fill(hist_.scratch.begin(), hist_.scratch.end(), std::uint16_t{0});
}

void ScopeEngine::render(std::string const& caption, std::string const& state, std::uint8_t* dst)
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
    }
    if (!state.empty() || input_.width <= 0)
    {
        std::memcpy(dst, stillPacked_.data(), stillPacked_.size());
        std::fill(hist_.counts.begin(), hist_.counts.end(), std::uint16_t{0});
        return;
    }
    renderPacked(render_, hist_, still_, stillPacked_.data(), dst);
}
} // namespace scopes
