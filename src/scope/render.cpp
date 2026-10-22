#include "scope/render.hpp"

#include "media/patterns.hpp"

#include <font8x8_basic.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
        return {{0}, {false}, {}, {white}, true};
    }
    return {{0}, {false}, {}, {traceColor(settings.traceColor)}, false};
}
} // namespace

int panelCount(std::string const& mode)
{
    return mode == "ycbcr-parade" || mode == "rgb-parade" ? 3 : 1;
}

Layout makeLayout(int width, int height, int panels)
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
    return inputWidth <= 0 ? layout.panelW : std::min(layout.panelW, inputWidth);
}

int histogramBins(Layout const& layout)
{
    return std::min(layout.plotH, 1024);
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
        // §4.4: the R′, G′ and B′ traces add in R′G′B′ (pure primaries, so a neutral trace is
        // white); BT.709 to Y′CbCr per component, × 64 for the integer sum.
        double const ky[3] = {0.2126, 0.7152, 0.0722};
        double const kcb[3] = {-0.2126 / 1.8556, -0.7152 / 1.8556, 0.5};
        double const kcr[3] = {0.5, -0.7152 / 1.5748, -0.0722 / 1.5748};
        for (int c = 0; c < 3; ++c)
        {
            auto const& lut = plan.planeIntensity[static_cast<std::size_t>(c)];
            auto& table = plan.overlayColour[static_cast<std::size_t>(c)];
            table.resize(lut.size());
            for (std::size_t n = 0; n < lut.size(); ++n)
            {
                double const v = lut[n] / 1023.0 * 64.0;
                table[n] = {static_cast<std::int32_t>(std::lround(ky[c] * 876.0 * v)), static_cast<std::int32_t>(std::lround(kcb[c] * 896.0 * v)),
                    static_cast<std::int32_t>(std::lround(kcr[c] * 896.0 * v))};
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

RenderPlan makeRenderPlan(Layout const& layout, DisplaySettings const& settings, int columns, PlaneCounts const& counts, bool key)
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
    int const lineLuma = 64 + static_cast<int>(std::lround(876 * 0.8));
    plan.base.allocate(layout.width, layout.height);
    plan.base.fill(kBackgroundY, kBackgroundC, kBackgroundC);
    if (key)
    {
        plan.base.a.assign(plan.base.y.size(), plan.bgAlpha);
    }
    int const s = layout.textScale;
    auto const labelLuma = static_cast<std::uint16_t>(kBackgroundY + scale1023(lineLuma - kBackgroundY, level * 1023 / 100));
    if (level > 0)
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
                for (int x = 0; x < layout.panelW; ++x)
                {
                    if (line.style == "dotted" && ((x / (2 * s)) % 2) != 0)
                    {
                        continue;
                    }
                    plan.plotGraticule.push_back({static_cast<std::uint16_t>(x0 + x), static_cast<std::uint16_t>(line.row), alpha, static_cast<std::uint16_t>(lineLuma)});
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
        // Units in the top-left corner of the margin.
        std::string const units = settings.graticuleUnits == "mv" ? "mV" : settings.graticuleUnits == "percent" ? "%" : "code";
        int const bandBottom = std::max(0, layout.plotY - 4 * s);
        drawText(plan.base, std::max(0, layout.plotX - 3 * s - textWidth(units, s)), std::max(0, (bandBottom - 8 * s) / 2), s, units, labelLuma, plan.labelAlpha);
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
            cbs[g.x / 2] = blend(cbs[g.x / 2], kBackgroundC, g.alpha);
            crs[g.x / 2] = blend(crs[g.x / 2], kBackgroundC, g.alpha);
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
// The cells of one panel row -> its trace groups (graticule blended over). Groups without
// counts keep the still picture's bytes. `lines` holds the plane rows of the panel: one, or
// three for the overlay. Returns true when the row got a trace group.
template <bool OVERLAY, class C>
bool panelRow(RenderPlan const& plan, PanelPlan const& panel, C const* const* lines, std::uint32_t& gi, std::uint32_t gend, std::size_t rowOffset,
    std::vector<TraceGroup>& trace)
{
    auto const& layout = plan.layout;
    int const groups = layout.panelW / 6;
    auto const traceBefore = trace.size();
    bool lastEmpty = true;
    auto const* line = lines[0];
    auto const& colour = plan.colours[static_cast<std::size_t>(panel.colour)];
    auto const* lut = plan.planeIntensity[static_cast<std::size_t>(panel.plane)].data();
    auto const maxCount = plan.planeMax[static_cast<std::size_t>(panel.plane)];
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
        // The key (§7): max(background alpha, trace intensity), the graticule over it.
        std::uint16_t a[6] = {};
        if (plan.key)
        {
            for (int i = 0; i < 6; ++i)
            {
                std::uint16_t level = 0;
                for (int p = 0; p < (OVERLAY ? 3 : 1); ++p)
                {
                    auto const plane = static_cast<std::size_t>(OVERLAY ? p : panel.plane);
                    level = std::max(level, plan.planeIntensity[plane][std::min<std::uint32_t>(lines[p][6 * g + i], plan.planeMax[plane])]);
                }
                a[i] = std::max(plan.bgAlpha, level);
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
        else
        {
            auto const* c = line + 6 * g;
            for (int p = 0; p < 6; p += 2)
            {
                std::uint32_t const c0 = c[p];
                std::uint32_t const c1 = c[p + 1];
                // intensity[0] is 0: no branch on empty cells.
                int const i0 = lut[std::min(c0, maxCount)];
                int const i1 = lut[std::min(c1, maxCount)];
                y[p] = colour.y[static_cast<std::size_t>(i0)];
                y[p + 1] = colour.y[static_cast<std::size_t>(i1)];
                auto const mean = static_cast<std::size_t>((i0 + i1 + 1) >> 1);
                cb[p / 2] = colour.cb[mean];
                cr[p / 2] = colour.cr[mean];
            }
        }
        for (; gi < gend && plan.plotGraticule[gi].x < x0 + 6; ++gi)
        {
            auto const& gp = plan.plotGraticule[gi];
            int const at = gp.x - x0;
            y[at] = blend(y[at], gp.luma, gp.alpha);
            a[at] = std::max(a[at], gp.alpha);
            if ((at & 1) == 0)
            {
                cb[at / 2] = blend(cb[at / 2], kBackgroundC, gp.alpha);
                cr[at / 2] = blend(cr[at / 2], kBackgroundC, gp.alpha);
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
        if (totals != nullptr && (r == 0 || plan.binOfRow[static_cast<std::size_t>(r) - 1] != bin))
        {
            // The first plot row of this bin: its samples over all columns (§4.7).
            for (int p = 0; p < plan.planes; ++p)
            {
                std::uint64_t sum = 0;
                auto const* cells = rows[static_cast<std::size_t>(p)];
                for (int c = 0; c < plan.columns; ++c)
                {
                    sum += cells[c];
                }
                totals->plane[static_cast<std::size_t>(p)][bin] = sum;
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
                }
            }
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
                           hist.cells() == planeCells * static_cast<std::size_t>(plan.planes);
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
    for (int row = 0; row < bandBottom; ++row)
    {
        if (!frame.a.empty())
        {
            std::fill(frame.a.begin() + static_cast<std::ptrdiff_t>(row) * width + layout.plotX, frame.a.begin() + static_cast<std::ptrdiff_t>(row + 1) * width,
                plan.bgAlpha);
        }
        std::fill(frame.y.begin() + static_cast<std::ptrdiff_t>(row) * width + layout.plotX, frame.y.begin() + static_cast<std::ptrdiff_t>(row + 1) * width,
            kBackgroundY);
        std::fill(frame.cb.begin() + static_cast<std::ptrdiff_t>(row) * cw + layout.plotX / 2, frame.cb.begin() + static_cast<std::ptrdiff_t>(row + 1) * cw,
            kBackgroundC);
        std::fill(frame.cr.begin() + static_cast<std::ptrdiff_t>(row) * cw + layout.plotX / 2, frame.cr.begin() + static_cast<std::ptrdiff_t>(row + 1) * cw,
            kBackgroundC);
    }
    if (!caption.empty())
    {
        drawText(frame, layout.plotX, std::max(0, (bandBottom - 8 * s) / 2), s, caption, 700);
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
} // namespace scopes
