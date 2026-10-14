#include "mxlio/io.hpp"

#include "app/triplebuffer.hpp"
#include "domain/scan.hpp"
#include "nmos/ids.hpp"
#include "scope/engine.hpp"
#include "scope/scheduler.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace scopes
{
namespace
{
// Longest wait for the next grain before the renderer looks at its state again.
constexpr std::uint64_t kGrainWaitNs = 20'000'000;
// How often the latest picture is copied for GET /api/v1/snapshot.png.
constexpr auto kSnapshotEvery = std::chrono::milliseconds(500);

struct FlowMeta
{
    int width = 0;
    int height = 0;
    int rateNum = 0;
    int rateDen = 1;
    bool interlaced = false;
    std::string mediaType;
    std::string label;
    std::string colorspace;
};

FlowMeta parseFlow(std::string const& body)
{
    FlowMeta meta;
    std::string error;
    auto const root = json::parse(body, error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        return meta;
    }
    auto const& obj = root.get<picojson::object>();
    auto const num = [&](picojson::object const& from, char const* key) {
        auto const it = from.find(key);
        return it != from.end() && it->second.is<double>() ? static_cast<int>(it->second.get<double>()) : 0;
    };
    meta.width = num(obj, "frame_width");
    meta.height = num(obj, "frame_height");
    meta.mediaType = json::fieldString(root, "media_type").value_or("");
    meta.label = json::fieldString(root, "label").value_or("");
    meta.colorspace = json::fieldString(root, "colorspace").value_or("");
    meta.interlaced = json::fieldString(root, "interlace_mode").value_or("").rfind("interlaced", 0) == 0;
    if (auto const rate = obj.find("grain_rate"); rate != obj.end() && rate->second.is<picojson::object>())
    {
        meta.rateNum = num(rate->second.get<picojson::object>(), "numerator");
        meta.rateDen = std::max(1, num(rate->second.get<picojson::object>(), "denominator"));
    }
    return meta;
}

std::string readFlowDef(mxlInstance instance, std::string const& flowId)
{
    std::size_t size = 0;
    mxlGetFlowDef(instance, flowId.c_str(), nullptr, &size);
    if (size == 0)
    {
        size = 16384;
    }
    std::string buffer(size, '\0');
    if (mxlGetFlowDef(instance, flowId.c_str(), buffer.data(), &size) != MXL_STATUS_OK)
    {
        return {};
    }
    buffer.resize(std::strlen(buffer.c_str()));
    return buffer;
}

// Empty when the scope can analyse the flow (receiver caps, SPECIFICATION §6), else why not.
std::string formatProblem(FlowMeta const& meta)
{
    if (meta.mediaType != "video/v210" && meta.mediaType != "video/v210a")
    {
        return "media_type " + (meta.mediaType.empty() ? std::string("missing") : meta.mediaType);
    }
    if (meta.width < 1 || meta.height < 1 || meta.width > 3840 || meta.height > 2160)
    {
        return "raster " + std::to_string(meta.width) + "x" + std::to_string(meta.height);
    }
    double const hz = meta.rateNum > 0 ? static_cast<double>(meta.rateNum) / meta.rateDen : 0;
    if (hz < 23.0 || hz > 60.5)
    {
        return "grain_rate";
    }
    return {};
}

std::string formatText(InputView const& view)
{
    if (view.width <= 0 || view.rateNum <= 0)
    {
        return {};
    }
    double const hz = static_cast<double>(view.rateNum) / view.rateDen;
    double const shown = view.interlaced ? hz * 2 : hz;
    char rate[32];
    if (std::fabs(shown - std::round(shown)) < 0.005)
    {
        std::snprintf(rate, sizeof(rate), "%.0f", shown);
    }
    else
    {
        std::snprintf(rate, sizeof(rate), "%.2f", shown);
    }
    return std::to_string(view.height) + (view.interlaced ? "i" : "p") + rate;
}

std::string stateText(std::string const& state)
{
    if (state == "not_routed")
    {
        return "NOT ROUTED";
    }
    if (state == "waiting")
    {
        return "WAITING";
    }
    if (state == "no_signal")
    {
        return "NO SIGNAL";
    }
    if (state == "format_mismatch")
    {
        return "FORMAT";
    }
    return {};
}

std::string rateLabel(double hz)
{
    if (hz <= 0)
    {
        return "every frame";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%g Hz", hz);
    return text;
}

std::string videoFlowDefinition(std::string const& id, std::string const& description, std::string const& mode, int width, int height, Rate rate)
{
    return std::string("{\"id\":\"") + id + "\",\"format\":\"urn:x-nmos:format:video\",\"label\":\"Scopes Out Video\",\"description\":\"" +
           jsonEscape(description) +
           "\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"Scopes Out:Video\"],\"urn:x-mxl:tag:scope-mode/v1.0\":[\"" + jsonEscape(mode) +
           "\"]},\"parents\":[],\"media_type\":\"video/v210\",\"grain_rate\":{\"numerator\":" + std::to_string(rate.num) + ",\"denominator\":" +
           std::to_string(rate.den) + "},\"frame_width\":" + std::to_string(width) + ",\"frame_height\":" + std::to_string(height) +
           ",\"interlace_mode\":\"progressive\",\"colorspace\":\"BT709\",\"transfer_characteristic\":\"SDR\",\"components\":[{\"name\":\"Y\",\"width\":" +
           std::to_string(width) + ",\"height\":" + std::to_string(height) + ",\"bit_depth\":10},{\"name\":\"Cb\",\"width\":" + std::to_string(width / 2) +
           ",\"height\":" + std::to_string(height) + ",\"bit_depth\":10},{\"name\":\"Cr\",\"width\":" + std::to_string(width / 2) + ",\"height\":" +
           std::to_string(height) + ",\"bit_depth\":10}]}";
}
} // namespace

struct MediaIo::Impl
{
    Config config;
    AppState& state;
    Metrics& metrics;
    SimdLevel level;
    NmosIds ids;
    std::string domainId;
    std::atomic<bool> run{false};
    std::atomic<bool> firstFrame{false};
    TripleBuffer buffers;
    std::thread renderer;
    std::thread writer;
    std::mutex flowMu;
    std::function<void(OutputFlow const&)> onFlow;

    Impl(Config cfg, AppState& stateIn, Metrics& metricsIn, SimdLevel levelIn)
        : config(std::move(cfg))
        , state(stateIn)
        , metrics(metricsIn)
        , level(levelIn)
        , ids(makeNmosIds(config.nmosSeed))
    {
    }

    void ensureDomain()
    {
        if (isMirrorDomain(config.outputDomainDir))
        {
            throw ConfigError("the output domain " + config.outputDomainDir + " is a fabrics mirror");
        }
        std::error_code ec;
        std::filesystem::create_directories(config.outputDomainDir, ec);
        if (ec)
        {
            throw ConfigError("cannot create the output domain " + config.outputDomainDir + ": " + ec.message());
        }
        domainId = config.outputDomainId;
        auto const defPath = std::filesystem::path(config.outputDomainDir) / "domain_def.json";
        if (!std::filesystem::exists(defPath))
        {
            // BCP-007-03: id, label, description and tags.
            std::ofstream out(defPath);
            out << "{\"id\":\"" << domainId << "\",\"label\":\"mxl-scopes\",\"description\":\"Scopes output domain\",\"tags\":{}}\n";
            if (!out.flush())
            {
                throw ConfigError("cannot write " + defPath.string());
            }
        }
        else if (auto const existing = readDomainId(config.outputDomainDir))
        {
            if (*existing != domainId)
            {
                logError("domain_id_mismatch", {{"path", defPath.string()}, {"file", *existing}, {"configured", domainId}});
            }
            domainId = *existing;
        }
        auto const options = std::filesystem::path(config.outputDomainDir) / "options.json";
        if (!std::filesystem::exists(options))
        {
            std::ofstream out(options);
            out << "{\"urn:x-mxl:option:history_duration/v1.0\":" << config.historyNs << "}\n";
        }
    }

    void rendererMain()
    {
        ScopeEngine engine(level);
        UpdateScheduler scheduler;
        mxlInstance instance = nullptr;
        mxlFlowReader reader = nullptr;
        std::string openDomainPath;
        Route openRoute;
        bool firstRoute = true;
        FlowMeta meta;
        std::string problem;
        std::uint32_t grainCount = 0;
        int backoff = 250;
        InputView view;
        bool haveGrain = false;
        std::uint64_t newest = 0;
        std::uint64_t newestOrigin = 0;
        std::string metricState;
        RendererView stats;
        std::string shownKey;
        auto nextSnapshot = std::chrono::steady_clock::now();
        Labels const modeLabel{{"mode", "luma"}};

        auto closeReader = [&] {
            if (reader != nullptr)
            {
                mxlReleaseFlowReader(instance, reader);
                reader = nullptr;
            }
            haveGrain = false;
            problem.clear();
            scheduler.resetInput();
        };
        auto closeAll = [&] {
            closeReader();
            if (instance != nullptr)
            {
                mxlDestroyInstance(instance);
                instance = nullptr;
            }
            openDomainPath.clear();
        };
        auto publishView = [&] {
            if (metricState != view.state)
            {
                for (char const* name : {"not_routed", "waiting", "running", "holding", "no_signal", "format_mismatch"})
                {
                    metrics.set("input_state", {{"state", name}}, view.state == name ? 1 : 0);
                }
                logInfo("input_state", {{"state", view.state}, {"reason", view.reason}, {"flow", view.route.flowId}});
                metricState = view.state;
            }
            state.setInput(view);
        };
        auto caption = [&](DisplaySettings const& s) {
            if (!s.caption)
            {
                return std::string{};
            }
            std::string text = modeTitle(s.mode);
            if (!view.label.empty())
            {
                text += "  |  " + view.label;
            }
            if (auto const format = formatText(view); !format.empty())
            {
                text += "  |  " + format;
            }
            return text + "  |  " + rateLabel(s.updateRate);
        };
        auto snapshot = [&](std::uint8_t const* picture) {
            auto const now = std::chrono::steady_clock::now();
            if (now < nextSnapshot)
            {
                return;
            }
            nextSnapshot = now + kSnapshotEvery;
            auto shot = std::make_shared<Snapshot>();
            shot->width = config.outputWidth;
            shot->height = config.outputHeight;
            shot->update = stats.updates;
            shot->v210.assign(picture, picture + buffers.size());
            state.setSnapshot(std::move(shot));
        };
        // The picture of a state without a trace; drawn again only when something it shows changed.
        auto showState = [&](DisplaySettings const& s) {
            auto const text = stateText(view.state);
            auto const key = text + "\n" + caption(s) + "\n" + std::to_string(s.graticuleLevel) + s.graticuleUnits + (s.limitedRange ? "l" : "f");
            if (key == shownKey && firstFrame.load())
            {
                return;
            }
            engine.render(caption(s), text, buffers.back());
            snapshot(buffers.back());
            buffers.publish();
            firstFrame.store(true);
            shownKey = key;
        };
        auto wait = [&](int ms) {
            for (int slept = 0; slept < ms && run.load(); slept += 10)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };

        while (run.load())
        {
            auto const settingsPtr = state.settings();
            auto const& settings = *settingsPtr;
            auto const route = state.route();
            state.beatRenderer(mxlGetTime());
            if (firstRoute || !(route == openRoute))
            {
                closeReader();
                openRoute = route;
                firstRoute = false;
                backoff = 250;
                view = InputView{.grains = view.grains, .resyncs = view.resyncs};
                view.route = route;
            }
            if (!route.enable || route.flowId.empty() || route.domainId.empty())
            {
                closeAll();
                view.state = "not_routed";
                view.reason.clear();
                publishView();
                engine.configure({}, settings, config.outputWidth, config.outputHeight);
                showState(settings);
                wait(50);
                continue;
            }
            auto const toWaiting = [&](char const* reason) {
                view.state = "waiting";
                view.reason = reason;
                publishView();
                engine.configure({}, settings, config.outputWidth, config.outputHeight);
                showState(settings);
                wait(backoff);
                backoff = std::min(5000, backoff * 2);
            };
            auto const domain = resolveDomain(config.scanPath, route.domainId);
            if (!domain)
            {
                closeAll();
                toWaiting("domain_not_found");
                continue;
            }
            if (instance != nullptr && openDomainPath != domain->path)
            {
                closeAll();
            }
            if (instance == nullptr)
            {
                instance = mxlCreateInstance(domain->path.c_str(), nullptr);
                if (instance == nullptr)
                {
                    toWaiting("domain_not_found");
                    continue;
                }
                openDomainPath = domain->path;
            }
            if (reader == nullptr)
            {
                if (mxlCreateFlowReader(instance, route.flowId.c_str(), nullptr, &reader) != MXL_STATUS_OK)
                {
                    reader = nullptr;
                    toWaiting("flow_not_found");
                    continue;
                }
                backoff = 250;
                meta = parseFlow(readFlowDef(instance, route.flowId));
                mxlFlowInfo info{};
                grainCount = mxlFlowReaderGetInfo(reader, &info) == MXL_STATUS_OK ? info.config.discrete.grainCount : 0;
                problem = formatProblem(meta);
                view.label = meta.label;
                view.mediaType = meta.mediaType;
                view.width = meta.width;
                view.height = meta.height;
                view.rateNum = meta.rateNum;
                view.rateDen = meta.rateDen;
                view.interlaced = meta.interlaced;
                view.colorimetry = meta.colorspace;
                // v1 assumes BT.709 (§4.1): anything else is shown, not refused.
                view.colorimetryWarning = !meta.colorspace.empty() && meta.colorspace != "BT709";
                logInfo("input_opened", {{"flow", route.flowId}, {"media_type", meta.mediaType}, {"format", formatText(view)}, {"problem", problem}});
            }
            if (!problem.empty())
            {
                view.state = "format_mismatch";
                view.reason = problem;
                publishView();
                engine.configure({}, settings, config.outputWidth, config.outputHeight);
                showState(settings);
                wait(200);
                continue;
            }
            engine.configure({meta.width, meta.height, v210RowBytes(meta.width)}, settings, config.outputWidth, config.outputHeight);
            scheduler.configure(settings.updateRate, static_cast<double>(meta.rateNum) / meta.rateDen);

            mxlFlowInfo info{};
            if (mxlFlowReaderGetInfo(reader, &info) != MXL_STATUS_OK)
            {
                closeReader();
                toWaiting("flow_not_found");
                continue;
            }
            mxlRational const rate{meta.rateNum, meta.rateDen};
            auto const head = info.runtime.headIndex;
            mxlGrainInfo grain{};
            std::uint8_t* payload = nullptr;
            mxlStatus status = MXL_STATUS_OK;
            if (haveGrain && newest >= head)
            {
                // Caught up: wait for the next grain, at most until the next update tick.
                std::uint64_t timeout = kGrainWaitNs;
                auto const now = mxlGetTime();
                if (auto const tick = scheduler.nextTickNs(); tick > now)
                {
                    timeout = std::clamp<std::uint64_t>(tick - now, 1'000'000, kGrainWaitNs);
                }
                status = mxlFlowReaderGetGrain(reader, newest + 1, timeout, &grain, &payload);
            }
            else
            {
                status = mxlFlowReaderGetGrainNonBlocking(reader, head, &grain, &payload);
            }
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
            {
                ++view.resyncs;
                metrics.inc("input_resyncs_total", {});
                status = mxlFlowReaderGetGrainNonBlocking(reader, head, &grain, &payload);
            }
            if (status == MXL_ERR_FLOW_INVALID)
            {
                // The writer re-created the flow: open it again. The last picture holds.
                ++view.resyncs;
                metrics.inc("input_resyncs_total", {});
                closeReader();
                continue;
            }
            auto const now = mxlGetTime();
            if (status == MXL_STATUS_OK && payload != nullptr && (grain.flags & MXL_GRAIN_FLAG_INVALID) == 0 && (!haveGrain || grain.index > newest))
            {
                newest = grain.index;
                newestOrigin = mxlIndexToTimestamp(&rate, grain.index);
                haveGrain = true;
                ++view.grains;
                metrics.inc("input_grains_total", {});
                view.state = "running";
                view.reason.clear();
                view.latencyMs = now > newestOrigin ? static_cast<double>(now - newestOrigin) / 1e6 : 0;
            }
            else if (!haveGrain)
            {
                view.state = "waiting";
                view.reason = "no_grain";
            }
            else
            {
                // §11: the last picture holds for SCOPES_HOLD_MS, then NO SIGNAL.
                auto const frameNs = static_cast<std::uint64_t>(1e9 * meta.rateDen / std::max(1, meta.rateNum));
                if (now > newestOrigin + static_cast<std::uint64_t>(settings.holdMs) * 1'000'000ull)
                {
                    view.state = "no_signal";
                }
                else if (now > newestOrigin + 3 * frameNs)
                {
                    view.state = "holding";
                }
            }
            publishView();

            bool const live = haveGrain && (view.state == "running" || view.state == "holding");
            std::uint64_t target = newest;
            if (live && settings.inputOffsetGrains > 0 && grainCount > 2)
            {
                // §2.2: the grain SCOPES_INPUT_OFFSET_GRAINS behind the newest, if still in the ring.
                auto const back = std::min<std::uint64_t>(static_cast<std::uint64_t>(settings.inputOffsetGrains), grainCount - 2);
                target = newest >= back ? newest - back : newest;
            }
            auto const decision = scheduler.poll(now, live, target);
            if (decision.skipped > 0)
            {
                stats.skipped += decision.skipped;
                metrics.inc("updates_skipped_total", {}, static_cast<double>(decision.skipped));
            }
            if (decision.update)
            {
                mxlGrainInfo analysed{};
                std::uint8_t* picture = nullptr;
                if (target == grain.index && payload != nullptr && status == MXL_STATUS_OK)
                {
                    analysed = grain;
                    picture = payload;
                }
                else if (mxlFlowReaderGetGrainNonBlocking(reader, target, &analysed, &picture) != MXL_STATUS_OK)
                {
                    picture = nullptr;
                }
                if (picture != nullptr && (analysed.flags & MXL_GRAIN_FLAG_INVALID) == 0)
                {
                    auto const t0 = std::chrono::steady_clock::now();
                    engine.analyse(picture);
                    auto const t1 = std::chrono::steady_clock::now();
                    // §2.2: analysed in place, so check the writer did not reach this slot meanwhile.
                    mxlFlowRuntimeInfo runtime{};
                    bool const torn = grainCount > 0 && mxlFlowReaderGetRuntimeInfo(reader, &runtime) == MXL_STATUS_OK &&
                                      runtime.headIndex + 1 >= target + grainCount;
                    if (torn)
                    {
                        ++stats.torn;
                        metrics.inc("updates_torn_total", {});
                        engine.clear();
                    }
                    else
                    {
                        engine.render(caption(settings), "", buffers.back());
                        auto const t2 = std::chrono::steady_clock::now();
                        snapshot(buffers.back());
                        buffers.publish();
                        firstFrame.store(true);
                        shownKey.clear();
                        ++stats.updates;
                        stats.lastIndex = target;
                        stats.analyseMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                        stats.renderMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
                        metrics.inc("updates_total", modeLabel);
                        metrics.observe("analyse_seconds", modeLabel, std::chrono::duration<double>(t1 - t0).count());
                        metrics.observe("render_seconds", modeLabel, std::chrono::duration<double>(t2 - t1).count());
                    }
                }
            }
            else if (!live)
            {
                showState(settings);
            }
            stats.updateHz = scheduler.effectiveHz();
            metrics.set("update_rate_hz", {}, stats.updateHz);
            state.setRenderer(stats);
        }
        closeAll();
    }

    [[nodiscard]] Rate desiredRate(Rate current) const
    {
        if (config.outputRate)
        {
            return *config.outputRate;
        }
        // SCOPES_OUTPUT_RATE=input: the routed flow's rate once known (§7).
        auto const in = state.input();
        if (in.rateNum > 0 && in.state != "format_mismatch")
        {
            Rate rate{in.rateNum, in.rateDen};
            if (rate.hz() >= 23.0 && rate.hz() <= 60.5)
            {
                return rate;
            }
        }
        return current;
    }

    void writerMain()
    {
        auto* instance = mxlCreateInstance(config.outputDomainDir.c_str(), nullptr);
        if (instance == nullptr)
        {
            logError("output_domain_open_failed", {{"path", config.outputDomainDir}});
            return;
        }
        mxlGarbageCollectFlows(instance);
        for (int i = 0; i < 200 && !firstFrame.load() && run.load(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        mxlFlowWriter writer = nullptr;
        Rate rate = config.outputRate.value_or(Rate{50, 1});
        rate = desiredRate(rate);
        OutputView view;
        view.domainId = domainId;
        view.width = config.outputWidth;
        view.height = config.outputHeight;
        auto open = [&] {
            if (writer != nullptr)
            {
                mxlReleaseFlowWriter(instance, writer);
                writer = nullptr;
            }
            auto const token = std::to_string(config.outputWidth) + "x" + std::to_string(config.outputHeight) + "p" + rateToken(rate);
            auto const flowId = ids.flow(token);
            auto const description = modeTitle(state.settings()->mode);
            auto const def = videoFlowDefinition(flowId, description, state.settings()->mode, config.outputWidth, config.outputHeight, rate);
            if (mxlCreateFlowWriter(instance, def.c_str(), "{\"maxCommitBatchSizeHint\":1}", &writer, nullptr, nullptr) != MXL_STATUS_OK)
            {
                writer = nullptr;
                logError("output_flow_failed", {{"flow", flowId}});
            }
            view.flowId = flowId;
            view.rateNum = rate.num;
            view.rateDen = rate.den;
            view.writing = writer != nullptr;
            state.setOutput(view);
            logInfo("output_flow", {{"flow", flowId}, {"format", token}});
            std::function<void(OutputFlow const&)> callback;
            {
                std::lock_guard lock{flowMu};
                callback = onFlow;
            }
            if (callback && writer != nullptr)
            {
                callback(OutputFlow{flowId, config.outputWidth, config.outputHeight, rate, description});
            }
        };
        open();
        mxlRational mxlRate{rate.num, rate.den};
        std::uint64_t index = mxlGetCurrentIndex(&mxlRate);
        while (run.load())
        {
            state.beatWriter(mxlGetTime());
            if (auto const wanted = desiredRate(rate); !(wanted == rate))
            {
                // A route to a flow of another rate: a new output flow, a new flow id (§7).
                rate = wanted;
                open();
                mxlRate = mxlRational{rate.num, rate.den};
                index = mxlGetCurrentIndex(&mxlRate);
            }
            auto const nowIndex = mxlGetCurrentIndex(&mxlRate);
            if (nowIndex != MXL_UNDEFINED_INDEX && nowIndex > index)
            {
                auto const skipped = nowIndex - index;
                view.missed += skipped;
                view.late += skipped;
                metrics.inc("output_frames_missed_total", {}, static_cast<double>(skipped));
                metrics.inc("output_frames_late_total", {}, static_cast<double>(skipped));
                index = nowIndex;
            }
            auto const when = mxlIndexToTimestamp(&mxlRate, index);
            if (mxlGetTime() + 1'000'000 < when)
            {
                mxlSleepUntil(when);
            }
            if (writer != nullptr)
            {
                mxlGrainInfo grain{};
                std::uint8_t* payload = nullptr;
                if (mxlFlowWriterOpenGrain(writer, index, &grain, &payload) == MXL_STATUS_OK && payload != nullptr)
                {
                    auto const* picture = buffers.front();
                    std::memcpy(payload, picture, std::min<std::size_t>(grain.grainSize, buffers.size()));
                    grain.validSlices = grain.totalSlices;
                    grain.flags = 0;
                    mxlFlowWriterCommitGrain(writer, &grain);
                    ++view.frames;
                    metrics.inc("output_frames_total", {});
                }
                else
                {
                    ++view.missed;
                    metrics.inc("output_frames_missed_total", {});
                }
                if (mxlGetTime() > mxlIndexToTimestamp(&mxlRate, index + 1))
                {
                    ++view.late;
                    metrics.inc("output_frames_late_total", {});
                }
            }
            else
            {
                // No flow: try again once a second.
                std::this_thread::sleep_for(std::chrono::seconds(1));
                open();
                index = mxlGetCurrentIndex(&mxlRate);
                continue;
            }
            state.setOutput(view);
            ++index;
        }
        if (writer != nullptr)
        {
            mxlReleaseFlowWriter(instance, writer);
        }
        mxlDestroyInstance(instance);
    }
};

MediaIo::MediaIo(Config config, AppState& state, Metrics& metrics, SimdLevel level)
    : impl_(std::make_unique<Impl>(std::move(config), state, metrics, level))
{
}

MediaIo::~MediaIo()
{
    stop();
}

void MediaIo::start()
{
    impl_->ensureDomain();
    impl_->buffers.allocate(v210RowBytes(impl_->config.outputWidth) * static_cast<std::size_t>(impl_->config.outputHeight));
    impl_->run.store(true);
    impl_->renderer = std::thread([this] { impl_->rendererMain(); });
    impl_->writer = std::thread([this] { impl_->writerMain(); });
}

void MediaIo::stop()
{
    impl_->run.store(false);
    if (impl_->renderer.joinable())
    {
        impl_->renderer.join();
    }
    if (impl_->writer.joinable())
    {
        impl_->writer.join();
    }
}

void MediaIo::removeOwnDomain()
{
    auto const& config = impl_->config;
    if (config.outputDomainDir.empty() || isMirrorDomain(config.outputDomainDir))
    {
        return;
    }
    std::error_code ec;
    auto const output = std::filesystem::weakly_canonical(config.outputDomainDir, ec);
    auto const scan = std::filesystem::weakly_canonical(config.scanPath, ec);
    if (output.empty() || output == scan || output == output.root_path())
    {
        logError("domain_cleanup_refused", {{"path", config.outputDomainDir}});
        return;
    }
    std::filesystem::remove_all(config.outputDomainDir, ec);
    if (ec)
    {
        logError("domain_cleanup_failed", {{"path", config.outputDomainDir}, {"error", ec.message()}});
        return;
    }
    logInfo("domain_removed", {{"path", config.outputDomainDir}});
}

std::string MediaIo::domainId() const
{
    return impl_->domainId;
}

void MediaIo::setFlowCallback(std::function<void(OutputFlow const&)> callback)
{
    std::lock_guard lock{impl_->flowMu};
    impl_->onFlow = std::move(callback);
}
} // namespace scopes
