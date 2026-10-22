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
// A parade has three panels of equal width with a 4 % gap between them (the chroma labels of
// the YCbCr parade sit in the gaps); `plotX`/`plotW` span all panels.
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
    int panels = 1;
    // Panel k starts at panelX[k] and is panelW wide; both multiples of 6 (v210 groups).
    std::array<int, 3> panelX{};
    int panelW = 0;
};

// Panels of a mode: 3 for the parades, else 1.
int panelCount(std::string const& mode);
Layout makeLayout(int width, int height, int panels = 1);
// The histogram size for an input `inputWidth` wide (0: no input): columns min(panelW, width),
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

// Samples per update and the largest count of a cell, per histogram plane (§4.4: c_ref is a
// plane's samples per column).
struct PlaneCounts
{
    std::array<std::uint64_t, 3> samples{};
    std::array<std::uint32_t, 3> maxCount{};
    [[nodiscard]] bool operator==(PlaneCounts const&) const = default;
};

// Intensity -> Y′CbCr of one trace colour over the background.
struct TraceColour
{
    std::array<std::uint16_t, 1024> y{};
    std::array<std::uint16_t, 1024> cb{};
    std::array<std::uint16_t, 1024> cr{};
};

struct PanelPlan
{
    int x = 0;      // first output column
    int plane = 0;  // histogram plane (single-plane panels)
    int colour = 0; // index into RenderPlan::colours
};

// Built once per settings, input format and raster change (§2.3).
struct RenderPlan
{
    Layout layout;
    int columns = 0;
    int bins = 0;
    int planes = 1;
    // Per panel pixel column: the histogram column; per plot row (top first): the bin.
    std::vector<std::uint32_t> columnOf;
    std::vector<std::uint16_t> binOfRow;
    // True when every panel column shows its own histogram column (columns == panelW).
    bool identityColumns = false;
    std::vector<PanelPlan> panels;
    // One panel, three planes added in R′G′B′ (rgb-overlay, §4.4).
    bool overlay = false;
    // Count -> trace intensity 0..1023 (§4.4) per plane, indexed by min(count, planeMax).
    std::array<std::vector<std::uint16_t>, 3> planeIntensity;
    std::array<std::uint32_t, 3> planeMax{};
    std::vector<TraceColour> colours;
    // Overlay: per plane and count (up to planeMax), the Y′, Cb and Cr contribution of the R′,
    // G′ or B′ trace at that count's intensity, × 64 (the intensity table folded in).
    struct OverlayEntry
    {
        std::int32_t y = 0;
        std::int32_t cb = 0;
        std::int32_t cr = 0;
    };
    std::array<std::vector<OverlayEntry>, 3> overlayColour;
    // Plane 0 and colour 0, for the luma tests and tools.
    std::vector<std::uint16_t> intensity;
    std::uint32_t maxCount = 0;
    std::array<std::uint16_t, 1024> yOf{};
    std::array<std::uint16_t, 1024> cbOf{};
    std::array<std::uint16_t, 1024> crOf{};
    // The graticule lines of the first panel.
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
// `samples` and `maxCount` come from the analyse plan; zero without an input. The mode in the
// settings decides the panels; the layout must have its panel count.
RenderPlan makeRenderPlan(Layout const& layout, DisplaySettings const& settings, int columns, PlaneCounts const& counts);
// Rebuilds the intensity tables (and the overlay's colour tables) for other counts: the number
// of grains in an update changes c_ref and the largest count, nothing else (§4.4).
void setIntensity(RenderPlan& plan, DisplaySettings const& settings, PlaneCounts const& counts);
// Luma (and tests): every plane with the same counts.
RenderPlan makeRenderPlan(Layout const& layout, DisplaySettings const& settings, int columns, std::uint64_t samples, std::uint32_t maxCount);

// Gain of the intensity curve at SCOPES_INTENSITY 100 % (§4.4): a cell with 1/20 of its
// column's samples is at 63 %, a few hits are dim, a flat field saturates.
inline constexpr double kIntensityGain = 20.0;

// Intensity table of §4.4: I = 1023 × (1 − exp(−g × c / c_ref)), g = intensity / 100 × gain;
// c = 0 is the background, c >= 1 at least traceMin.
std::vector<std::uint16_t> intensityTable(DisplaySettings const& settings, double cRef, std::uint32_t maxCount);

// The plot without a trace (background and graticule) into `frame`, a copy of plan.base.
void renderEmptyPlot(RenderPlan const& plan, Frame422& frame);
// Caption line in the top margin and the state text in the middle of the plot (§5.2).
void renderTexts(RenderPlan const& plan, Frame422& frame, std::string const& caption, std::string const& state);
// One packed v210 group (6 pixels) of the plot that carries a trace, at its byte offset in the
// packed picture. A picture is its still picture with these groups written over it.
struct TraceGroup
{
    std::uint32_t offset = 0;
    std::uint8_t bytes[16] = {};
};

// Histogram -> the plot groups with a trace (graticule blended over), row by row. Groups
// without counts keep the still picture's bytes (background and graticule). Clears the counts.
void collectTrace(RenderPlan const& plan, Histogram& hist, std::vector<TraceGroup>& trace);
void applyTrace(std::vector<TraceGroup> const& trace, std::uint8_t* dst);

// 8×8 bitmap text (ASCII), `scale` pixels per font pixel, neutral chroma.
void drawText(Frame422& frame, int x, int y, int scale, std::string const& text, std::uint16_t luma);
int textWidth(std::string const& text, int scale);
} // namespace scopes
