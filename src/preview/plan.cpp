#include "preview/plan.hpp"

#include "media/v210.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

namespace scopes
{
namespace
{
std::string quote(std::string const& text)
{
    return "\"" + jsonEscape(text) + "\"";
}

// For each of `in` input positions, the output position whose box [o × in / out, (o + 1) × in /
// out) holds it; and each output's box size.
void boxes(int in, int out, std::vector<int>& of, std::vector<int>& count)
{
    of.assign(static_cast<std::size_t>(in), 0);
    count.assign(static_cast<std::size_t>(out), 0);
    for (int o = 0; o < out; ++o)
    {
        int const a = static_cast<int>(static_cast<std::int64_t>(o) * in / out);
        int const b = std::max(a + 1, static_cast<int>(static_cast<std::int64_t>(o + 1) * in / out));
        for (int i = a; i < std::min(b, in); ++i)
        {
            of[static_cast<std::size_t>(i)] = o;
        }
        count[static_cast<std::size_t>(o)] = std::min(b, in) - a;
    }
}
} // namespace

PreviewPlan previewPlan(Config const& cfg)
{
    PreviewPlan plan;
    plan.webrtc = cfg.previewMode == "webrtc";
    plan.ownMediamtx = plan.webrtc && cfg.previewPublishUrl.empty();
    plan.path = cfg.previewPathPrefix + "/scope";
    plan.publishBase = cfg.previewPublishUrl.empty() ? "rtsp://127.0.0.1:" + std::to_string(cfg.mediamtxRtspPort) : cfg.previewPublishUrl;
    plan.publishUrl = plan.publishBase + "/" + plan.path;
    return plan;
}

std::string previewWhepUrl(Config const& cfg)
{
    auto const base = cfg.previewWhepUrl.empty() ? "http://" + cfg.nmosHostAddress + ":" + std::to_string(cfg.mediamtxWhepPort) : cfg.previewWhepUrl;
    return base + "/" + previewPlan(cfg).path + "/whep";
}

std::string previewHlsUrl(Config const& cfg)
{
    auto const base = cfg.previewHlsUrl.empty() ? "http://" + cfg.nmosHostAddress + ":" + std::to_string(cfg.mediamtxHlsPort) : cfg.previewHlsUrl;
    return base + "/" + previewPlan(cfg).path + "/index.m3u8";
}

std::string renderMediamtxConfig(Config const& cfg)
{
    std::string yml;
    yml += "logLevel: warn\n";
    yml += "api: false\n";
    yml += "metrics: false\n";
    yml += "pprof: false\n";
    yml += "playback: false\n";
    yml += "rtsp: true\n";
    yml += "rtspAddress: 127.0.0.1:" + std::to_string(cfg.mediamtxRtspPort) + "\n";
    // TCP only: without UDP, MediaMTX binds no RTP/RTCP ports (8000/8001 in every instance).
    yml += "rtspTransports: [tcp]\n";
    yml += "rtmp: false\n";
    yml += "srt: false\n";
    yml += "moq: false\n";
    yml += "hls: true\n";
    yml += "hlsAddress: :" + std::to_string(cfg.mediamtxHlsPort) + "\n";
    yml += "hlsAllowOrigins: [\"*\"]\n";
    yml += "hlsVariant: lowLatency\n";
    yml += "hlsSegmentDuration: 1s\n";
    yml += "hlsPartDuration: 200ms\n";
    yml += "webrtc: true\n";
    yml += "webrtcAddress: :" + std::to_string(cfg.mediamtxWhepPort) + "\n";
    yml += "webrtcAllowOrigins: [\"*\"]\n";
    yml += "webrtcLocalUDPAddress: :" + std::to_string(cfg.mediamtxIcePort) + "\n";
    yml += "webrtcLocalTCPAddress: :" + std::to_string(cfg.mediamtxIcePort) + "\n";
    yml += cfg.nmosHostAddress.empty() ? "webrtcAdditionalHosts: []\n" : "webrtcAdditionalHosts: [\"" + cfg.nmosHostAddress + "\"]\n";
    yml += "webrtcICEServers2: []\n";
    yml += "pathDefaults:\n";
    yml += "  source: publisher\n";
    yml += "paths:\n";
    yml += "  all_others:\n";
    return yml;
}

std::string previewJson(Config const& cfg, PreviewStatus const& status)
{
    auto const plan = previewPlan(cfg);
    std::ostringstream out;
    out << "{\"mode\":" << quote(cfg.previewMode);
    if (plan.webrtc)
    {
        out << ",\"publish\":" << quote(plan.ownMediamtx ? "own" : "shared") << ",\"publish_url\":" << quote(plan.publishBase)
            << ",\"path_prefix\":" << quote(cfg.previewPathPrefix) << ",\"path\":" << quote(plan.path) << ",\"state\":" << quote(status.state)
            << ",\"error\":" << quote(status.error) << ",\"encoder\":" << quote(status.encoder) << ",\"frames\":" << status.frames
            << ",\"width\":" << status.width << ",\"height\":" << status.height << ",\"fps\":" << status.fps << ",\"whep\":" << quote(previewWhepUrl(cfg))
            << ",\"hls\":" << quote(previewHlsUrl(cfg)) << ",\"public\":{\"whep\":" << (cfg.previewWhepUrl.empty() ? "false" : "true")
            << ",\"hls\":" << (cfg.previewHlsUrl.empty() ? "false" : "true") << "}";
        if (plan.ownMediamtx)
        {
            out << ",\"mediamtx\":{\"running\":" << (status.mediamtxRunning ? "true" : "false") << ",\"restarts\":" << status.mediamtxRestarts << "}";
        }
    }
    else
    {
        out << ",\"state\":\"off\"";
    }
    out << ",\"snapshot\":\"/api/v1/snapshot.png\"}";
    return out.str();
}

PreviewSize previewSize(int outputWidth, int outputHeight, int previewHeight)
{
    PreviewSize size;
    size.height = std::max(2, std::min(previewHeight, outputHeight) & ~1);
    auto const width = static_cast<int>((static_cast<std::int64_t>(outputWidth) * size.height + outputHeight / 2) / std::max(1, outputHeight));
    size.width = std::max(2, width & ~1);
    return size;
}

void v210ToNv12(std::uint8_t const* v210, std::size_t rowBytes, int width, int height, PreviewSize size, std::uint8_t* y, int yStride, std::uint8_t* uv,
    int uvStride)
{
    int const cw = width / 2;
    int const ow = size.width;
    int const oh = size.height;
    std::vector<int> xOf, xCount, yOf, yCount, cxOf, cxCount, cyOf, cyCount;
    boxes(width, ow, xOf, xCount);
    boxes(height, oh, yOf, yCount);
    boxes(cw, ow / 2, cxOf, cxCount);
    boxes(height, oh / 2, cyOf, cyCount);
    Frame422 row;
    row.allocate(width, 1);
    std::vector<std::uint32_t> ySum(static_cast<std::size_t>(ow), 0);
    std::vector<std::uint32_t> cbSum(static_cast<std::size_t>(ow / 2), 0);
    std::vector<std::uint32_t> crSum(static_cast<std::size_t>(ow / 2), 0);
    auto const to8 = [](std::uint32_t sum, std::uint32_t count) {
        // The mean of 10-bit samples in 8 bits, rounded.
        return static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (sum + 2 * count) / (4 * count)));
    };
    for (int r = 0; r < height; ++r)
    {
        unpackV210(v210 + static_cast<std::size_t>(r) * rowBytes, rowBytes, row);
        for (int x = 0; x < width; ++x)
        {
            ySum[static_cast<std::size_t>(xOf[static_cast<std::size_t>(x)])] += row.y[static_cast<std::size_t>(x)];
        }
        for (int x = 0; x < cw; ++x)
        {
            auto const o = static_cast<std::size_t>(cxOf[static_cast<std::size_t>(x)]);
            cbSum[o] += row.cb[static_cast<std::size_t>(x)];
            crSum[o] += row.cr[static_cast<std::size_t>(x)];
        }
        int const oy = yOf[static_cast<std::size_t>(r)];
        if (r + 1 == height || yOf[static_cast<std::size_t>(r) + 1] != oy)
        {
            auto* dst = y + static_cast<std::size_t>(oy) * static_cast<std::size_t>(yStride);
            for (int o = 0; o < ow; ++o)
            {
                auto const count = static_cast<std::uint32_t>(xCount[static_cast<std::size_t>(o)] * yCount[static_cast<std::size_t>(oy)]);
                dst[o] = to8(ySum[static_cast<std::size_t>(o)], count);
            }
            std::fill(ySum.begin(), ySum.end(), 0u);
        }
        int const cy = cyOf[static_cast<std::size_t>(r)];
        if (r + 1 == height || cyOf[static_cast<std::size_t>(r) + 1] != cy)
        {
            auto* dst = uv + static_cast<std::size_t>(cy) * static_cast<std::size_t>(uvStride);
            for (int o = 0; o < ow / 2; ++o)
            {
                auto const count = static_cast<std::uint32_t>(cxCount[static_cast<std::size_t>(o)] * cyCount[static_cast<std::size_t>(cy)]);
                dst[2 * o] = to8(cbSum[static_cast<std::size_t>(o)], count);
                dst[2 * o + 1] = to8(crSum[static_cast<std::size_t>(o)], count);
            }
            std::fill(cbSum.begin(), cbSum.end(), 0u);
            std::fill(crSum.begin(), crSum.end(), 0u);
        }
    }
}
} // namespace scopes
