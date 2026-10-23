#pragma once

#include "config/config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scopes
{
// The WebRTC preview of the platform preview contract (SPECIFICATION §2.5). Adapted from
// mxl-replay src/ops/preview.hpp (GPL-3.0, copied into this MIT project with the author's
// permission, Copyright (c) 2026 Adrian Hilber).
struct PreviewPlan
{
    bool webrtc = false;
    // webrtc without PREVIEW_PUBLISH_URL: this process runs its own MediaMTX.
    bool ownMediamtx = false;
    // <PREVIEW_PATH_PREFIX>/scope.
    std::string path;
    // PREVIEW_PUBLISH_URL, or rtsp://127.0.0.1:MEDIAMTX_RTSP_PORT.
    std::string publishBase;
    // <publishBase>/<path>.
    std::string publishUrl;
};
[[nodiscard]] PreviewPlan previewPlan(Config const& cfg);

// <PREVIEW_WHEP_URL>/<path>/whep and <PREVIEW_HLS_URL>/<path>/index.m3u8, or the own MediaMTX on
// NMOS_HOST_ADDRESS when the setting is empty (the page then puts in its own host name).
[[nodiscard]] std::string previewWhepUrl(Config const& cfg);
[[nodiscard]] std::string previewHlsUrl(Config const& cfg);

// The built-in MediaMTX (own mode): RTSP ingest on 127.0.0.1:MEDIAMTX_RTSP_PORT (TCP only), WHEP on
// MEDIAMTX_WHEP_PORT with ICE on MEDIAMTX_ICE_UDP_PORT (UDP and TCP) and NMOS_HOST_ADDRESS as ICE
// host, low-latency HLS on MEDIAMTX_HLS_PORT; no API, metrics, RTMP, SRT or MoQ. Adapted from
// mxl-multiviewer src/ops/mediamtx.cpp (Apache-2.0, see NOTICE), itself from mxl-webrtc-monitor.
[[nodiscard]] std::string renderMediamtxConfig(Config const& cfg);

// The state of the preview's RTSP publish, as /statusz and the metrics show it.
struct PreviewStatus
{
    // "off" (SCOPES_PREVIEW_MODE=off), "connecting" until MediaMTX took the stream, "publishing"
    // while packets go out, "error" after a failure (with `error`) until it publishes again.
    std::string state = "off";
    std::string error;
    // "nvenc" or "x264" once an encoder is open.
    std::string encoder;
    std::uint64_t frames = 0;
    int width = 0;
    int height = 0;
    double fps = 0;
    // The built-in MediaMTX (own mode).
    bool mediamtxRunning = false;
    std::uint64_t mediamtxRestarts = 0;
    [[nodiscard]] bool operator==(PreviewStatus const&) const = default;
};

// The "preview" object of /api/v1/info and /api/v1/status.
[[nodiscard]] std::string previewJson(Config const& cfg, PreviewStatus const& status);

// The preview raster for an output of `width` × `height`: SCOPES_PREVIEW_HEIGHT lines (at most the
// output's), the width keeping the aspect, both even.
struct PreviewSize
{
    int width = 0;
    int height = 0;
};
[[nodiscard]] PreviewSize previewSize(int outputWidth, int outputHeight, int previewHeight);

// Packed v210 (rowBytes per line) -> 8-bit NV12 of `size` by area average (§2.5): each output
// sample is the mean of the input samples whose box it covers (integer boxes, rounded). The
// chroma of NV12 is 4:2:0: a CbCr pair per 2×2 output pixels.
void v210ToNv12(std::uint8_t const* v210, std::size_t rowBytes, int width, int height, PreviewSize size, std::uint8_t* y, int yStride, std::uint8_t* uv,
    int uvStride);
} // namespace scopes
