#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "media/v210.hpp"
#include "scope/analyse.hpp"
#include "scope/settings.hpp"

namespace scopes
{
// Where the plot sits in the output raster (SPECIFICATION §5.2): 8 % left margin for the
// labels, 2 % right, 4 % top and bottom (at least one text line on top for the caption).
struct Layout
{
    int width = 0;
    int height = 0;
    int plotX = 0;
    int plotY = 0;
    int plotW = 0;
    int plotH = 0;
    // Scale of the 8×8 font: 16 px text at 1080 lines.
    int textScale = 1;
};

Layout makeLayout(int width, int height);
// The histogram size for an input `inputWidth` wide (0: no input): columns min(plotW, width),
// bins min(plotH, 1024).
int histogramColumns(Layout const& layout, int inputWidth);
int histogramBins(Layout const& layout);

struct GraticulePixel
{
    std::uint16_t x = 0;
    std::uint16_t row = 0;
    std::uint16_t alpha = 0; // 0..1023
    std::uint16_t luma = 0;
};

struct GraticuleLine
{
    int code = 0;
    int row = 0;            // output row
    std::string label;      // empty: not labelled
    std::string style;      // solid, thin, dotted
};

// Built once per settings, input format and raster change (§2.3).
struct RenderPlan
{
    Layout layout;
    int columns = 0;
    int bins = 0;
    // Per plot pixel column: the histogram column; per plot row (top first): the bin.
    std::vector<std::uint32_t> columnOf;
    std::vector<std::uint16_t> binOfRow;
    // True when every plot column shows its own histogram column (columns == plotW).
    bool identityColumns = false;
    // Count -> trace intensity 0..1023 (§4.4), indexed by min(count, maxCount).
    std::vector<std::uint16_t> intensity;
    std::uint32_t maxCount = 0;
    // Intensity -> Y′CbCr of the trace over the background.
    std::array<std::uint16_t, 1024> yOf{};
    std::array<std::uint16_t, 1024> cbOf{};
    std::array<std::uint16_t, 1024> crOf{};
    std::vector<GraticuleLine> lines;
    // Graticule pixels inside the plot, blended over the trace on every render, sorted by
    // row; the pixels of plot row r are [graticuleStart[r], graticuleStart[r + 1]).
    std::vector<GraticulePixel> plotGraticule;
    std::vector<std::uint32_t> graticuleStart;
    // Background, labels and everything else outside the plot.
    Frame422 base;
};

inline constexpr std::uint16_t kBackgroundY = 64;
inline constexpr std::uint16_t kBackgroundC = 512;

// The plot row of a 10-bit code (the middle row of the rows that show its bin).
int rowOfCode(RenderPlan const& plan, int code);
// `samples` and `maxCount` come from the analyse plan; zero without an input.
RenderPlan makeRenderPlan(Layout const& layout, DisplaySettings const& settings, int columns, std::uint64_t samples, std::uint32_t maxCount);

// Intensity table of §4.4: c = 0 is the background, c >= 1 at least traceMin.
std::vector<std::uint16_t> intensityTable(DisplaySettings const& settings, double cRef, std::uint32_t maxCount);

// The plot without a trace (background and graticule) into `frame`, a copy of plan.base.
void renderEmptyPlot(RenderPlan const& plan, Frame422& frame);
// Caption line in the top margin and the state text in the middle of the plot (§5.2).
void renderTexts(RenderPlan const& plan, Frame422& frame, std::string const& caption, std::string const& state);
// Histogram -> packed v210, line by line: the rows outside the plot and the margins come from
// `still` (the picture of renderEmptyPlot and renderTexts) and `stillPacked`, the plot from
// the counts with the graticule over them. Clears the counts.
void renderPacked(RenderPlan const& plan, Histogram& hist, Frame422 const& still, std::uint8_t const* stillPacked, std::uint8_t* dst);

// 8×8 bitmap text (ASCII), `scale` pixels per font pixel, neutral chroma.
void drawText(Frame422& frame, int x, int y, int scale, std::string const& text, std::uint16_t luma);
int textWidth(std::string const& text, int scale);
} // namespace scopes
