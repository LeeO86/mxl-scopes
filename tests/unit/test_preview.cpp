// The WebRTC preview (SPECIFICATION §2.5): the plan of the preview contract, the MediaMTX config of
// own mode, the status JSON, the preview raster and the v210 -> NV12 area average.
#include <doctest/doctest.h>

#include "config/config.hpp"
#include "media/patterns.hpp"
#include "media/v210.hpp"
#include "preview/plan.hpp"

#include <map>
#include <string>
#include <vector>

using namespace scopes;

namespace
{
Config configWith(std::map<std::string, std::string> env)
{
    env.emplace("NMOS_HOST_ADDRESS", "10.0.0.5");
    return loadConfig(env, {});
}
} // namespace

TEST_CASE("preview plan: own and shared mode, path, URLs")
{
    auto own = configWith({});
    auto plan = previewPlan(own);
    CHECK(plan.webrtc);
    CHECK(plan.ownMediamtx);
    CHECK(plan.path == "mxl-scopes/scope");
    CHECK(plan.publishUrl == "rtsp://127.0.0.1:8574/mxl-scopes/scope");
    CHECK(previewWhepUrl(own) == "http://10.0.0.5:8579/mxl-scopes/scope/whep");
    CHECK(previewHlsUrl(own) == "http://10.0.0.5:8578/mxl-scopes/scope/index.m3u8");
    auto shared = configWith({{"PREVIEW_PUBLISH_URL", "rtsp://mediamtx:8554"}, {"PREVIEW_PATH_PREFIX", "prod/scope1"}, {"PREVIEW_WHEP_URL", "https://preview.example"}});
    plan = previewPlan(shared);
    CHECK_FALSE(plan.ownMediamtx);
    CHECK(plan.publishUrl == "rtsp://mediamtx:8554/prod/scope1/scope");
    CHECK(previewWhepUrl(shared) == "https://preview.example/prod/scope1/scope/whep");
    auto off = configWith({{"SCOPES_PREVIEW_MODE", "off"}});
    CHECK_FALSE(previewPlan(off).webrtc);
    CHECK(previewJson(off, {}).find("\"state\":\"off\"") != std::string::npos);
}

TEST_CASE("preview: the own MediaMTX config and the status JSON")
{
    auto cfg = configWith({});
    auto const yml = renderMediamtxConfig(cfg);
    CHECK(yml.find("rtspAddress: 127.0.0.1:8574\n") != std::string::npos);
    CHECK(yml.find("rtspTransports: [tcp]\n") != std::string::npos);
    CHECK(yml.find("webrtcAddress: :8579\n") != std::string::npos);
    CHECK(yml.find("hlsAddress: :8578\n") != std::string::npos);
    CHECK(yml.find("webrtcLocalUDPAddress: :8279\n") != std::string::npos);
    CHECK(yml.find("webrtcAdditionalHosts: [\"10.0.0.5\"]\n") != std::string::npos);
    PreviewStatus status;
    status.state = "publishing";
    status.encoder = "x264";
    status.frames = 12;
    status.mediamtxRunning = true;
    auto const json = previewJson(cfg, status);
    for (auto const* part : {"\"mode\":\"webrtc\"", "\"publish\":\"own\"", "\"path\":\"mxl-scopes/scope\"", "\"state\":\"publishing\"", "\"encoder\":\"x264\"",
             "\"frames\":12", "\"mediamtx\":{\"running\":true,\"restarts\":0}", "\"whep\":\"http://10.0.0.5:8579/mxl-scopes/scope/whep\""})
    {
        CAPTURE(part);
        CHECK(json.find(part) != std::string::npos);
    }
}

TEST_CASE("preview raster: SCOPES_PREVIEW_HEIGHT, the aspect kept, even")
{
    CHECK(previewSize(1920, 1080, 540).width == 960);
    CHECK(previewSize(1920, 1080, 540).height == 540);
    CHECK(previewSize(1280, 720, 1080).height == 720); // never above the output
    CHECK(previewSize(1280, 720, 1080).width == 1280);
    CHECK(previewSize(3840, 2160, 270).width == 480);
    auto const odd = previewSize(1000, 1080, 540);
    CHECK(odd.width % 2 == 0);
    CHECK(odd.height == 540);
}

TEST_CASE("v210 -> NV12: area average, 10 to 8 bits, chroma per 2x2")
{
    // A flat field keeps its value; a 2x2 checker of 64 and 940 averages to 502 (125.5 -> 126).
    Frame422 frame;
    frame.allocate(1920, 1080);
    frame.fill(512, 256, 768);
    auto packed = packPattern(frame);
    PreviewSize const size{960, 540};
    std::vector<std::uint8_t> y(960 * 540);
    std::vector<std::uint8_t> uv(960 * 270);
    v210ToNv12(packed.data(), v210RowBytes(1920), 1920, 1080, size, y.data(), 960, uv.data(), 960);
    CHECK(y[0] == 128);
    CHECK(y[960 * 540 - 1] == 128);
    CHECK(uv[0] == 64);
    CHECK(uv[1] == 192);
    for (int r = 0; r < 1080; ++r)
    {
        for (int x = 0; x < 1920; ++x)
        {
            frame.y[static_cast<std::size_t>(r) * 1920 + static_cast<std::size_t>(x)] = ((r + x) % 2) != 0 ? 940 : 64;
        }
    }
    packed = packPattern(frame);
    v210ToNv12(packed.data(), v210RowBytes(1920), 1920, 1080, size, y.data(), 960, uv.data(), 960);
    CHECK(y[0] == 126);
    CHECK(y[480 * 960 + 333] == 126);
    // A non-integer ratio (1280x720 -> 960x540) stays inside the input range.
    Frame422 ramp;
    ramp.allocate(1280, 720);
    for (int r = 0; r < 720; ++r)
    {
        for (int x = 0; x < 1280; ++x)
        {
            ramp.y[static_cast<std::size_t>(r) * 1280 + static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(64 + x * 876 / 1279);
        }
    }
    packed = packPattern(ramp);
    v210ToNv12(packed.data(), v210RowBytes(1280), 1280, 720, size, y.data(), 960, uv.data(), 960);
    CHECK(y[0] == 16);
    CHECK(y[959] == 235);
    for (int x = 1; x < 960; ++x)
    {
        CHECK(y[static_cast<std::size_t>(x)] >= y[static_cast<std::size_t>(x) - 1]);
    }
}
