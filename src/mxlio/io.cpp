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
#include <ctime>
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

std::uint64_t threadCpuNs()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::string formatText(InputView const& view)
{
    if (view.width <= 0 || view.rateNum <= 0)
    {
        return {};
    }
    double const hz = static_cast<double>(view.rateNum) / view.rateDen;
    // The grain rate: the field rate of an interlaced flow (MXL grains are fields), so 1080i50.
    double const shown = hz;
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
    // True when this process wrote the domain's domain_def.json (MXL_CLEANUP_ON_EXIT).
    bool createdDomain = false;
    std::atomic<bool> run{false};
    std::atomic<bool> firstFrame{false};
    // One picture on its way from the renderer to the writer: the still picture (background,
    // graticule, texts) and the packed groups that carry the trace (§2.4). The renderer computes
    // the groups right after the analyse step; the writer only writes the groups that differ
    // into each output grain, so a slow update skips ticks and never delays an output grain.
    struct Job
    {
        std::shared_ptr<std::vector<std::uint8_t> const> still;
        std::uint64_t stillVersion = 0;
        std::vector<TraceGroup> trace;
        // The mode the picture shows (for /api/v1/output).
        std::string mode;
    };
    TripleSlot<Job> jobs;
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
            createdDomain = true;
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
        ScopeEngine engine(level, kDefaultStripColumns, config.threads);
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
        // The last grain index fetched, valid or not: the next read waits for cursor + 1, so an
        // invalid or partly written head grain never makes the loop spin.
        bool haveCursor = false;
        std::uint64_t cursor = 0;
        // Lines of one grain (half the frame for an interlaced flow) and grains per update.
        int grainRows = 0;
        int fields = 1;
        std::uint64_t newest = 0;
        std::uint64_t newestOrigin = 0;
        std::string metricState;
        RendererView stats;
        std::string shownKey;
        Labels modeLabel{{"mode", "luma"}};
        // SCOPES_ACCUMULATE=all: grains added into the back job since the last update, and the
        // analyse time they took.
        int accGrains = 0;
        // The settings or the format changed: draw the new picture without waiting for a tick.
        bool redraw = false;
        double accWallMs = 0;
        double accCpuMs = 0;

        // The reader goes; what was last seen on this route stays, so a flow that disappears
        // holds and then shows NO SIGNAL like one that stops (§11).
        auto closeReader = [&] {
            if (reader != nullptr)
            {
                mxlReleaseFlowReader(instance, reader);
                reader = nullptr;
            }
            problem.clear();
            haveCursor = false;
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
            double const inputHz = view.rateDen > 0 ? static_cast<double>(view.rateNum) / view.rateDen / (view.interlaced ? 2 : 1) : 0;
            return text + "  |  " + rateLabel(modeUpdateRate(s.mode, s.updateRate, inputHz));
        };
        // Hands a picture to the writer: the trace of the analysed counts, or a state picture
        // (no trace). Clears the counts.
        Measurements publishedMeasurements;
        auto publishJob = [&](std::string const& captionText, std::string const& stateText) {
            redraw = false;
            accGrains = 0;
            accWallMs = 0;
            accCpuMs = 0;
            auto& job = jobs.back();
            auto const t0 = std::chrono::steady_clock::now();
            engine.trace(captionText, stateText, job.trace);
            auto const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            job.still = engine.stillShared();
            job.stillVersion = engine.stillVersion();
            job.mode = engine.paintParams().settings.mode;
            jobs.publish();
            firstFrame.store(true);
            if (stateText.empty())
            {
                stats.renderMs = seconds * 1000.0;
                metrics.observe("render_seconds", modeLabel, seconds);
            }
            // §4.7: the measurements of this update; none while a state text shows.
            Measurements measured = engine.measurements();
            if (!stateText.empty())
            {
                measured = Measurements{};
                measured.update = engine.measurements().update;
                measured.mode = engine.paintParams().settings.mode;
            }
            if (!(measured == publishedMeasurements))
            {
                publishedMeasurements = measured;
                state.setMeasurements(measured);
                auto const gauge = [&](char const* name, auto const& value) {
                    if (value)
                    {
                        metrics.set(name, {}, static_cast<double>(*value));
                    }
                    else
                    {
                        metrics.remove(name, {});
                    }
                };
                gauge("luma_min_code", measured.lumaMin);
                gauge("luma_max_code", measured.lumaMax);
                gauge("luma_mean_code", measured.lumaMean);
                gauge("luma_below_legal_ratio", measured.lumaBelow);
                gauge("luma_above_legal_ratio", measured.lumaAbove);
                gauge("chroma_out_of_range_ratio", measured.chromaOut);
                gauge("rgb_out_of_gamut_ratio", measured.rgbOutOfGamut);
            }
        };
        // The picture of a state without a trace; drawn again only when something it shows changed.
        auto showState = [&](DisplaySettings const& s) {
            auto const text = stateText(view.state);
            auto const key = text + "\n" + caption(s) + "\n" + s.mode + "\n" + std::to_string(s.graticuleLevel) + s.graticuleUnits + (s.limitedRange ? "l" : "f");
            if (key == shownKey && firstFrame.load())
            {
                return;
            }
            publishJob(caption(s), text);
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
            if (modeLabel[0].second != settings.mode)
            {
                modeLabel = Labels{{"mode", settings.mode}};
                metrics.inc("updates_total", modeLabel, 0);
            }
            if (firstRoute || !(route == openRoute))
            {
                closeReader();
                openRoute = route;
                firstRoute = false;
                backoff = 250;
                view = InputView{.grains = view.grains, .resyncs = view.resyncs};
                view.route = route;
                haveGrain = false;
                scheduler.resetInput();
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
            // §11: the last picture holds for SCOPES_HOLD_MS after the newest grain, then NO SIGNAL.
            auto const staleState = [&](std::uint64_t now) {
                auto const frameNs = meta.rateNum > 0 ? static_cast<std::uint64_t>(1e9 * meta.rateDen / meta.rateNum) : 40'000'000ull;
                if (now > newestOrigin + static_cast<std::uint64_t>(settings.holdMs) * 1'000'000ull)
                {
                    view.state = "no_signal";
                }
                else if (now > newestOrigin + 3 * frameNs)
                {
                    view.state = "holding";
                }
            };
            auto const toWaiting = [&](char const* reason) {
                if (haveGrain)
                {
                    // The flow was running on this route: it went away, it is not missing yet.
                    staleState(mxlGetTime());
                    view.reason = reason;
                    publishView();
                    if (view.state == "no_signal")
                    {
                        showState(settings);
                    }
                    wait(std::min(backoff, 250));
                    return;
                }
                view.state = "waiting";
                view.reason = reason;
                publishView();
                engine.configure({}, settings, config.outputWidth, config.outputHeight);
                showState(settings);
                wait(backoff);
                backoff = std::min(5000, backoff * 2);
            };
            // The domain scan reads every domain_def.json: only while no reader is open.
            if (reader == nullptr)
            {
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
                grainCount = 0;
                if (mxlFlowReaderGetInfo(reader, &info) == MXL_STATUS_OK)
                {
                    grainCount = info.config.discrete.grainCount;
                    // MXL indexes grains at its own rate: the field rate of an interlaced flow
                    // (twice the flow_def grain_rate, FlowParser at 218ddaa).
                    if (info.config.common.grainRate.numerator > 0 && info.config.common.grainRate.denominator > 0)
                    {
                        meta.rateNum = static_cast<int>(info.config.common.grainRate.numerator);
                        meta.rateDen = static_cast<int>(info.config.common.grainRate.denominator);
                    }
                }
                problem = formatProblem(meta);
                if (problem.empty() && meta.interlaced && meta.height % 2 != 0)
                {
                    problem = "interlaced height " + std::to_string(meta.height) + " is odd";
                }
                // An interlaced grain is one field: half the lines (§4.1); an update takes both.
                grainRows = meta.interlaced ? meta.height / 2 : meta.height;
                fields = meta.interlaced ? 2 : 1;
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
                // Open the flow again in a second: a writer may re-create it in a format that fits.
                closeReader();
                wait(1000);
                continue;
            }
            // The input's frame rate: for an interlaced flow two field grains make one frame.
            double const frameHz = static_cast<double>(meta.rateNum) / meta.rateDen / fields;
            double const updateRate = modeUpdateRate(settings.mode, settings.updateRate, frameHz);
            scheduler.configure(updateRate, frameHz, static_cast<std::uint64_t>(fields));
            // §4.3: `all` adds every frame between two updates: room for input rate / update rate
            // frames, rounded up, and one more for jitter.
            bool const accumulateAll = settings.accumulate == "all" && !scheduler.frameMode();
            int const framesCapacity = accumulateAll ? static_cast<int>(std::ceil(frameHz / updateRate)) + 1 : 1;
            try
            {
                if (engine.configure({meta.width, grainRows, v210RowBytes(meta.width), fields, framesCapacity}, settings, config.outputWidth, config.outputHeight))
                {
                    // New tables (the counts start again from zero); draw the new picture at once,
                    // also while the input holds (a mode change without new grains).
                    accGrains = 0;
                    accWallMs = 0;
                    accCpuMs = 0;
                    redraw = true;
                    stats.lineStep = engine.lineStep();
                }
            }
            catch (std::exception const& ex)
            {
                // Never terminate on an input the tables cannot take: show FORMAT instead.
                logError("analyse_configure_failed", {{"error", ex.what()}, {"flow", route.flowId}});
                view.state = "format_mismatch";
                view.reason = std::string("analyse: ") + ex.what();
                publishView();
                engine.configure({}, settings, config.outputWidth, config.outputHeight);
                showState(settings);
                closeReader();
                wait(1000);
                continue;
            }
            auto const grainBytes = v210RowBytes(meta.width) * static_cast<std::size_t>(grainRows);

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
            // Wait for the grain after the last one fetched; jump to the head only when behind.
            // A blocking read with a timeout, so a head grain that is invalid or not yet complete
            // never makes this loop spin. `all` reads every grain while it is still in the ring.
            bool const inRing = haveCursor && cursor + 1 < head && head - (cursor + 1) + 2 < grainCount;
            std::uint64_t const want = accumulateAll && inRing ? cursor + 1 : (!haveCursor || cursor + 1 < head ? head : cursor + 1);
            if (accumulateAll && haveCursor && want == head && cursor + 1 < head)
            {
                metrics.inc("accumulate_dropped_total", {}, static_cast<double>(head - cursor - 1));
            }
            std::uint64_t timeout = kGrainWaitNs;
            if (auto const tick = scheduler.nextTickNs(), now = mxlGetTime(); tick > now)
            {
                timeout = std::clamp<std::uint64_t>(tick - now, 1'000'000, kGrainWaitNs);
            }
            status = mxlFlowReaderGetGrain(reader, want, timeout, &grain, &payload);
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
            {
                ++view.resyncs;
                metrics.inc("input_resyncs_total", {});
                haveCursor = false;
                continue;
            }
            if (status == MXL_STATUS_OK)
            {
                cursor = grain.index;
                haveCursor = true;
            }
            else if (status != MXL_ERR_TIMEOUT && status != MXL_ERR_FLOW_INVALID)
            {
                // Any other answer (too early, ...) comes back at once: do not spin on it.
                wait(5);
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
            bool fresh = false;
            if (status == MXL_STATUS_OK && payload != nullptr && (grain.flags & MXL_GRAIN_FLAG_INVALID) == 0 && grain.grainSize >= grainBytes &&
                (!haveGrain || grain.index > newest))
            {
                fresh = true;
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
                staleState(now);
            }
            publishView();

            bool const live = haveGrain && (view.state == "running" || view.state == "holding");
            // Checks that the writer did not reach the analysed slots meanwhile (§2.2): the head
            // far enough behind, and each grain still the one read.
            auto const fetch = [&](std::uint64_t index, std::uint8_t** picture) {
                mxlGrainInfo info{};
                if (mxlFlowReaderGetGrainNonBlocking(reader, index, &info, picture) != MXL_STATUS_OK || *picture == nullptr ||
                    (info.flags & MXL_GRAIN_FLAG_INVALID) != 0 || info.index != index || info.grainSize < grainBytes)
                {
                    *picture = nullptr;
                }
            };
            auto const tornRead = [&](std::uint64_t const* indexes, std::uint8_t* const* pictures, int count) {
                mxlFlowRuntimeInfo runtime{};
                auto const oldest = *std::min_element(indexes, indexes + count);
                if (grainCount > 0 && mxlFlowReaderGetRuntimeInfo(reader, &runtime) == MXL_STATUS_OK && runtime.headIndex + 1 >= oldest + grainCount)
                {
                    return true;
                }
                for (int f = 0; f < count; ++f)
                {
                    std::uint8_t* again = nullptr;
                    fetch(indexes[f], &again);
                    if (again != pictures[f])
                    {
                        return true;
                    }
                }
                return false;
            };
            if (accumulateAll && fresh && live && accGrains >= engine.maxGrains())
            {
                // More grains than the cells were sized for (jitter beyond the capacity): not added.
                metrics.inc("accumulate_dropped_total", {});
            }
            else if (accumulateAll && fresh && live)
            {
                // `all`: every new grain (each field of an interlaced flow) goes into the counts
                // of the next update as it arrives.
                auto const c0 = threadCpuNs();
                auto const t0 = std::chrono::steady_clock::now();
                engine.analyse(payload);
                auto const t1 = std::chrono::steady_clock::now();
                auto const c1 = threadCpuNs();
                std::uint64_t const index = grain.index;
                std::uint8_t* const picture = payload;
                if (tornRead(&index, &picture, 1))
                {
                    // The counts of this update are lost; the next update starts again.
                    ++stats.torn;
                    metrics.inc("updates_torn_total", {});
                    engine.clear();
                    accGrains = 0;
                    accWallMs = 0;
                    accCpuMs = 0;
                }
                else
                {
                    ++accGrains;
                    accWallMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
                    accCpuMs += static_cast<double>(c1 - c0) / 1e6;
                }
            }
            std::uint64_t target = newest;
            if (live && settings.inputOffsetGrains > 0 && grainCount > 2)
            {
                // §2.2: the grain SCOPES_INPUT_OFFSET_GRAINS behind the newest, if still in the ring.
                auto const back = std::min<std::uint64_t>(static_cast<std::uint64_t>(settings.inputOffsetGrains), grainCount - 2);
                target = newest >= back ? newest - back : newest;
            }
            if (fields == 2 && target % 2 == 0 && target > 0)
            {
                // Interlaced: an update is a whole frame, fields (2k, 2k + 1); the newest complete
                // frame ends on an odd field. So `frame` updates once per frame and no field is
                // analysed twice.
                --target;
            }
            auto const decision = scheduler.poll(now, live, target);
            if (decision.skipped > 0)
            {
                stats.skipped += decision.skipped;
                metrics.inc("updates_skipped_total", {}, static_cast<double>(decision.skipped));
            }
            if ((decision.update || redraw) && accumulateAll && accGrains > 0)
            {
                // `all`: the grains since the last update are in the counts already. A redraw
                // after a settings change shows them at once instead of analysing the newest
                // grain a second time.
                ++stats.updates;
                stats.analyseMs = accWallMs;
                stats.analyseCpuMs = accCpuMs;
                stats.lastIndex = newest;
                metrics.inc("updates_total", modeLabel);
                metrics.observe("analyse_seconds", modeLabel, accWallMs / 1e3);
                metrics.observe("analyse_cpu_seconds", modeLabel, accCpuMs / 1e3);
                publishJob(caption(settings), "");
                shownKey.clear();
            }
            else if (decision.update || (redraw && live))
            {
                // The grains of this update: the target, and for an interlaced flow the field
                // before it as well (both fields of a frame). Each is checked for its size.
                std::uint8_t* pictures[2] = {nullptr, nullptr};
                std::uint64_t indexes[2] = {target, target};
                fetch(target, &pictures[0]);
                if (fields == 2)
                {
                    indexes[1] = target > 0 ? target - 1 : target;
                    fetch(indexes[1], &pictures[1]);
                    if (pictures[1] == nullptr)
                    {
                        // No earlier field in the ring: the one field counts twice.
                        pictures[1] = pictures[0];
                        indexes[1] = target;
                    }
                }
                if (pictures[0] == nullptr && redraw)
                {
                    // The newest grain left the ring: the new settings' picture without a trace.
                    engine.clear();
                    publishJob(caption(settings), "");
                }
                if (pictures[0] != nullptr)
                {
                    auto const c0 = threadCpuNs();
                    auto const t0 = std::chrono::steady_clock::now();
                    for (int f = 0; f < fields; ++f)
                    {
                        engine.analyse(pictures[f]);
                    }
                    auto const t1 = std::chrono::steady_clock::now();
                    auto const c1 = threadCpuNs();
                    // §2.2: analysed in place, so check the writer did not reach these slots.
                    if (tornRead(indexes, pictures, fields))
                    {
                        ++stats.torn;
                        metrics.inc("updates_torn_total", {});
                        engine.clear();
                        accGrains = 0;
                        accWallMs = 0;
                        accCpuMs = 0;
                    }
                    else
                    {
                        // The trace groups go to the writer, which writes them into its next
                        // output grain (§2.4). (An `all` update that found no new grain since the
                        // last one analyses the newest frame, like `latest`.)
                        ++stats.updates;
                        stats.lastIndex = target;
                        stats.analyseMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
                        stats.analyseCpuMs = static_cast<double>(c1 - c0) / 1e6;
                        metrics.inc("updates_total", modeLabel);
                        metrics.observe("analyse_seconds", modeLabel, std::chrono::duration<double>(t1 - t0).count());
                        metrics.observe("analyse_cpu_seconds", modeLabel, static_cast<double>(c1 - c0) / 1e9);
                        publishJob(caption(settings), "");
                        shownKey.clear();
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
        std::uint32_t grainCount = 0;
        // The still picture of the newest job and its version.
        std::shared_ptr<std::vector<std::uint8_t> const> still;
        std::uint64_t stillVersion = 0;
        // The mode the NMOS flow describes; a live mode change keeps the flow id (§7.1).
        std::string announcedMode = state.settings()->mode;
        // The picture is the still picture plus the trace groups of the newest update. Each ring
        // slot remembers which still picture and which trace groups it holds, so writing a grain
        // touches only the groups that differ (§2.4: no render buffer, no full copy).
        std::vector<TraceGroup> current;
        // Bumped whenever the trace groups change; equal traces (a still input) keep it.
        std::uint64_t generation = 1;
        struct SlotRecord
        {
            std::uint64_t still = 0;
            std::uint64_t generation = 0;
            std::vector<std::uint32_t> offsets;
        };
        std::vector<SlotRecord> slots;
        std::uint8_t const* previous = nullptr;
        std::uint64_t previousIndex = 0;
        auto nextSnapshot = std::chrono::steady_clock::now();
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
            announcedMode = state.settings()->mode;
            auto const description = modeTitle(announcedMode);
            auto const def = videoFlowDefinition(flowId, description, announcedMode, config.outputWidth, config.outputHeight, rate);
            mxlFlowConfigInfo flowConfig{};
            if (mxlCreateFlowWriter(instance, def.c_str(), "{\"maxCommitBatchSizeHint\":1}", &writer, &flowConfig, nullptr) != MXL_STATUS_OK)
            {
                writer = nullptr;
                logError("output_flow_failed", {{"flow", flowId}});
            }
            grainCount = writer != nullptr ? flowConfig.discrete.grainCount : 0;
            // A new flow: nothing is known about the contents of its slots.
            slots.assign(grainCount, SlotRecord{});
            previous = nullptr;
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
                callback(OutputFlow{flowId, config.outputWidth, config.outputHeight, rate, description, announcedMode});
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
                auto const pictureBytes = v210RowBytes(config.outputWidth) * static_cast<std::size_t>(config.outputHeight);
                if (mxlFlowWriterOpenGrain(writer, index, &grain, &payload) == MXL_STATUS_OK && payload != nullptr && grain.grainSize >= pictureBytes)
                {
                    if (auto* job = jobs.take(); job != nullptr)
                    {
                        bool const same = job->trace.size() == current.size() &&
                                          (current.empty() || std::memcmp(job->trace.data(), current.data(), current.size() * sizeof(TraceGroup)) == 0);
                        if (!same)
                        {
                            current.swap(job->trace);
                            ++generation;
                        }
                        still = job->still;
                        stillVersion = job->stillVersion;
                        view.mode = job->mode;
                    }
                    if (still != nullptr && still->size() == pictureBytes)
                    {
                        SlotRecord fresh;
                        auto& slot = grainCount > 0 ? slots[index % grainCount] : fresh;
                        auto const version = stillVersion;
                        auto const deltaBytes = (slot.offsets.size() + current.size()) * 16u;
                        bool const previousCurrent = previous != nullptr && index > previousIndex && index - previousIndex < grainCount &&
                                                     slots[previousIndex % grainCount].still == version &&
                                                     slots[previousIndex % grainCount].generation == generation;
                        if (slot.still == version && slot.generation == generation)
                        {
                            // The slot holds this very picture already (a still input, no signal).
                        }
                        else if (slot.still == version && deltaBytes * 4 < pictureBytes)
                        {
                            // Undo the trace this slot got last time, then draw the current one.
                            for (auto const offset : slot.offsets)
                            {
                                std::memcpy(payload + offset, still->data() + offset, 16);
                            }
                            applyTrace(current, payload);
                        }
                        else if (previousCurrent)
                        {
                            // A large trace (a noisy picture): one copy of the previous grain.
                            std::memcpy(payload, previous, pictureBytes);
                        }
                        else
                        {
                            std::memcpy(payload, still->data(), pictureBytes);
                            applyTrace(current, payload);
                        }
                        slot.still = version;
                        slot.generation = generation;
                        slot.offsets.resize(current.size());
                        for (std::size_t i = 0; i < current.size(); ++i)
                        {
                            slot.offsets[i] = current[i].offset;
                        }
                    }
                    else
                    {
                        std::memset(payload, 0, pictureBytes);
                    }
                    previous = payload;
                    previousIndex = index;
                    auto const now = std::chrono::steady_clock::now();
                    if (now >= nextSnapshot)
                    {
                        nextSnapshot = now + kSnapshotEvery;
                        auto shot = std::make_shared<Snapshot>();
                        shot->width = config.outputWidth;
                        shot->height = config.outputHeight;
                        shot->v210.assign(payload, payload + pictureBytes);
                        state.setSnapshot(std::move(shot));
                    }
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
            if (auto const mode = state.settings()->mode; mode != announcedMode && writer != nullptr)
            {
                // The MXL flow_def.json keeps the description it was created with; NMOS follows.
                announcedMode = mode;
                std::function<void(OutputFlow const&)> callback;
                {
                    std::lock_guard lock{flowMu};
                    callback = onFlow;
                }
                if (callback)
                {
                    callback(OutputFlow{view.flowId, config.outputWidth, config.outputHeight, rate, modeTitle(mode), mode});
                }
            }
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
    // Only a domain this process created, or one an earlier mxl-scopes run with this seed
    // created (our id and label): never another function's domain the setting points at.
    bool ours = impl_->createdDomain;
    if (!ours)
    {
        std::ifstream in(std::filesystem::path(config.outputDomainDir) / "domain_def.json");
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        auto const root = json::parse(body, error);
        ours = error.empty() && json::fieldString(root, "id").value_or("") == config.outputDomainId &&
               json::fieldString(root, "label").value_or("") == "mxl-scopes";
    }
    if (!ours)
    {
        logWarn("domain_cleanup_skipped", {{"path", config.outputDomainDir}, {"reason", "not created by mxl-scopes with this id"}});
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
