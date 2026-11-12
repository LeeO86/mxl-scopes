#include "scope/render.hpp"

#include "media/patterns.hpp"

#ifdef SCOPES_WITH_BLEND2D
#include <blend2d/blend2d.h>

#include "scope/dejavu_sans.hpp"
#else
#include <font8x8_basic.h>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace scopes
{
namespace
{
int even(int value)
{
    return value & ~1;
}

// Rounded (d × i) / 1023 for a signed difference d; the same on every backend.
int scale1023(int d, int i)
{
    return d >= 0 ? (d * i + 511) / 1023 : -((-d * i + 511) / 1023);
}

std::uint16_t blend(int under, int over, int alpha)
{
    return static_cast<std::uint16_t>((under * (1023 - alpha) + over * alpha + 511) / 1023);
}

Ycbcr10 traceColor(std::string const& name)
{
    if (name == "white")
    {
        return rgbToYcbcr709(1.0, 1.0, 1.0);
    }
    if (name == "amber")
    {
        return rgbToYcbcr709(1.0, 0.72, 0.12);
    }
    return rgbToYcbcr709(0.25, 1.0, 0.3);
}

std::string number(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", value);
    return text;
}

// The graticule of the luma panel (§4.5) as codes, labels and styles.
std::vector<GraticuleLine> lumaLines(DisplaySettings const& settings)
{
    std::vector<GraticuleLine> lines;
    auto add = [&](int code, std::string label, char const* style) { lines.push_back({std::clamp(code, 0, 1023), 0, std::move(label), style}); };
    if (settings.limitedRange)
    {
        auto const codeOfMv = [](double mv) { return static_cast<int>(std::lround(64.0 + mv * 876.0 / 700.0)); };
        if (settings.graticuleUnits == "mv")
        {
            for (int mv = 100; mv < 700; mv += 100)
            {
                add(codeOfMv(mv), number(mv), "thin");
            }
            add(64, "0", "solid");
            add(940, "700", "solid");
        }
        else if (settings.graticuleUnits == "percent")
        {
            for (int k = 1; k <= 9; ++k)
            {
                add(static_cast<int>(std::lround(64.0 + 87.6 * k)), k == 5 ? "50" : "", "thin");
            }
            add(64, "0", "solid");
            add(940, "100", "solid");
        }
        else
        {
            for (int code = 128; code < 940; code += 128)
            {
                if (code != 512)
                {
                    add(code, number(code), "thin");
                }
            }
            add(512, "512", "thin");
            add(64, "64", "solid");
            add(940, "940", "solid");
        }
        add(1019, "max", "dotted");
        add(4, "min", "dotted");
    }
    else
    {
        // Full range: 0 and 1023 are 0 % and 100 %; mV shown as mapped, 0..700 (§4.5).
        if (settings.graticuleUnits == "mv")
        {
            for (int mv = 100; mv < 700; mv += 100)
            {
                add(static_cast<int>(std::lround(mv * 1023.0 / 700.0)), number(mv), "thin");
            }
            add(0, "0", "solid");
            add(1023, "700", "solid");
        }
        else if (settings.graticuleUnits == "percent")
        {
            for (int k = 1; k <= 9; ++k)
            {
                add(static_cast<int>(std::lround(1023.0 * k / 10.0)), k == 5 ? "50" : "", "thin");
            }
            add(0, "0", "solid");
            add(1023, "100", "solid");
        }
        else
        {
            for (int code = 128; code < 1023; code += 128)
            {
                add(code, number(code), "thin");
            }
            add(0, "0", "solid");
            add(1023, "1023", "solid");
        }
    }
    return lines;
}

// The graticule of a Cb or Cr panel (§4.5): the centre at 512 (0 mV), solid lines at the ends of
// the range (∓350 mV: 64 and 960 limited, 0 and 1023 full), labels in mV, % of 700 mV or code.
std::vector<GraticuleLine> chromaLines(DisplaySettings const& settings)
{
    int const lo = settings.limitedRange ? 64 : 0;
    int const hi = settings.limitedRange ? 960 : 1023;
    std::vector<GraticuleLine> lines;
    auto label = [&](int code, char const* mv, char const* percent) {
        if (settings.graticuleUnits == "code")
        {
            return number(code);
        }
        return std::string(settings.graticuleUnits == "percent" ? percent : mv);
    };
    lines.push_back({lo, 0, label(lo, "-350", "-50"), "solid"});
    lines.push_back({512, 0, label(512, "0", "0"), "thin"});
    lines.push_back({hi, 0, label(hi, "350", "50"), "solid"});
    return lines;
}

struct ModeLook
{
    std::vector<int> planes;            // per panel
    std::vector<bool> chroma;           // per panel: the chroma graticule
    std::vector<std::string> names;     // per panel, under it (parades)
    std::vector<Ycbcr10> colours;       // per panel
    bool overlay = false;
    // Overlay: the R′G′B′ colour of each plane's trace (they add in R′G′B′).
    std::array<std::array<double, 3>, 3> overlayRgb{};
};

ModeLook modeLook(DisplaySettings const& settings)
{
    auto const white = rgbToYcbcr709(1.0, 1.0, 1.0);
    auto const red = rgbToYcbcr709(1.0, 0.3, 0.3);
    auto const green = rgbToYcbcr709(0.3, 1.0, 0.35);
    auto const blue = rgbToYcbcr709(0.35, 0.55, 1.0);
    if (settings.mode == "ycbcr-parade")
    {
        return {{0, 1, 2}, {false, true, true}, {"Y", "Cb", "Cr"}, {white, blue, red}, false};
    }
    if (settings.mode == "rgb-parade")
    {
        return {{0, 1, 2}, {false, false, false}, {"R", "G", "B"}, {red, green, blue}, false};
    }
    if (settings.mode == "rgb-overlay")
    {
        // Pure primaries: a neutral trace (R′ = G′ = B′) is white.
        return {{0}, {false}, {}, {white}, true, {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}};
    }
    if (settings.mode == "ycbcr-overlay")
    {
        // Y′ white, Cb blue, Cr red on the shared code axis (decided 2026-10-22), in the tints of
        // the YCbCr parade so a full-intensity chroma trace stays readable on black.
        return {{0}, {false}, {}, {white}, true, {{{1, 1, 1}, {0.35, 0.55, 1}, {1, 0.3, 0.3}}}};
    }
    // Luma and the vectorscope: SCOPES_TRACE_COLOR (`picture` does not use this colour).
    return {{0}, {false}, {}, {traceColor(settings.traceColor)}, false};
}

// The hue of the Cb/Cr direction (pb, pr) as a fully saturated colour: R′G′B′ of that chroma at
// zero luma, stretched to 0..1 (adding grey and scaling keep the direction).
Ycbcr10 hueColour(double pb, double pr)
{
    double rgb[3] = {1.5748 * pr, -0.187324 * pb - 0.468124 * pr, 1.8556 * pb};
    double const lo = std::min({rgb[0], rgb[1], rgb[2]});
    double const hi = std::max({rgb[0], rgb[1], rgb[2]});
    for (auto& v : rgb)
    {
        v = hi > lo ? (v - lo) / (hi - lo) : 1.0;
    }
    return rgbToYcbcr709(rgb[0], rgb[1], rgb[2]);
}

// The vectorscope graticule (§5.3) into plan.plotGraticule, behind the trace: the hue ring at the
// edge, rings every 10 % of 350 mV of chroma, the Cb and Cr axes, a vector towards each colour,
// the skin-tone line, the 75 % and 100 % target boxes and the colour names; the zoom ("2×") right
// of the plot. Drawn on a grid of the plot first, the brightest element wins a pixel.
void vectorGraticule(RenderPlan& plan, DisplaySettings const& settings, int lineLuma)
{
    auto const& layout = plan.layout;
    int const side = layout.panelW;
    int const s = layout.textScale;
    int const level = settings.graticuleLevel;
    int const width = settings.graticuleWidth;
    int const zoom = plan.zoom;
    // A chroma code -> the centre of its cell in plot pixels (x right, y down), or none outside.
    auto const position = [&](int code, bool vertical, double& at) {
        int const q = (code - 512) * zoom + kVectorCodes / 2;
        if (q < 0 || q >= kVectorCodes)
        {
            return false;
        }
        int const cell = q * (vertical ? plan.bins : plan.columns) / kVectorCodes;
        int first = -1;
        int last = -1;
        int const n = vertical ? layout.plotH : side;
        for (int i = 0; i < n; ++i)
        {
            int const c = vertical ? plan.binOfRow[static_cast<std::size_t>(i)] : static_cast<int>(plan.columnOf[static_cast<std::size_t>(i)]);
            if (c == cell)
            {
                first = first < 0 ? i : first;
                last = i;
            }
        }
        at = (first + last + 1) / 2.0;
        return first >= 0;
    };
    double cx = 0;
    double cy = 0;
    position(512, false, cx);
    position(512, true, cy);
    // Plot pixels per chroma code at this zoom; the hue ring's outer edge touches the plot.
    double const perCode = zoom * static_cast<double>(side) / kVectorCodes;
    double const outer = side / 2.0 - 0.5;
    double const ringWidth = 2.0 * s * width;
    double const inner = outer - ringWidth;
    struct Cell
    {
        std::uint16_t alpha = 0;
        std::uint16_t y = 0;
        std::uint16_t cb = 512;
        std::uint16_t cr = 512;
    };
    std::vector<Cell> grid(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
    auto const alphaOf = [&](double strength) { return static_cast<std::uint16_t>(std::lround(1023.0 * strength * level / 100.0)); };
    auto const put = [&](int x, int y, std::uint16_t alpha, int luma, int cb = 512, int cr = 512) {
        if (x < 0 || y < 0 || x >= side || y >= side)
        {
            return;
        }
        auto& cell = grid[static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x)];
        if (alpha > cell.alpha)
        {
            cell = {alpha, static_cast<std::uint16_t>(luma), static_cast<std::uint16_t>(cb), static_cast<std::uint16_t>(cr)};
        }
    };
    // A line of the graticule width from (x0, y0) along the unit vector (dx, dy), `length` pixels.
    auto const line = [&](double x0, double y0, double dx, double dy, double length, std::uint16_t alpha) {
        for (double t = 0; t <= length; t += 0.5)
        {
            int const x = static_cast<int>(std::floor(x0 + t * dx - (width - 1) / 2.0));
            int const y = static_cast<int>(std::floor(y0 + t * dy - (width - 1) / 2.0));
            for (int j = 0; j < width; ++j)
            {
                for (int i = 0; i < width; ++i)
                {
                    put(x + i, y + j, alpha, lineLuma);
                }
            }
        }
    };
    if (level > 0)
    {
        // The hue ring and the rings (every 44.8 codes: 10 % of the 448 codes of 350 mV).
        double const ringStep = 44.8 * perCode;
        auto const ringAlpha = alphaOf(0.5);
        auto const hueAlpha = alphaOf(1.0);
        for (int y = 0; y < side; ++y)
        {
            for (int x = 0; x < side; ++x)
            {
                double const dx = x + 0.5 - cx;
                double const dy = cy - (y + 0.5);
                double const d = std::sqrt(dx * dx + dy * dy);
                if (d >= inner && d <= outer + 0.5)
                {
                    auto const hue = hueColour(dx / std::max(d, 1e-9), dy / std::max(d, 1e-9));
                    put(x, y, hueAlpha, hue.y, hue.cb, hue.cr);
                    continue;
                }
                double const n = std::round(d / ringStep);
                if (n >= 1 && d < inner && std::fabs(d - n * ringStep) <= width / 2.0)
                {
                    put(x, y, ringAlpha, lineLuma);
                }
            }
        }
        // The Cb and Cr axes.
        for (auto [dx, dy] : {std::pair{1.0, 0.0}, std::pair{-1.0, 0.0}, std::pair{0.0, 1.0}, std::pair{0.0, -1.0}})
        {
            line(cx, cy, dx, dy, inner, ringAlpha);
        }
        // The skin-tone line at 123° (from +Cb towards +Cr).
        double const skin = 123.0 * 3.14159265358979323846 / 180.0;
        line(cx, cy, std::cos(skin), -std::sin(skin), inner, alphaOf(0.9));
        // The six colours: a vector to the hue ring, boxes at 75 % and 100 % (BT.709), the name.
        struct Target
        {
            char const* name;
            double r, g, b;
        };
        Target const targets[] = {{"R", 1, 0, 0}, {"Mg", 1, 0, 1}, {"B", 0, 0, 1}, {"Cy", 0, 1, 1}, {"G", 0, 1, 0}, {"Yl", 1, 1, 0}};
        int const box = std::max(3, static_cast<int>(std::lround(side / 100.0)));
        auto const boxAlpha = alphaOf(1.0);
        for (auto const& t : targets)
        {
            auto const full = rgbToYcbcr709(t.r, t.g, t.b);
            double const vx = full.cb - 512;
            double const vy = -(full.cr - 512);
            double const norm = std::sqrt(vx * vx + vy * vy);
            line(cx, cy, vx / norm, vy / norm, inner, ringAlpha);
            for (double amount : {0.75, 1.0})
            {
                auto const c = rgbToYcbcr709(amount * t.r, amount * t.g, amount * t.b);
                double bx = 0;
                double by = 0;
                if (!position(c.cb, false, bx) || !position(c.cr, true, by))
                {
                    continue;
                }
                int const x0 = static_cast<int>(std::floor(bx)) - box;
                int const y0 = static_cast<int>(std::floor(by)) - box;
                for (int k = 0; k <= 2 * box; ++k)
                {
                    for (int w = 0; w < width; ++w)
                    {
                        put(x0 + k, y0 + w, boxAlpha, lineLuma);
                        put(x0 + k, y0 + 2 * box - w, boxAlpha, lineLuma);
                        put(x0 + w, y0 + k, boxAlpha, lineLuma);
                        put(x0 + 2 * box - w, y0 + k, boxAlpha, lineLuma);
                    }
                }
            }
            // The name just inside the hue ring, on the colour's vector (its box clear of the
            // ring): rasterised, then graticule pixels of its coverage.
            int const tw = textWidth(t.name, s);
            double const radius = inner - 3.0 * s - std::fabs(vx / norm) * tw / 2.0 - std::fabs(vy / norm) * 4.0 * s;
            int const tx = static_cast<int>(std::lround(cx + radius * vx / norm - tw / 2.0));
            int const ty = static_cast<int>(std::lround(cy + radius * vy / norm - 4.0 * s));
            Frame422 text;
            text.allocate(even(tw + 2) + 2, 10 * s + 2);
            text.fill(0, kBackgroundC, kBackgroundC);
            drawText(text, 1, 1, s, t.name, 1023);
            for (int y = 0; y < text.height; ++y)
            {
                for (int x = 0; x < text.width; ++x)
                {
                    int const cover = text.y[static_cast<std::size_t>(y) * static_cast<std::size_t>(text.width) + static_cast<std::size_t>(x)];
                    if (cover > 0)
                    {
                        put(tx + x - 1, ty + y - 1, static_cast<std::uint16_t>((cover * level + 50) / 100), lineLuma);
                    }
                }
            }
        }
        for (int y = 0; y < side; ++y)
        {
            for (int x = 0; x < side; ++x)
            {
                auto const& cell = grid[static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x)];
                if (cell.alpha > 0)
                {
                    plan.plotGraticule.push_back({static_cast<std::uint16_t>(layout.plotX + x), static_cast<std::uint16_t>(layout.plotY + y), cell.alpha, cell.y, cell.cb, cell.cr});
                }
            }
        }
    }
    // The zoom, right of the plot's top corner, like the caption (§5.3).
#ifdef SCOPES_WITH_BLEND2D
    std::string const times = "×";
#else
    std::string const times = "x";
#endif
    drawText(plan.base, layout.plotX + layout.plotW + 3 * s, layout.plotY, s, std::to_string(zoom) + times, 700);
}
} // namespace

int panelCount(std::string const& mode)
{
    return mode == "ycbcr-parade" || mode == "rgb-parade" ? 3 : 1;
}

Layout makeModeLayout(int width, int height, std::string const& mode)
{
    return makeLayout(width, height, panelCount(mode), mode == "vectorscope");
}

Layout makeLayout(int width, int height, int panels, bool square)
{
    Layout layout;
    layout.width = width;
    layout.height = height;
    layout.textScale = std::max(1, static_cast<int>(std::lround(height / 540.0)));
    int const text = 8 * layout.textScale;
    int const top = std::max(static_cast<int>(std::lround(height * 0.04)), text + 6);
    int const bottom = std::max(static_cast<int>(std::lround(height * 0.04)), text / 2 + 2);
    // The plot starts and ends on v210 group boundaries (6 pixels), so the renderer packs whole
    // groups of the plot and keeps the still picture's bytes where a group has no trace.
    layout.plotX = 6 * static_cast<int>(std::lround(width * 0.08 / 6.0));
    int const right = even(std::max(2, static_cast<int>(std::lround(width * 0.02))));
    layout.plotY = top;
    layout.plotW = ((width - layout.plotX - right) / 6) * 6;
    layout.plotH = height - top - bottom;
    layout.captionX = layout.plotX;
    if (square)
    {
        // §5.3: the largest square of whole v210 groups that fits, in the middle of the raster.
        int const side = std::max(6, (std::min(layout.plotH, layout.plotW) / 6) * 6);
        layout.square = true;
        layout.plotX = 6 * ((width - side) / 12);
        layout.plotY = top + (layout.plotH - side) / 2;
        layout.plotW = side;
        layout.plotH = side;
    }
    layout.panelX[0] = layout.plotX;
    layout.panelW = layout.plotW;
    if (panels == 3)
    {
        // Three equal panels, 4 % gaps (the YCbCr parade's chroma labels sit in them).
        int const gap = std::max(6, 6 * static_cast<int>(std::lround(width * 0.04 / 6.0)));
        layout.panels = 3;
        layout.panelW = std::max(6, ((layout.plotW - 2 * gap) / 3 / 6) * 6);
        for (int k = 0; k < 3; ++k)
        {
            layout.panelX[static_cast<std::size_t>(k)] = layout.plotX + k * (layout.panelW + gap);
        }
        layout.plotW = 3 * layout.panelW + 2 * gap;
    }
    return layout;
}

int histogramColumns(Layout const& layout, int inputWidth)
{
    if (layout.square)
    {
        // The vectorscope: one cell per 2×2 pixels, so a flat colour is a dot, not one pixel.
        return std::min(layout.panelW / 2, 1024);
    }
    return inputWidth <= 0 ? layout.panelW : std::min(layout.panelW, inputWidth);
}

int histogramBins(Layout const& layout)
{
    return layout.square ? histogramColumns(layout, 0) : std::min(layout.plotH, 1024);
}

std::vector<int> traceRowSplit(RenderPlan const& plan, int parts)
{
    // Even shares of the plot rows, each boundary moved down to the next row that starts a bin:
    // a bin's cells are cleared after its last row, so no two parts may share a bin.
    int const rows = plan.layout.plotH;
    std::vector<int> split{0};
    for (int k = 1; k < parts; ++k)
    {
        int r = std::max(split.back(), static_cast<int>(static_cast<std::int64_t>(rows) * k / parts));
        while (r > 0 && r < rows && plan.binOfRow[static_cast<std::size_t>(r)] == plan.binOfRow[static_cast<std::size_t>(r) - 1])
        {
            ++r;
        }
        split.push_back(std::min(r, rows));
    }
    split.push_back(rows);
    return split;
}

int rowOfCode(RenderPlan const& plan, int code)
{
    int const bin = (std::clamp(code, 0, 1023) * plan.bins) >> 10;
    int first = -1;
    int last = -1;
    for (int r = 0; r < static_cast<int>(plan.binOfRow.size()); ++r)
    {
        if (plan.binOfRow[static_cast<std::size_t>(r)] == bin)
        {
            first = first < 0 ? r : first;
            last = r;
        }
    }
    if (first < 0)
    {
        return plan.layout.plotY + plan.layout.plotH - 1;
    }
    return plan.layout.plotY + (first + last) / 2;
}

std::vector<std::uint16_t> intensityTable(DisplaySettings const& settings, double cRef, std::uint32_t maxCount)
{
    std::vector<std::uint16_t> table{0};
    double const g = settings.intensity / 100.0 * kIntensityGain;
    cRef = std::max(cRef, 1e-9);
    for (std::uint32_t c = 1; c <= std::max<std::uint32_t>(1, maxCount); ++c)
    {
        auto const value = static_cast<int>(std::lround(1023.0 * (1.0 - std::exp(-g * static_cast<double>(c) / cRef))));
        auto const level = static_cast<std::uint16_t>(std::clamp(std::max(value, settings.traceMin), 1, 1023));
        table.push_back(level);
        if (level >= 1023)
        {
            break;
        }
    }
    return table;
}

void setIntensity(RenderPlan& plan, DisplaySettings const& settings, PlaneCounts const& counts)
{
    // §4.4: c_ref is the count of a cell that holds all samples of its column (a flat field).
    int const columns = plan.columns;
    for (int p = 0; p < 3; ++p)
    {
        auto const i = static_cast<std::size_t>(p);
        double const cRef = columns > 0 && counts.samples[i] > 0 ? static_cast<double>(counts.samples[i]) / columns : 1.0;
        plan.planeIntensity[i] = intensityTable(settings, cRef, counts.maxCount[i]);
        plan.planeMax[i] = static_cast<std::uint32_t>(plan.planeIntensity[i].size() - 1);
    }
    plan.intensity = plan.planeIntensity[0];
    plan.maxCount = plan.planeMax[0];
    if (plan.overlay)
    {
        // §4.4: the planes' traces add in R′G′B′ (rgb-overlay: pure primaries, so a neutral trace
        // is white); BT.709 to Y′CbCr per plane colour, × 64 for the integer sum.
        for (int c = 0; c < 3; ++c)
        {
            auto const& k = plan.overlayYcbcr[static_cast<std::size_t>(c)];
            auto const& lut = plan.planeIntensity[static_cast<std::size_t>(c)];
            auto& table = plan.overlayColour[static_cast<std::size_t>(c)];
            table.resize(lut.size());
            for (std::size_t n = 0; n < lut.size(); ++n)
            {
                double const v = lut[n] / 1023.0 * 64.0;
                table[n] = {static_cast<std::int32_t>(std::lround(k[0] * 876.0 * v)), static_cast<std::int32_t>(std::lround(k[1] * 896.0 * v)),
                    static_cast<std::int32_t>(std::lround(k[2] * 896.0 * v))};
            }
        }
    }
}

RenderPlan makeRenderPlan(Layout const& layout, DisplaySettings const& settings, int columns, std::uint64_t samples, std::uint32_t maxCount)
{
    PlaneCounts counts;
    counts.samples = {samples, samples, samples};
    counts.maxCount = {maxCount, maxCount, maxCount};
    return makeRenderPlan(layout, settings, columns, counts);
}

RenderPlan makeRenderPlan(Layout const& layout, DisplaySettings const& settings, int columns, PlaneCounts const& counts, bool key, int sumShift)
{
    RenderPlan plan;
    plan.layout = layout;
    plan.key = key;
    plan.bgAlpha = static_cast<std::uint16_t>(std::clamp(settings.bgAlpha, 0, 1023));
    plan.labelAlpha = static_cast<std::uint16_t>(std::max<int>(plan.bgAlpha, std::lround(1023.0 * settings.graticuleLevel / 100.0)));
    plan.fillBytes = v210RowBytes(layout.width) * static_cast<std::size_t>(layout.height);
    plan.columns = columns;
    plan.bins = histogramBins(layout);
    plan.planes = planesOf(scopeKindOf(settings.mode));
    auto const look = modeLook(settings);
    plan.overlay = look.overlay;
    for (std::size_t c = 0; c < 3; ++c)
    {
        auto const& rgb = look.overlayRgb[c];
        if (rgb == std::array<double, 3>{1, 0, 0})
        {
            plan.overlayYcbcr[c] = {0.2126, -0.2126 / 1.8556, 0.5};
        }
        else if (rgb == std::array<double, 3>{0, 1, 0})
        {
            plan.overlayYcbcr[c] = {0.7152, -0.7152 / 1.8556, -0.7152 / 1.5748};
        }
        else if (rgb == std::array<double, 3>{0, 0, 1})
        {
            plan.overlayYcbcr[c] = {0.0722, 0.5, -0.0722 / 1.5748};
        }
        else
        {
            double const y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
            plan.overlayYcbcr[c] = {y, (rgb[2] - y) / 1.8556, (rgb[0] - y) / 1.5748};
        }
    }
    plan.columnOf.resize(static_cast<std::size_t>(layout.panelW));
    for (int px = 0; px < layout.panelW; ++px)
    {
        plan.columnOf[static_cast<std::size_t>(px)] = static_cast<std::uint32_t>((static_cast<std::int64_t>(px) * columns) / layout.panelW);
    }
    plan.identityColumns = columns == layout.panelW;
    plan.binOfRow.resize(static_cast<std::size_t>(layout.plotH));
    for (int r = 0; r < layout.plotH; ++r)
    {
        plan.binOfRow[static_cast<std::size_t>(r)] =
            static_cast<std::uint16_t>((static_cast<std::int64_t>(layout.plotH - 1 - r) * plan.bins) / layout.plotH);
    }
    plan.vector = settings.mode == "vectorscope";
    plan.zoom = plan.vector ? std::max(1, settings.vectorZoom) : 1;
    plan.picture = settings.traceColor == "picture" && (plan.vector || settings.mode == "luma");
    if (plan.picture)
    {
        plan.sumShift = sumShift;
        plan.pictureMean.assign(4096, 0.0f);
        for (std::size_t c = 1; c < plan.pictureMean.size(); ++c)
        {
            plan.pictureMean[c] = static_cast<float>(1u << sumShift) / static_cast<float>(c);
        }
        plan.pictureGain.assign(2048, 0.0f);
        for (std::size_t t = 1; t < plan.pictureGain.size(); ++t)
        {
            plan.pictureGain[t] = (876.0f / 1023.0f) / static_cast<float>(t);
        }
        if (plan.vector)
        {
            // The chroma at the centre of a cell: its code range at the zoom (§5.3), − 512.
            auto const centre = [&](int cell, int cells) {
                double const q = (cell + 0.5) * kVectorCodes / cells;
                return static_cast<std::int16_t>(std::lround((q - kVectorCodes / 2.0) / plan.zoom));
            };
            for (int px = 0; px < layout.panelW; ++px)
            {
                plan.pictureCb.push_back(centre(static_cast<int>(plan.columnOf[static_cast<std::size_t>(px)]), columns));
            }
            for (int r = 0; r < layout.plotH; ++r)
            {
                plan.pictureCr.push_back(centre(plan.binOfRow[static_cast<std::size_t>(r)], plan.bins));
            }
        }
        else
        {
            for (int r = 0; r < layout.plotH; ++r)
            {
                plan.pictureY.push_back(static_cast<std::uint16_t>(binCentreCode(plan.binOfRow[static_cast<std::size_t>(r)], plan.bins)));
            }
        }
    }
    setIntensity(plan, settings, counts);
    for (auto const& trace : look.colours)
    {
        TraceColour colour;
        for (int i = 0; i < 1024; ++i)
        {
            colour.y[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(kBackgroundY + scale1023(trace.y - kBackgroundY, i));
            colour.cb[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(kBackgroundC + scale1023(trace.cb - kBackgroundC, i));
            colour.cr[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(kBackgroundC + scale1023(trace.cr - kBackgroundC, i));
        }
        plan.colours.push_back(colour);
    }
    plan.yOf = plan.colours[0].y;
    plan.cbOf = plan.colours[0].cb;
    plan.crOf = plan.colours[0].cr;
    for (int k = 0; k < layout.panels; ++k)
    {
        auto const i = static_cast<std::size_t>(k);
        plan.panels.push_back({layout.panelX[i], i < look.planes.size() ? look.planes[i] : 0, i < look.colours.size() ? static_cast<int>(i) : 0});
    }

    // The graticule (§4.5): lines in each panel, labels left of it (the left margin for the first
    // panel, the gap for a chroma panel).
    int const level = settings.graticuleLevel;
    int const width = settings.graticuleWidth;
    int const lineLuma = 64 + static_cast<int>(std::lround(876 * 0.8));
    plan.base.allocate(layout.width, layout.height);
    plan.base.fill(kBackgroundY, kBackgroundC, kBackgroundC);
    if (key)
    {
        plan.base.a.assign(plan.base.y.size(), plan.bgAlpha);
    }
    int const s = layout.textScale;
    auto const labelLuma = static_cast<std::uint16_t>(kBackgroundY + scale1023(lineLuma - kBackgroundY, level * 1023 / 100));
    if (plan.vector)
    {
        vectorGraticule(plan, settings, lineLuma);
    }
    else if (level > 0)
    {
        for (int k = 0; k < layout.panels; ++k)
        {
            auto const i = static_cast<std::size_t>(k);
            bool const chroma = i < look.chroma.size() && look.chroma[i];
            auto lines = chroma ? chromaLines(settings) : lumaLines(settings);
            bool const labelled = k == 0 || chroma;
            int const x0 = layout.panelX[i];
            for (auto& line : lines)
            {
                line.row = rowOfCode(plan, line.code);
                double const strength = line.style == "solid" ? 1.0 : line.style == "dotted" ? 0.9 : 0.5;
                auto const alpha = static_cast<std::uint16_t>(std::lround(1023.0 * strength * level / 100.0));
                // SCOPES_GRATICULE_WIDTH rows around the line's row (1: the row only), inside the plot.
                int const first = std::max(layout.plotY, line.row - (width - 1) / 2);
                int const last = std::min(layout.plotY + layout.plotH - 1, line.row + width / 2);
                for (int row = first; row <= last; ++row)
                {
                    for (int x = 0; x < layout.panelW; ++x)
                    {
                        if (line.style == "dotted" && ((x / (2 * s)) % 2) != 0)
                        {
                            continue;
                        }
                        plan.plotGraticule.push_back({static_cast<std::uint16_t>(x0 + x), static_cast<std::uint16_t>(row), alpha, static_cast<std::uint16_t>(lineLuma)});
                    }
                }
                if (labelled && !line.label.empty())
                {
                    int const w = textWidth(line.label, s);
                    drawText(plan.base, x0 - 3 * s - w, line.row - 4 * s, s, line.label, labelLuma, plan.labelAlpha);
                }
            }
            if (k == 0)
            {
                plan.lines = lines;
            }
        }
        std::sort(plan.plotGraticule.begin(), plan.plotGraticule.end(),
            [](GraticulePixel const& a, GraticulePixel const& b) { return a.row != b.row ? a.row < b.row : a.x < b.x; });
        if (width > 1)
        {
            // Wide lines close together overlap: one pixel each, the brightest line wins.
            std::vector<GraticulePixel> merged;
            merged.reserve(plan.plotGraticule.size());
            for (auto const& p : plan.plotGraticule)
            {
                if (!merged.empty() && merged.back().row == p.row && merged.back().x == p.x)
                {
                    if (p.alpha > merged.back().alpha)
                    {
                        merged.back() = p;
                    }
                    continue;
                }
                merged.push_back(p);
            }
            plan.plotGraticule = std::move(merged);
        }
        // Units in the top-left corner of the margin.
        std::string const units = settings.graticuleUnits == "mv" ? "mV" : settings.graticuleUnits == "percent" ? "%" : "code";
        int const bandBottom = std::max(0, layout.plotY - 4 * s);
        drawText(plan.base, std::max(0, layout.captionX - 3 * s - textWidth(units, s)), std::max(0, (bandBottom - 8 * s) / 2), s, units, labelLuma, plan.labelAlpha);
    }
    // Panel names under the parade panels, when the bottom margin has room.
    int const bottom = layout.height - layout.plotY - layout.plotH;
    if (layout.panels > 1 && bottom >= 8 * s + 2)
    {
        for (int k = 0; k < layout.panels && static_cast<std::size_t>(k) < look.names.size(); ++k)
        {
            auto const& name = look.names[static_cast<std::size_t>(k)];
            auto const& c = look.colours[static_cast<std::size_t>(k)];
            int const x = layout.panelX[static_cast<std::size_t>(k)] + (layout.panelW - textWidth(name, s)) / 2;
            drawText(plan.base, x, layout.plotY + layout.plotH + (bottom - 8 * s) / 2, s, name, static_cast<std::uint16_t>(std::max(c.y, 400)));
        }
    }
    plan.graticuleStart.assign(static_cast<std::size_t>(layout.plotH) + 1, 0);
    std::size_t next = 0;
    for (int r = 0; r <= layout.plotH; ++r)
    {
        while (next < plan.plotGraticule.size() && plan.plotGraticule[next].row < layout.plotY + r)
        {
            ++next;
        }
        plan.graticuleStart[static_cast<std::size_t>(r)] = static_cast<std::uint32_t>(next);
    }
    return plan;
}

namespace
{
// The graticule pixels of one plot row over the row's samples (x relative to the row start).
void blendGraticuleRow(RenderPlan const& plan, int r, std::uint16_t* ys, std::uint16_t* cbs, std::uint16_t* crs, std::uint16_t* as)
{
    auto const begin = plan.graticuleStart[static_cast<std::size_t>(r)];
    auto const end = plan.graticuleStart[static_cast<std::size_t>(r) + 1];
    for (auto i = begin; i < end; ++i)
    {
        auto const& g = plan.plotGraticule[i];
        ys[g.x] = blend(ys[g.x], g.luma, g.alpha);
        if (as != nullptr)
        {
            as[g.x] = std::max(as[g.x], g.alpha);
        }
        if ((g.x & 1) == 0)
        {
            cbs[g.x / 2] = blend(cbs[g.x / 2], g.cb, g.alpha);
            crs[g.x / 2] = blend(crs[g.x / 2], g.cr, g.alpha);
        }
    }
}
} // namespace

void renderEmptyPlot(RenderPlan const& plan, Frame422& frame)
{
    auto const& layout = plan.layout;
    int const cw = frame.chromaWidth();
    for (int r = 0; r < layout.plotH; ++r)
    {
        int const row = layout.plotY + r;
        auto* ys = frame.y.data() + static_cast<std::size_t>(row) * frame.width;
        auto* cbs = frame.cb.data() + static_cast<std::size_t>(row) * cw;
        auto* crs = frame.cr.data() + static_cast<std::size_t>(row) * cw;
        auto* as = frame.a.empty() ? nullptr : frame.a.data() + static_cast<std::size_t>(row) * frame.width;
        for (int k = 0; k < layout.panels; ++k)
        {
            int const x0 = layout.panelX[static_cast<std::size_t>(k)];
            std::fill(ys + x0, ys + x0 + layout.panelW, kBackgroundY);
            std::fill(cbs + x0 / 2, cbs + (x0 + layout.panelW) / 2, kBackgroundC);
            std::fill(crs + x0 / 2, crs + (x0 + layout.panelW) / 2, kBackgroundC);
            if (as != nullptr)
            {
                std::fill(as + x0, as + x0 + layout.panelW, plan.bgAlpha);
            }
        }
        blendGraticuleRow(plan, r, ys, cbs, crs, as);
    }
}

namespace
{
inline void put32(std::uint8_t* dst, std::uint32_t word)
{
    if constexpr (std::endian::native == std::endian::little)
    {
        std::memcpy(dst, &word, sizeof(word));
    }
    else
    {
        for (int i = 0; i < 4; ++i)
        {
            dst[i] = static_cast<std::uint8_t>((word >> (8 * i)) & 0xff);
        }
    }
}

inline std::uint32_t word3(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    return a | (b << 10) | (c << 20);
}
} // namespace

namespace
{
// Half of a signed sum rounded up (the chroma of a pixel pair); no branch.
int half(int v)
{
    return (v + 1) >> 1;
}

// The picture colour of the six cells of a group (§4.4): each cell's luma lifted to
// kPictureLumaFloor (adding grey keeps the hue), then brightened to the brightest colour of that
// hue and saturation (the largest R′G′B′ component at 100 %), then scaled by its intensity
// `level` like the other trace colours; an empty cell (level 0) comes out as background. Out: Y′
// and the chroma offsets from 512. `counts` and `sums` are the cells', `px` the first one's pixel
// column in the panel, `r` the plot row. Single-precision IEEE maths in baseline code (no FMA),
// so every engine path draws the same bytes; stage by stage over the six cells, so their chains
// overlap.
template <class C>
void pictureColours(RenderPlan const& plan, C const* counts, std::uint64_t const* sums, int px, int r, std::uint16_t const* level, std::uint16_t* y, int* cb, int* cr)
{
    // The mean code of a sum: sums of code >> sumShift, so × 2^sumShift and half a step back.
    float const bias = static_cast<float>((1 << plan.sumShift) - 1) / 2.0f + 0.5f;
    auto const* meanOf = plan.pictureMean.data();
    auto const means = static_cast<std::uint32_t>(plan.pictureMean.size());
    float scale[6];
    for (int i = 0; i < 6; ++i)
    {
        auto const count = std::max<std::uint32_t>(1, counts[i]);
        scale[i] = count < means ? meanOf[count] : static_cast<float>(1u << plan.sumShift) / static_cast<float>(count);
    }
    auto const mean = [&](int i, int half) { return static_cast<int>(static_cast<float>(static_cast<std::uint32_t>(sums[i] >> (32 * half))) * scale[i] + bias); };
    // In codes: Y′ − 64 of 876, chroma of 896 (the BT.709 matrix scaled by 876 / 896).
    float yy[6];
    float pb[6];
    float pr[6];
    for (int i = 0; i < 6; ++i)
    {
        int const luma = plan.vector ? mean(i, 0) : plan.pictureY[static_cast<std::size_t>(r)];
        yy[i] = static_cast<float>(std::clamp(luma, kPictureLumaFloor, 940) - 64);
        pb[i] = static_cast<float>(plan.vector ? plan.pictureCb[static_cast<std::size_t>(px + i)] : mean(i, 0) - 512);
        pr[i] = static_cast<float>(plan.vector ? plan.pictureCr[static_cast<std::size_t>(r)] : mean(i, 1) - 512);
    }
    float gain[6];
    auto const last = plan.pictureGain.size() - 1;
    for (int i = 0; i < 6; ++i)
    {
        // The largest of R′, G′, B′ (BT.709); at least yy, as Y′ is their weighted mean.
        float const top = yy[i] + std::max(std::max(1.5397f * pr[i], -0.183143f * pb[i] - 0.457675f * pr[i]), 1.81418f * pb[i]);
        gain[i] = static_cast<float>(level[i]) * plan.pictureGain[std::min(static_cast<std::size_t>(top + 0.5f), last)];
    }
    // Rounded to the nearest code (halves up) without a branch: every value is above −1024.
    auto const round = [](float v) { return static_cast<int>(v + 1024.5f) - 1024; };
    for (int i = 0; i < 6; ++i)
    {
        y[i] = static_cast<std::uint16_t>(kBackgroundY + round(yy[i] * gain[i]));
        cb[i] = round(pb[i] * gain[i]);
        cr[i] = round(pr[i] * gain[i]);
    }
}

// The cells of one panel row -> its trace groups (graticule behind the trace). Groups without
// counts keep the still picture's bytes. `lines` holds the plane rows of the panel: one, or
// three for the overlay. PICTURE: the picture colour, `sumLine` the row's sum words (one per
// pixel), `r` the plot row; with `clearSums` the words of each group with a trace are cleared
// here (the row is the bin's only one), so the caller need not clear the whole row. Returns true when the row got a trace group.
template <bool OVERLAY, class C, bool PICTURE = false>
bool panelRow(RenderPlan const& plan, PanelPlan const& panel, C const* const* lines, std::uint32_t& gi, std::uint32_t gend, std::size_t rowOffset,
    std::vector<TraceGroup>& trace, std::uint64_t* sumLine = nullptr, int r = 0, bool clearSums = false)
{
    auto const& layout = plan.layout;
    int const groups = layout.panelW / 6;
    auto const traceBefore = trace.size();
    bool lastEmpty = true;
    auto const& colour = plan.colours[static_cast<std::size_t>(panel.colour)];
    // The cells of eight groups (48) of every plane of the panel are zero.
    auto const eightEmpty = [&](int g) {
        std::uint64_t any = 0;
        for (int p = 0; p < (OVERLAY ? 3 : 1); ++p)
        {
            std::uint64_t words[6 * sizeof(C)];
            std::memcpy(words, lines[p] + 6 * g, sizeof(words));
            for (auto const word : words)
            {
                any |= word;
            }
        }
        return any == 0;
    };
    // The six cells of group g of plane p are zero.
    auto const groupEmpty = [&](int p, int g) {
        if constexpr (sizeof(C) == 2)
        {
            std::uint64_t head = 0;
            std::uint32_t tail = 0;
            std::memcpy(&head, lines[p] + 6 * g, sizeof(head));
            std::memcpy(&tail, lines[p] + 6 * g + 4, sizeof(tail));
            return (head | tail) == 0;
        }
        else
        {
            std::uint64_t words[3];
            std::memcpy(words, lines[p] + 6 * g, sizeof(words));
            return (words[0] | words[1] | words[2]) == 0;
        }
    };
    for (int g = 0; g < groups; ++g)
    {
        // In an empty stretch, skip eight empty groups (48 cells, 96 bytes) at a time; inside
        // a trace (a noisy picture) the check would only cost.
        while (lastEmpty && g + 8 <= groups && eightEmpty(g))
        {
            g += 8;
            while (gi < gend && plan.plotGraticule[gi].x < panel.x + 6 * g)
            {
                ++gi;
            }
        }
        if (g >= groups)
        {
            break;
        }
        int const x0 = panel.x + 6 * g;
        if constexpr (OVERLAY)
        {
            lastEmpty = groupEmpty(0, g) && groupEmpty(1, g) && groupEmpty(2, g);
        }
        else
        {
            lastEmpty = groupEmpty(0, g);
        }
        if (lastEmpty)
        {
            // No trace: the still bytes are right, graticule included.
            while (gi < gend && plan.plotGraticule[gi].x < x0 + 6)
            {
                ++gi;
            }
            continue;
        }
        std::uint16_t y[6];
        std::uint16_t cb[3];
        std::uint16_t cr[3];
        // Trace intensity per pixel (the brightest plane of the overlay): the trace's opacity
        // over the graticule behind it.
        std::uint16_t level[6] = {};
        for (int i = 0; i < 6; ++i)
        {
            for (int p = 0; p < (OVERLAY ? 3 : 1); ++p)
            {
                auto const plane = static_cast<std::size_t>(OVERLAY ? p : panel.plane);
                level[i] = std::max(level[i], plan.planeIntensity[plane][std::min<std::uint32_t>(lines[p][6 * g + i], plan.planeMax[plane])]);
            }
        }
        // The key (§7): max(background alpha, trace intensity), the graticule's alpha under it.
        std::uint16_t a[6] = {};
        if (plan.key)
        {
            for (int i = 0; i < 6; ++i)
            {
                a[i] = std::max(plan.bgAlpha, level[i]);
            }
        }
        if constexpr (OVERLAY)
        {
            // Sums of the three planes' contributions (× 64) per pixel; chroma from the pair.
            std::int32_t ys[6];
            std::int32_t cbs[6];
            std::int32_t crs[6];
            for (int i = 0; i < 6; ++i)
            {
                auto const& r = plan.overlayColour[0][std::min<std::uint32_t>(lines[0][6 * g + i], plan.planeMax[0])];
                auto const& gg = plan.overlayColour[1][std::min<std::uint32_t>(lines[1][6 * g + i], plan.planeMax[1])];
                auto const& b = plan.overlayColour[2][std::min<std::uint32_t>(lines[2][6 * g + i], plan.planeMax[2])];
                ys[i] = r.y + gg.y + b.y;
                cbs[i] = r.cb + gg.cb + b.cb;
                crs[i] = r.cr + gg.cr + b.cr;
            }
            for (int i = 0; i < 6; ++i)
            {
                y[i] = static_cast<std::uint16_t>(std::clamp(kBackgroundY + ((ys[i] + 32) >> 6), 4, 1019));
            }
            for (int k = 0; k < 3; ++k)
            {
                cb[k] = static_cast<std::uint16_t>(std::clamp(kBackgroundC + ((cbs[2 * k] + cbs[2 * k + 1] + 64) >> 7), 4, 1019));
                cr[k] = static_cast<std::uint16_t>(std::clamp(kBackgroundC + ((crs[2 * k] + crs[2 * k + 1] + 64) >> 7), 4, 1019));
            }
        }
        else if constexpr (PICTURE)
        {
            int cbs[6];
            int crs[6];
            // No branch on empty cells: their intensity is 0, so they come out as background.
            pictureColours(plan, lines[0] + 6 * g, sumLine + 6 * g, 6 * g, r, level, y, cbs, crs);
            if (clearSums)
            {
                std::memset(sumLine + 6 * g, 0, 6 * sizeof(std::uint64_t));
            }
            for (int k = 0; k < 3; ++k)
            {
                cb[k] = static_cast<std::uint16_t>(std::clamp(kBackgroundC + half(cbs[2 * k] + cbs[2 * k + 1]), 4, 1019));
                cr[k] = static_cast<std::uint16_t>(std::clamp(kBackgroundC + half(crs[2 * k] + crs[2 * k + 1]), 4, 1019));
            }
        }
        else
        {
            for (int p = 0; p < 6; p += 2)
            {
                // intensity[0] is 0: no branch on empty cells.
                int const i0 = level[p];
                int const i1 = level[p + 1];
                y[p] = colour.y[static_cast<std::size_t>(i0)];
                y[p + 1] = colour.y[static_cast<std::size_t>(i1)];
                auto const mean = static_cast<std::size_t>((i0 + i1 + 1) >> 1);
                cb[p / 2] = colour.cb[mean];
                cr[p / 2] = colour.cr[mean];
            }
        }
        // The graticule behind the trace: the trace covers it by its intensity, so a trace on a
        // line (a black picture on the 0 % line) stays visible. The graticule is neutral like
        // the background (only the luma changes), except the vectorscope's hue ring, whose
        // chroma the pixel pair's trace covers by its mean intensity.
        for (; gi < gend && plan.plotGraticule[gi].x < x0 + 6; ++gi)
        {
            auto const& gp = plan.plotGraticule[gi];
            int const at = gp.x - x0;
            int const under = blend(kBackgroundY, gp.luma, gp.alpha) - kBackgroundY;
            y[at] = static_cast<std::uint16_t>(std::clamp(y[at] + scale1023(under, 1023 - level[at]), 4, 1019));
            a[at] = std::max(a[at], gp.alpha);
            if ((at & 1) == 0 && (gp.cb != kBackgroundC || gp.cr != kBackgroundC))
            {
                int const cover = 1023 - ((level[at] + level[at + 1] + 1) >> 1);
                int const k = at / 2;
                cb[k] = static_cast<std::uint16_t>(std::clamp(cb[k] + scale1023(blend(kBackgroundC, gp.cb, gp.alpha) - kBackgroundC, cover), 4, 1019));
                cr[k] = static_cast<std::uint16_t>(std::clamp(cr[k] + scale1023(blend(kBackgroundC, gp.cr, gp.alpha) - kBackgroundC, cover), 4, 1019));
            }
        }
        TraceGroup group;
        group.offset = static_cast<std::uint32_t>(rowOffset + 16 * static_cast<std::size_t>(x0 / 6));
        put32(group.bytes, word3(cb[0], y[0], cr[0]));
        put32(group.bytes + 4, word3(y[1], cb[1], y[2]));
        put32(group.bytes + 8, word3(cr[1], y[3], cb[2]));
        put32(group.bytes + 12, word3(y[4], cr[2], y[5]));
        if (plan.key)
        {
            auto const row = rowOffset / v210RowBytes(layout.width);
            group.keyOffset = static_cast<std::uint32_t>(plan.fillBytes + row * alphaRowBytes(layout.width) + 4 * static_cast<std::size_t>(x0 / 3));
            put32(group.key, word3(a[0], a[1], a[2]));
            put32(group.key + 4, word3(a[3], a[4], a[5]));
        }
        trace.push_back(group);
    }
    return trace.size() != traceBefore;
}
} // namespace

namespace
{
// The samples of one histogram row. 16-bit cells add in 32-bit lanes (at most 3840 × 65535 fits),
// which the compiler vectorises; 32-bit cells in 64 bits.
template <class C>
std::uint64_t rowSum(C const* cells, int columns)
{
    using Acc = std::conditional_t<sizeof(C) == 2, std::uint32_t, std::uint64_t>;
    Acc sum = 0;
    for (int c = 0; c < columns; ++c)
    {
        sum += cells[c];
    }
    return sum;
}

template <class C>
void collectTraceCells(RenderPlan const& plan, Histogram& hist, std::vector<TraceGroup>& trace, int rowBegin, int rowEnd, BinTotals* totals)
{
    auto const& layout = plan.layout;
    auto const planeCells = static_cast<std::size_t>(plan.columns) * static_cast<std::size_t>(plan.bins);
    auto const rowBytes = v210RowBytes(layout.width);
    std::array<std::vector<C>, 3> sampled;
    if (!plan.identityColumns)
    {
        for (int p = 0; p < plan.planes; ++p)
        {
            sampled[static_cast<std::size_t>(p)].resize(static_cast<std::size_t>(layout.panelW) + 8);
        }
    }
    // The picture colour's sums: one word per cell, next to the counts of plane 0.
    bool const picture = plan.picture;
    std::vector<std::uint64_t> sampledSums(plan.identityColumns || !picture ? 0 : static_cast<std::size_t>(layout.panelW));
    // Every plot row shows its own bin and column (the usual case up to 1080 lines): a row whose
    // cells are all zero need not be cleared.
    bool const inlineClear = plan.identityColumns && plan.bins == layout.plotH;
    for (int r = rowBegin; r < rowEnd; ++r)
    {
        auto const row = static_cast<std::size_t>(layout.plotY + r);
        auto const bin = plan.binOfRow[static_cast<std::size_t>(r)];
        C* rows[3] = {nullptr, nullptr, nullptr};
        C const* lines[3] = {nullptr, nullptr, nullptr};
        for (int p = 0; p < plan.planes; ++p)
        {
            auto const i = static_cast<std::size_t>(p);
            rows[i] = hist.data<C>() + i * planeCells + static_cast<std::size_t>(bin) * static_cast<std::size_t>(plan.columns);
            lines[i] = rows[i];
            if (!plan.identityColumns)
            {
                for (int px = 0; px < layout.panelW; ++px)
                {
                    sampled[i][static_cast<std::size_t>(px)] = rows[i][plan.columnOf[static_cast<std::size_t>(px)]];
                }
                lines[i] = sampled[i].data();
            }
        }
        std::uint64_t* sumRow = nullptr;
        std::uint64_t* sumLine = nullptr;
        if (picture)
        {
            sumRow = hist.sums.data() + static_cast<std::size_t>(bin) * static_cast<std::size_t>(plan.columns);
            sumLine = sumRow;
            if (!plan.identityColumns)
            {
                for (int px = 0; px < layout.panelW; ++px)
                {
                    sampledSums[static_cast<std::size_t>(px)] = sumRow[plan.columnOf[static_cast<std::size_t>(px)]];
                }
                sumLine = sampledSums.data();
            }
        }
        if (totals != nullptr && (r == 0 || plan.binOfRow[static_cast<std::size_t>(r) - 1] != bin))
        {
            // The first plot row of this bin: its samples over all columns (§4.7).
            for (int p = 0; p < plan.planes; ++p)
            {
                totals->plane[static_cast<std::size_t>(p)][bin] = rowSum(rows[static_cast<std::size_t>(p)], plan.columns);
            }
        }
        auto gi = plan.graticuleStart[static_cast<std::size_t>(r)];
        auto const gend = plan.graticuleStart[static_cast<std::size_t>(r) + 1];
        bool had[3] = {false, false, false};
        for (auto const& panel : plan.panels)
        {
            while (gi < gend && plan.plotGraticule[gi].x < panel.x)
            {
                ++gi;
            }
            if (plan.overlay)
            {
                if (panelRow<true, C>(plan, panel, lines, gi, gend, rowBytes * row, trace))
                {
                    had[0] = had[1] = had[2] = true;
                }
            }
            else if (picture)
            {
                had[0] = panelRow<false, C, true>(plan, panel, lines, gi, gend, rowBytes * row, trace, sumLine, r, inlineClear) || had[0];
            }
            else if (panelRow<false, C>(plan, panel, lines + panel.plane, gi, gend, rowBytes * row, trace))
            {
                had[panel.plane] = true;
            }
        }
        // Clear the bin once the last plot row that shows it is drawn; a row without a trace has
        // only zeros and is not written.
        if (r + 1 == layout.plotH || plan.binOfRow[static_cast<std::size_t>(r) + 1] != bin)
        {
            for (int p = 0; p < plan.planes; ++p)
            {
                if (!inlineClear || had[p])
                {
                    std::memset(rows[p], 0, sizeof(C) * static_cast<std::size_t>(plan.columns));
                    if (p == 0 && sumRow != nullptr && !inlineClear)
                    {
                        std::memset(sumRow, 0, sizeof(std::uint64_t) * static_cast<std::size_t>(plan.columns));
                    }
                }
            }
        }
    }
    // The vectorscope's trash cell (samples outside the zoomed plot) after its last row.
    if (hist.extra > 0 && rowEnd == layout.plotH)
    {
        auto const trash = static_cast<std::size_t>(plan.planes) * planeCells;
        hist.data<C>()[trash] = 0;
        if (picture)
        {
            hist.sums[trash] = 0;
        }
    }
}
} // namespace

void collectTrace(RenderPlan const& plan, Histogram& hist, std::vector<TraceGroup>& trace, int rowBegin, int rowEnd, BinTotals* totals)
{
    trace.clear();
    rowEnd = rowEnd < 0 ? plan.layout.plotH : std::min(rowEnd, plan.layout.plotH);
    rowBegin = std::clamp(rowBegin, 0, rowEnd);
    auto const planeCells = static_cast<std::size_t>(plan.columns) * static_cast<std::size_t>(plan.bins);
    bool const hasCounts = hist.cells() > 0 && hist.columns == plan.columns && hist.bins == plan.bins && hist.planes == plan.planes &&
                           hist.cells() == planeCells * static_cast<std::size_t>(plan.planes) &&
                           hist.sums.empty() != plan.picture;
    if (!hasCounts)
    {
        return;
    }
    if (hist.wide)
    {
        collectTraceCells<std::uint32_t>(plan, hist, trace, rowBegin, rowEnd, totals);
    }
    else
    {
        collectTraceCells<std::uint16_t>(plan, hist, trace, rowBegin, rowEnd, totals);
    }
}

void applyTrace(std::vector<TraceGroup> const& trace, std::uint8_t* dst)
{
    for (auto const& group : trace)
    {
        std::memcpy(dst + group.offset, group.bytes, sizeof(group.bytes));
        if (group.keyOffset != 0)
        {
            std::memcpy(dst + group.keyOffset, group.key, sizeof(group.key));
        }
    }
}

void renderTexts(RenderPlan const& plan, Frame422& frame, std::string const& caption, std::string const& state)
{
    auto const& layout = plan.layout;
    int const s = layout.textScale;
    int const width = frame.width;
    int const cw = frame.chromaWidth();
    // The caption band: the top margin right of the label column.
    int const bandBottom = std::max(0, layout.plotY - 4 * s);
    int const x0 = layout.captionX;
    for (int row = 0; row < bandBottom; ++row)
    {
        if (!frame.a.empty())
        {
            std::fill(frame.a.begin() + static_cast<std::ptrdiff_t>(row) * width + x0, frame.a.begin() + static_cast<std::ptrdiff_t>(row + 1) * width,
                plan.bgAlpha);
        }
        std::fill(frame.y.begin() + static_cast<std::ptrdiff_t>(row) * width + x0, frame.y.begin() + static_cast<std::ptrdiff_t>(row + 1) * width,
            kBackgroundY);
        std::fill(frame.cb.begin() + static_cast<std::ptrdiff_t>(row) * cw + x0 / 2, frame.cb.begin() + static_cast<std::ptrdiff_t>(row + 1) * cw,
            kBackgroundC);
        std::fill(frame.cr.begin() + static_cast<std::ptrdiff_t>(row) * cw + x0 / 2, frame.cr.begin() + static_cast<std::ptrdiff_t>(row + 1) * cw,
            kBackgroundC);
    }
    if (!caption.empty())
    {
        drawText(frame, x0, std::max(0, (bandBottom - 8 * s) / 2), s, caption, 700);
    }
    if (!state.empty())
    {
        int const big = 2 * s;
        int const w = textWidth(state, big);
        int const h = 8 * big;
        int const x = layout.plotX + (layout.plotW - w) / 2;
        int const y = layout.plotY + (layout.plotH - h) / 2;
        int const pad = 2 * big;
        for (int row = std::max(0, y - pad); row < std::min(frame.height, y + h + pad); ++row)
        {
            for (int col = std::max(0, even(x - pad)); col < std::min(width, x + w + pad); ++col)
            {
                frame.y[static_cast<std::size_t>(row) * width + col] = kBackgroundY;
                if (!frame.a.empty())
                {
                    frame.a[static_cast<std::size_t>(row) * width + col] = plan.bgAlpha;
                }
                if ((col & 1) == 0)
                {
                    frame.cb[static_cast<std::size_t>(row) * cw + col / 2] = kBackgroundC;
                    frame.cr[static_cast<std::size_t>(row) * cw + col / 2] = kBackgroundC;
                }
            }
        }
        drawText(frame, x, y, big, state, 940);
    }
}

#ifdef SCOPES_WITH_BLEND2D
namespace
{
// Text with Blend2D and DejaVu Sans (§3.1), as mxl-multiviewer. The text is rasterised as coverage
// into an A8 image with the JIT off (Blend2D's portable pipelines), so the pixels are the same on
// every x86-64 CPU and the golden pictures hold, then blended into the picture.
BLFontFace const& dejavuFace()
{
    static BLFontFace const face = [] {
        BLFontData data;
        BLFontFace created;
        if (data.create_from_data(scopes::font::dejavuSans, scopes::font::dejavuSansSize) == BL_SUCCESS)
        {
            created.create_from_data(data, 0);
        }
        return created;
    }();
    return face;
}

// 16 px text at 1080 lines is the 8×8 font at scale 2; DejaVu at 10 px per scale has about its
// cap height.
float pixelSize(int scale)
{
    return 10.0f * static_cast<float>(scale);
}

bool makeFont(int scale, BLFont& font)
{
    auto const& face = dejavuFace();
    return face.is_valid() && font.create_from_face(face, pixelSize(scale)) == BL_SUCCESS;
}
} // namespace

int textWidth(std::string const& text, int scale)
{
    BLFont font;
    if (text.empty() || !makeFont(scale, font))
    {
        return 0;
    }
    BLGlyphBuffer glyphs;
    glyphs.set_utf8_text(text.c_str(), text.size());
    font.shape(glyphs);
    BLTextMetrics metrics;
    if (font.get_text_metrics(glyphs, metrics) != BL_SUCCESS)
    {
        return 0;
    }
    return static_cast<int>(std::lround(metrics.advance.x));
}

void drawText(Frame422& frame, int x, int y, int scale, std::string const& text, std::uint16_t luma, std::uint16_t alpha)
{
    BLFont font;
    int const w = textWidth(text, scale) + 2;
    int const h = 10 * scale + 2;
    if (text.empty() || w <= 2 || !makeFont(scale, font))
    {
        return;
    }
    BLImage image;
    if (image.create(w, h, BL_FORMAT_A8) != BL_SUCCESS)
    {
        return;
    }
    {
        BLContextCreateInfo info{};
        info.flags = BL_CONTEXT_CREATE_FLAG_DISABLE_JIT;
        info.thread_count = 0;
        BLContext ctx;
        if (ctx.begin(image, info) != BL_SUCCESS)
        {
            return;
        }
        ctx.clear_all();
        // The text's box is the 8×8 font's: the cap height from the top, the baseline at 7.5 of 8.
        ctx.fill_utf8_text(BLPoint(1.0, 1.0 + 7.5 * scale), font, text.c_str(), text.size(), BLRgba32(0xffffffffu));
        ctx.end();
    }
    BLImageData data;
    if (image.get_data(&data) != BL_SUCCESS || data.pixel_data == nullptr)
    {
        return;
    }
    int const width = frame.width;
    int const cw = frame.chromaWidth();
    for (int row = 0; row < h; ++row)
    {
        int const fy = y + row - 1;
        if (fy < 0 || fy >= frame.height)
        {
            continue;
        }
        auto const* cover = static_cast<std::uint8_t const*>(data.pixel_data) + static_cast<std::ptrdiff_t>(row) * data.stride;
        for (int col = 0; col < w; ++col)
        {
            int const fx = x + col - 1;
            int const c = cover[col];
            if (c == 0 || fx < 0 || fx >= width)
            {
                continue;
            }
            int const a10 = (c * 1023 + 127) / 255;
            auto const at = static_cast<std::size_t>(fy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(fx);
            frame.y[at] = blend(frame.y[at], luma, a10);
            if (!frame.a.empty())
            {
                frame.a[at] = std::max<std::uint16_t>(frame.a[at], static_cast<std::uint16_t>((alpha * c + 127) / 255));
            }
            if ((fx & 1) == 0)
            {
                auto const ca = static_cast<std::size_t>(fy) * static_cast<std::size_t>(cw) + static_cast<std::size_t>(fx / 2);
                frame.cb[ca] = blend(frame.cb[ca], kBackgroundC, a10);
                frame.cr[ca] = blend(frame.cr[ca], kBackgroundC, a10);
            }
        }
    }
}
#else
int textWidth(std::string const& text, int scale)
{
    return static_cast<int>(text.size()) * 8 * scale;
}

void drawText(Frame422& frame, int x, int y, int scale, std::string const& text, std::uint16_t luma, std::uint16_t alpha)
{
    int const width = frame.width;
    int const cw = frame.chromaWidth();
    for (std::size_t n = 0; n < text.size(); ++n)
    {
        auto const code = static_cast<unsigned char>(text[n]);
        auto const* glyph = font8x8_basic[code < 128 ? code : '?'];
        int const gx = x + static_cast<int>(n) * 8 * scale;
        for (int fy = 0; fy < 8; ++fy)
        {
            auto const bits = static_cast<unsigned char>(glyph[fy]);
            for (int fx = 0; fx < 8; ++fx)
            {
                if ((bits & (1u << fx)) == 0)
                {
                    continue;
                }
                for (int sy = 0; sy < scale; ++sy)
                {
                    int const row = y + fy * scale + sy;
                    if (row < 0 || row >= frame.height)
                    {
                        continue;
                    }
                    for (int sx = 0; sx < scale; ++sx)
                    {
                        int const col = gx + fx * scale + sx;
                        if (col < 0 || col >= width)
                        {
                            continue;
                        }
                        frame.y[static_cast<std::size_t>(row) * width + col] = luma;
                        if (!frame.a.empty())
                        {
                            frame.a[static_cast<std::size_t>(row) * width + col] = alpha;
                        }
                        frame.cb[static_cast<std::size_t>(row) * cw + col / 2] = kBackgroundC;
                        frame.cr[static_cast<std::size_t>(row) * cw + col / 2] = kBackgroundC;
                    }
                }
            }
        }
    }
}
#endif
} // namespace scopes
