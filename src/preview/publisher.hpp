#pragma once

#include "app/state.hpp"
#include "preview/plan.hpp"

#include <functional>
#include <memory>
#include <string>

namespace scopes
{
// The WebRTC preview's encoder (SPECIFICATION §2.5). Its own thread takes the newest scope picture
// at the preview rate, scales it to SCOPES_PREVIEW_HEIGHT in NV12, encodes it as H.264 (libx264;
// h264_nvenc is tried first and used when it opens) and publishes it over RTSP/TCP to MediaMTX,
// which serves WHEP and HLS. The media threads never wait for it. Adapted from mxl-replay
// src/ops/publisher.cpp (GPL-3.0, copied into this MIT project with the author's permission) and
// mxl-multiviewer src/media/publisher.cpp (Apache-2.0, see NOTICE), both by the same author.
class PreviewPublisher
{
public:
    // `picture`: the newest scope picture (null before the first). `fps`: pictures per second now
    // (SCOPES_PREVIEW_FPS, at most the update rate; read on every picture).
    PreviewPublisher(std::string url, int previewHeight, std::function<std::shared_ptr<Snapshot const>()> picture, std::function<double()> fps);
    ~PreviewPublisher();
    PreviewPublisher(PreviewPublisher const&) = delete;
    PreviewPublisher& operator=(PreviewPublisher const&) = delete;

    // `observe` gets the seconds this thread spent on each published picture (scale, encode, send).
    void start(std::function<void(double)> observe);
    void stop();
    [[nodiscard]] PreviewStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace scopes
