#include "preview/publisher.hpp"

#include "media/v210.hpp"
#include "util/logging.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <mutex>
#include <thread>

namespace scopes
{
namespace
{
// §2.5: 2 Mbit/s CBR.
constexpr std::int64_t kBitrate = 2'000'000;

std::string avError(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

// FFmpeg's warnings go to this process's log, and the last error explains a failed open (for
// NVENC: no libnvidia-encode, no capable GPU).
std::mutex gAvMu;
std::string gAvLast;

void avLog(void* context, int level, char const* format, va_list args)
{
    if (level > AV_LOG_WARNING)
    {
        return;
    }
    char line[1024] = {};
    int prefix = 1;
    av_log_format_line2(context, level, format, args, line, sizeof(line), &prefix);
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    {
        text.pop_back();
    }
    if (text.empty())
    {
        return;
    }
    if (level <= AV_LOG_ERROR)
    {
        std::lock_guard lock{gAvMu};
        gAvLast = text;
    }
    logMessage(level <= AV_LOG_ERROR ? LogLevel::Warn : LogLevel::Debug, "ffmpeg", {{"message", text}});
}

std::string lastAvError(int code)
{
    std::lock_guard lock{gAvMu};
    auto text = gAvLast.empty() ? avError(code) : gAvLast + " (" + avError(code) + ")";
    gAvLast.clear();
    return text;
}
} // namespace

struct PreviewPublisher::Impl
{
    std::string url;
    int previewHeight;
    std::function<std::shared_ptr<Snapshot const>()> picture;
    std::function<double()> fpsNow;
    std::function<void(double)> observe;
    std::thread thread;
    std::mutex runMu;
    std::condition_variable wake;
    bool stopping = false;
    mutable std::mutex statusMu;
    PreviewStatus status;

    AVCodecContext* enc = nullptr;
    AVFormatContext* out = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    bool nvencFailed = false;
    bool idr = true;
    // The raster the encoder was opened for.
    int sourceWidth = 0;
    int sourceHeight = 0;
    PreviewSize size;

    Impl(std::string urlIn, int heightIn, std::function<std::shared_ptr<Snapshot const>()> pictureIn, std::function<double()> fpsIn)
        : url(std::move(urlIn))
        , previewHeight(heightIn)
        , picture(std::move(pictureIn))
        , fpsNow(std::move(fpsIn))
    {
        status.state = "connecting";
    }

    void setState(std::string const& state, std::string const& error)
    {
        std::lock_guard lock{statusMu};
        if (state == "error" && status.error != error)
        {
            logWarn("preview_publish_error", {{"url", url}, {"error", error}});
        }
        else if (state == "publishing" && status.state != "publishing")
        {
            logInfo("preview_publishing", {{"url", url}, {"encoder", status.encoder}});
        }
        status.state = state;
        status.error = error;
    }

    AVCodecContext* context(AVCodec const* codec, double fps) const
    {
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ctx->width = size.width;
        ctx->height = size.height;
        // Timestamps in milliseconds of the clock: the rate may change with the settings.
        ctx->time_base = AVRational{1, 1000};
        ctx->framerate = AVRational{static_cast<int>(std::lround(fps * 1000)), 1000};
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        ctx->color_range = AVCOL_RANGE_MPEG;
        ctx->colorspace = AVCOL_SPC_BT709;
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc = AVCOL_TRC_BT709;
        ctx->bit_rate = kBitrate;
        ctx->rc_max_rate = kBitrate;
        ctx->rc_buffer_size = static_cast<int>(kBitrate);
        // One second GOP: a new WHEP viewer has a picture within a second. No B-frames.
        ctx->gop_size = std::max(1, static_cast<int>(std::lround(fps)));
        ctx->max_b_frames = 0;
        // SPS and PPS in the RTSP SDP; MediaMTX repeats them for WebRTC readers.
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        return ctx;
    }

    bool openEncoder(double fps)
    {
        if (!nvencFailed)
        {
            // h264_nvenc on NV12 frames in host memory (FFmpeg uploads them); used when a GPU with
            // NVENC is there, otherwise x264 (the platform runs the scope without a GPU).
            AVCodec const* nvenc = avcodec_find_encoder_by_name("h264_nvenc");
            std::string reason = nvenc == nullptr ? "h264_nvenc is not in this FFmpeg build" : std::string{};
            if (reason.empty())
            {
                AVCodecContext* ctx = context(nvenc, fps);
                AVDictionary* options = nullptr;
                av_dict_set(&options, "preset", "p1", 0);
                av_dict_set(&options, "tune", "ull", 0);
                av_dict_set(&options, "rc", "cbr", 0);
                av_dict_set(&options, "zerolatency", "1", 0);
                av_dict_set(&options, "delay", "0", 0);
                av_dict_set(&options, "forced-idr", "1", 0);
                int const rc = avcodec_open2(ctx, nvenc, &options);
                av_dict_free(&options);
                if (rc >= 0)
                {
                    enc = ctx;
                    return opened("nvenc", fps);
                }
                reason = "h264_nvenc: " + lastAvError(rc);
                avcodec_free_context(&ctx);
            }
            nvencFailed = true;
            logInfo("preview_nvenc_unavailable", {{"reason", reason}, {"encoder", "x264"}});
        }
        AVCodec const* x264 = avcodec_find_encoder_by_name("libx264");
        if (x264 == nullptr)
        {
            setState("error", "no H.264 encoder: h264_nvenc unavailable and libx264 not in this FFmpeg build");
            return false;
        }
        AVCodecContext* ctx = context(x264, fps);
        AVDictionary* options = nullptr;
        av_dict_set(&options, "preset", "ultrafast", 0);
        av_dict_set(&options, "tune", "zerolatency", 0);
        av_dict_set(&options, "forced-idr", "1", 0);
        av_dict_set(&options, "x264-params", "nal-hrd=cbr", 0);
        int const rc = avcodec_open2(ctx, x264, &options);
        av_dict_free(&options);
        if (rc < 0)
        {
            setState("error", "libx264: " + lastAvError(rc));
            avcodec_free_context(&ctx);
            return false;
        }
        enc = ctx;
        return opened("x264", fps);
    }

    bool opened(char const* name, double fps)
    {
        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_NV12;
        frame->width = size.width;
        frame->height = size.height;
        if (av_frame_get_buffer(frame, 0) < 0)
        {
            closeEncoder();
            setState("error", "no memory for a preview picture");
            return false;
        }
        packet = packet != nullptr ? packet : av_packet_alloc();
        {
            std::lock_guard lock{statusMu};
            status.encoder = name;
            status.width = size.width;
            status.height = size.height;
        }
        logInfo("preview_encoder", {{"encoder", name}, {"width", std::to_string(size.width)}, {"height", std::to_string(size.height)},
                                       {"fps", std::to_string(fps)}, {"bitrate", std::to_string(kBitrate)}});
        return true;
    }

    void closeEncoder()
    {
        avcodec_free_context(&enc);
        av_frame_free(&frame);
    }

    bool openOutput()
    {
        AVFormatContext* ctx = nullptr;
        int rc = avformat_alloc_output_context2(&ctx, nullptr, "rtsp", url.c_str());
        if (rc < 0 || ctx == nullptr)
        {
            setState("error", "rtsp output: " + lastAvError(rc));
            return false;
        }
        AVStream* stream = avformat_new_stream(ctx, nullptr);
        avcodec_parameters_from_context(stream->codecpar, enc);
        stream->time_base = enc->time_base;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        // Socket timeout in microseconds: a stalled MediaMTX holds this thread for 2 s at most.
        av_dict_set(&options, "timeout", "2000000", 0);
        rc = avformat_write_header(ctx, &options);
        av_dict_free(&options);
        if (rc < 0)
        {
            setState("error", lastAvError(rc));
            avformat_free_context(ctx);
            return false;
        }
        out = ctx;
        idr = true;
        return true;
    }

    void closeOutput()
    {
        if (out != nullptr)
        {
            // The RTSP muxer sends TEARDOWN in the trailer, also after an error.
            av_write_trailer(out);
            avformat_free_context(out);
            out = nullptr;
        }
    }

    // Scale, encode and send one picture. False when the encoder or the connection failed.
    bool publish(Snapshot const& shot, std::int64_t pts)
    {
        if (av_frame_make_writable(frame) < 0)
        {
            setState("error", "cannot write a preview picture");
            return false;
        }
        v210ToNv12(shot.v210.data(), v210RowBytes(shot.width), shot.width, shot.height, size, frame->data[0], frame->linesize[0], frame->data[1],
            frame->linesize[1]);
        frame->pts = pts;
        // A new connection starts with an IDR picture.
        frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        idr = false;
        int rc = avcodec_send_frame(enc, frame);
        if (rc < 0)
        {
            setState("error", "encode: " + lastAvError(rc));
            closeOutput();
            closeEncoder();
            return false;
        }
        std::uint64_t sent = 0;
        while ((rc = avcodec_receive_packet(enc, packet)) == 0)
        {
            packet->stream_index = 0;
            av_packet_rescale_ts(packet, enc->time_base, out->streams[0]->time_base);
            rc = av_interleaved_write_frame(out, packet);
            av_packet_unref(packet);
            if (rc < 0)
            {
                setState("error", "rtsp write: " + lastAvError(rc));
                closeOutput();
                return false;
            }
            ++sent;
        }
        if (sent > 0)
        {
            {
                std::lock_guard lock{statusMu};
                status.frames += sent;
            }
            setState("publishing", "");
        }
        return true;
    }

    void loop()
    {
        pthread_setname_np(pthread_self(), "sc-preview");
        // Below the media threads: on a busy core the preview waits, never the output writer.
        setpriority(PRIO_PROCESS, static_cast<id_t>(::gettid()), 10);
        using Clock = std::chrono::steady_clock;
        auto const start = Clock::now();
        auto next = start;
        Clock::time_point retryEncoder{};
        Clock::time_point retryOutput{};
        std::int64_t lastPts = -1;
        for (;;)
        {
            double const fps = std::clamp(fpsNow(), 1.0, 30.0);
            {
                std::lock_guard lock{statusMu};
                status.fps = fps;
            }
            next += std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / fps));
            if (next < Clock::now())
            {
                next = Clock::now();
            }
            {
                std::unique_lock lock{runMu};
                if (wake.wait_until(lock, next, [this] { return stopping; }))
                {
                    break;
                }
            }
            auto const shot = picture();
            if (!shot || shot->v210.size() < v210RowBytes(shot->width) * static_cast<std::size_t>(shot->height))
            {
                continue;
            }
            auto const now = Clock::now();
            if (enc != nullptr && (shot->width != sourceWidth || shot->height != sourceHeight))
            {
                // Another output raster (a live format change): a new encoder and connection.
                closeOutput();
                closeEncoder();
            }
            if (enc == nullptr)
            {
                if (now < retryEncoder)
                {
                    continue;
                }
                sourceWidth = shot->width;
                sourceHeight = shot->height;
                size = previewSize(shot->width, shot->height, previewHeight);
                if (!openEncoder(fps))
                {
                    retryEncoder = now + std::chrono::seconds(5);
                    continue;
                }
            }
            if (out == nullptr && (now < retryOutput || !openOutput()))
            {
                retryOutput = now < retryOutput ? retryOutput : now + std::chrono::seconds(2);
                continue;
            }
            std::int64_t const pts = std::max(lastPts + 1, static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count()));
            lastPts = pts;
            auto const began = Clock::now();
            if (!publish(*shot, pts))
            {
                retryOutput = now + std::chrono::seconds(1);
                continue;
            }
            if (observe)
            {
                observe(std::chrono::duration<double>(Clock::now() - began).count());
            }
        }
        closeOutput();
        closeEncoder();
        av_packet_free(&packet);
    }
};

PreviewPublisher::PreviewPublisher(std::string url, int previewHeight, std::function<std::shared_ptr<Snapshot const>()> picture, std::function<double()> fps)
    : impl_(std::make_unique<Impl>(std::move(url), previewHeight, std::move(picture), std::move(fps)))
{
    static std::once_flag logging;
    std::call_once(logging, [] { av_log_set_callback(avLog); });
}

PreviewPublisher::~PreviewPublisher()
{
    stop();
}

void PreviewPublisher::start(std::function<void(double)> observe)
{
    impl_->observe = std::move(observe);
    impl_->thread = std::thread([this] { impl_->loop(); });
}

void PreviewPublisher::stop()
{
    {
        std::lock_guard lock{impl_->runMu};
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
}

PreviewStatus PreviewPublisher::status() const
{
    std::lock_guard lock{impl_->statusMu};
    return impl_->status;
}
} // namespace scopes
