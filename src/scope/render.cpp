#include "scope/render.hpp"

#include "media/patterns.hpp"

#include <font8x8_basic.h>

#include <algorithm>
#include <bit>
#include <cmath>
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
} // namespace

Layout makeLayout(int width, int height)
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
    return layout;
}

int histogramColumns(Layout const& layout, int inputWidth)
{
    return inputWidth <= 0 ? layout.plotW : std::min(layout.plotW, inputWidth);
}

int histogramBins(Layout const& layout)
{
    return std::min(layout.plotH, 1024);
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
    double const g = settings.intensity / 100.0 * 4.0;
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

RenderPlan makeRenderPlan(Layout const& layout, DisplaySettings const& settings, int columns, std::uint64_t samples, std::uint32_t maxCount)
{
    RenderPlan plan;
    plan.layout = layout;
    plan.columns = columns;
    plan.bins = histogramBins(layout);
    plan.columnOf.resize(static_cast<std::size_t>(layout.plotW));
    for (int px = 0; px < layout.plotW; ++px)
    {
        plan.columnOf[static_cast<std::size_t>(px)] = static_cast<std::uint32_t>((static_cast<std::int64_t>(px) * columns) / layout.plotW);
    }
    plan.identityColumns = columns == layout.plotW;
    plan.binOfRow.resize(static_cast<std::size_t>(layout.plotH));
    for (int r = 0; r < layout.plotH; ++r)
    {
        plan.binOfRow[static_cast<std::size_t>(r)] =
            static_cast<std::uint16_t>((static_cast<std::int64_t>(layout.plotH - 1 - r) * plan.bins) / layout.plotH);
    }
    // §4.4: c_ref is the count of a column whose samples spread evenly over the bins.
    double const cRef = columns > 0 && samples > 0 ? static_cast<double>(samples) / columns / plan.bins : 1.0;
    plan.intensity = intensityTable(settings, cRef, maxCount);
    plan.maxCount = static_cast<std::uint32_t>(plan.intensity.size() - 1);
    auto const trace = traceColor(settings.traceColor);
    for (int i = 0; i < 1024; ++i)
    {
        plan.yOf[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(kBackgroundY + scale1023(trace.y - kBackgroundY, i));
        plan.cbOf[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(kBackgroundC + scale1023(trace.cb - kBackgroundC, i));
        plan.crOf[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(kBackgroundC + scale1023(trace.cr - kBackgroundC, i));
    }

    // The graticule (§4.5): lines in the plot, labels in the left margin.
    int const level = settings.graticuleLevel;
    int const lineLuma = 64 + static_cast<int>(std::lround(876 * 0.8));
    plan.base.allocate(layout.width, layout.height);
    plan.base.fill(kBackgroundY, kBackgroundC, kBackgroundC);
    if (level > 0)
    {
        plan.lines = lumaLines(settings);
        int const s = layout.textScale;
        auto const labelLuma = static_cast<std::uint16_t>(kBackgroundY + scale1023(lineLuma - kBackgroundY, level * 1023 / 100));
        for (auto& line : plan.lines)
        {
            line.row = rowOfCode(plan, line.code);
            double const strength = line.style == "solid" ? 1.0 : line.style == "dotted" ? 0.9 : 0.5;
            auto const alpha = static_cast<std::uint16_t>(std::lround(1023.0 * strength * level / 100.0));
            for (int x = 0; x < layout.plotW; ++x)
            {
                if (line.style == "dotted" && ((x / (2 * s)) % 2) != 0)
                {
                    continue;
                }
                plan.plotGraticule.push_back(
                    {static_cast<std::uint16_t>(layout.plotX + x), static_cast<std::uint16_t>(line.row), alpha, static_cast<std::uint16_t>(lineLuma)});
            }
            if (!line.label.empty())
            {
                int const w = textWidth(line.label, s);
                drawText(plan.base, layout.plotX - 3 * s - w, line.row - 4 * s, s, line.label, labelLuma);
            }
        }
        std::sort(plan.plotGraticule.begin(), plan.plotGraticule.end(),
            [](GraticulePixel const& a, GraticulePixel const& b) { return a.row != b.row ? a.row < b.row : a.x < b.x; });
        // Units in the top-left corner of the margin.
        std::string const units = settings.graticuleUnits == "mv" ? "mV" : settings.graticuleUnits == "percent" ? "%" : "code";
        int const bandBottom = std::max(0, layout.plotY - 4 * s);
        drawText(plan.base, std::max(0, layout.plotX - 3 * s - textWidth(units, s)), std::max(0, (bandBottom - 8 * s) / 2), s, units, labelLuma);
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
void blendGraticuleRow(RenderPlan const& plan, int r, std::uint16_t* ys, std::uint16_t* cbs, std::uint16_t* crs)
{
    auto const begin = plan.graticuleStart[static_cast<std::size_t>(r)];
    auto const end = plan.graticuleStart[static_cast<std::size_t>(r) + 1];
    for (auto i = begin; i < end; ++i)
    {
        auto const& g = plan.plotGraticule[i];
        ys[g.x] = blend(ys[g.x], g.luma, g.alpha);
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
        std::fill(ys + layout.plotX, ys + layout.plotX + layout.plotW, kBackgroundY);
        std::fill(cbs + layout.plotX / 2, cbs + (layout.plotX + layout.plotW) / 2, kBackgroundC);
        std::fill(crs + layout.plotX / 2, crs + (layout.plotX + layout.plotW) / 2, kBackgroundC);
        blendGraticuleRow(plan, r, ys, cbs, crs);
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

void renderPacked(RenderPlan const& plan, Histogram& hist, Frame422 const& still, std::uint8_t const* stillPacked, std::uint8_t* dst)
{
    auto const& layout = plan.layout;
    auto const rowBytes = v210RowBytes(still.width);
    bool const hasCounts = !hist.counts.empty() && hist.columns == plan.columns && hist.bins == plan.bins;
    if (!hasCounts)
    {
        std::memcpy(dst, stillPacked, rowBytes * static_cast<std::size_t>(still.height));
        return;
    }
    // Rows outside the plot are the still picture's.
    std::memcpy(dst, stillPacked, rowBytes * static_cast<std::size_t>(layout.plotY));
    auto const below = static_cast<std::size_t>(layout.plotY + layout.plotH);
    std::memcpy(dst + rowBytes * below, stillPacked + rowBytes * below, rowBytes * (static_cast<std::size_t>(still.height) - below));
    std::vector<std::uint16_t> sampled(plan.identityColumns ? 0 : static_cast<std::size_t>(layout.plotW));
    auto const* lut = plan.intensity.data();
    auto const maxCount = plan.maxCount;
    int const groups = layout.plotW / 6;
    for (int r = 0; r < layout.plotH; ++r)
    {
        auto const row = static_cast<std::size_t>(layout.plotY + r);
        auto* out = dst + rowBytes * row;
        // The still row: margins, background and graticule. Groups with a trace are packed over it.
        std::memcpy(out, stillPacked + rowBytes * row, rowBytes);
        auto const bin = plan.binOfRow[static_cast<std::size_t>(r)];
        auto* counts = hist.counts.data() + static_cast<std::size_t>(bin) * static_cast<std::size_t>(plan.columns);
        std::uint16_t const* line = counts;
        if (!plan.identityColumns)
        {
            for (int px = 0; px < layout.plotW; ++px)
            {
                sampled[static_cast<std::size_t>(px)] = counts[plan.columnOf[static_cast<std::size_t>(px)]];
            }
            line = sampled.data();
        }
        auto gi = plan.graticuleStart[static_cast<std::size_t>(r)];
        auto const gend = plan.graticuleStart[static_cast<std::size_t>(r) + 1];
        for (int g = 0; g < groups; ++g)
        {
            auto const* c = line + 6 * g;
            int const x0 = layout.plotX + 6 * g;
            std::uint64_t head = 0;
            std::uint32_t tail = 0;
            std::memcpy(&head, c, sizeof(head));
            std::memcpy(&tail, c + 4, sizeof(tail));
            if ((head | tail) == 0)
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
            for (int p = 0; p < 6; p += 2)
            {
                std::uint32_t const c0 = c[p];
                std::uint32_t const c1 = c[p + 1];
                // intensity[0] is 0: no branch on empty cells.
                int const i0 = lut[std::min(c0, maxCount)];
                int const i1 = lut[std::min(c1, maxCount)];
                y[p] = plan.yOf[static_cast<std::size_t>(i0)];
                y[p + 1] = plan.yOf[static_cast<std::size_t>(i1)];
                auto const mean = static_cast<std::size_t>((i0 + i1 + 1) >> 1);
                cb[p / 2] = plan.cbOf[mean];
                cr[p / 2] = plan.crOf[mean];
            }
            for (; gi < gend && plan.plotGraticule[gi].x < x0 + 6; ++gi)
            {
                auto const& gp = plan.plotGraticule[gi];
                int const at = gp.x - x0;
                y[at] = blend(y[at], gp.luma, gp.alpha);
                if ((at & 1) == 0)
                {
                    cb[at / 2] = blend(cb[at / 2], kBackgroundC, gp.alpha);
                    cr[at / 2] = blend(cr[at / 2], kBackgroundC, gp.alpha);
                }
            }
            auto* group = out + 16 * static_cast<std::size_t>(x0 / 6);
            put32(group, word3(cb[0], y[0], cr[0]));
            put32(group + 4, word3(y[1], cb[1], y[2]));
            put32(group + 8, word3(cr[1], y[3], cb[2]));
            put32(group + 12, word3(y[4], cr[2], y[5]));
        }
        // Clear the bin once the last plot row that shows it is drawn.
        if (r + 1 == layout.plotH || plan.binOfRow[static_cast<std::size_t>(r) + 1] != bin)
        {
            std::memset(counts, 0, sizeof(std::uint16_t) * static_cast<std::size_t>(plan.columns));
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

void drawText(Frame422& frame, int x, int y, int scale, std::string const& text, std::uint16_t luma)
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
                        frame.cb[static_cast<std::size_t>(row) * cw + col / 2] = kBackgroundC;
                        frame.cr[static_cast<std::size_t>(row) * cw + col / 2] = kBackgroundC;
                    }
                }
            }
        }
    }
}
} // namespace scopes
