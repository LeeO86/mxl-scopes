#include "scope/measure.hpp"

#include <cstdio>
#include <sstream>

namespace scopes
{
void BinTotals::reset(int binCount, int planeCount)
{
    bins = binCount;
    planes = planeCount;
    for (int p = 0; p < 3; ++p)
    {
        plane[static_cast<std::size_t>(p)].assign(p < planeCount ? static_cast<std::size_t>(binCount) : 0, 0);
    }
}

int binCentreCode(int bin, int bins)
{
    int const low = (bin * 1024 + bins - 1) / bins;
    int const high = ((bin + 1) * 1024 + bins - 1) / bins - 1;
    return (low + high + 1) / 2;
}

namespace
{
struct PlaneStats
{
    std::uint64_t total = 0;
    int minCode = -1;
    int maxCode = -1;
    double sum = 0;
    std::uint64_t below = 0;
    std::uint64_t above = 0;
};

PlaneStats planeStats(std::vector<std::uint64_t> const& bins, int below, int above)
{
    PlaneStats s;
    int const n = static_cast<int>(bins.size());
    for (int b = 0; b < n; ++b)
    {
        auto const count = bins[static_cast<std::size_t>(b)];
        if (count == 0)
        {
            continue;
        }
        int const code = binCentreCode(b, n);
        s.total += count;
        s.minCode = s.minCode < 0 ? code : s.minCode;
        s.maxCode = code;
        s.sum += static_cast<double>(code) * static_cast<double>(count);
        s.below += code < below ? count : 0;
        s.above += code > above ? count : 0;
    }
    return s;
}

std::string num(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}
} // namespace

Measurements measure(ScopeKind kind, BinTotals const& totals, std::uint64_t outOfGamut, std::uint64_t pixels)
{
    Measurements m;
    m.codesPerBin = totals.bins > 0 ? 1024.0 / totals.bins : 0;
    if (kind != ScopeKind::Rgb && totals.planes >= 1)
    {
        auto const luma = planeStats(totals.plane[0], 64, 940);
        if (luma.total > 0)
        {
            auto const n = static_cast<double>(luma.total);
            m.lumaMin = luma.minCode;
            m.lumaMax = luma.maxCode;
            m.lumaMean = luma.sum / n;
            m.lumaBelow = static_cast<double>(luma.below) / n;
            m.lumaAbove = static_cast<double>(luma.above) / n;
        }
    }
    if (kind == ScopeKind::Ycbcr && totals.planes == 3)
    {
        auto const cb = planeStats(totals.plane[1], 64, 960);
        auto const cr = planeStats(totals.plane[2], 64, 960);
        if (cb.total + cr.total > 0)
        {
            m.chromaOut = static_cast<double>(cb.below + cb.above + cr.below + cr.above) / static_cast<double>(cb.total + cr.total);
        }
    }
    if (kind == ScopeKind::Rgb && pixels > 0)
    {
        m.rgbOutOfGamut = static_cast<double>(outOfGamut) / static_cast<double>(pixels);
    }
    return m;
}

std::string measurementsJson(Measurements const& m)
{
    std::ostringstream out;
    out << "{\"update\":" << m.update << ",\"mode\":\"" << m.mode << "\",\"codes_per_bin\":" << num(m.codesPerBin) << ",\"luma\":";
    if (m.lumaMin)
    {
        out << "{\"min_code\":" << *m.lumaMin << ",\"max_code\":" << *m.lumaMax << ",\"mean_code\":" << num(*m.lumaMean) << ",\"below_64_ratio\":" << num(*m.lumaBelow)
            << ",\"above_940_ratio\":" << num(*m.lumaAbove) << "}";
    }
    else
    {
        out << "null";
    }
    out << ",\"chroma\":";
    if (m.chromaOut)
    {
        out << "{\"out_of_range_ratio\":" << num(*m.chromaOut) << "}";
    }
    else
    {
        out << "null";
    }
    out << ",\"rgb\":";
    if (m.rgbOutOfGamut)
    {
        out << "{\"out_of_gamut_ratio\":" << num(*m.rgbOutOfGamut) << "}";
    }
    else
    {
        out << "null";
    }
    out << "}";
    return out.str();
}
} // namespace scopes
