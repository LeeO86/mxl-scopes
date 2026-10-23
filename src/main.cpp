#include "app/state.hpp"
#include "config/store.hpp"
#include "mxlio/io.hpp"
#include "nmos/node.hpp"
#include "ops/api.hpp"
#include "ops/httpserver.hpp"
#include "ops/metrics.hpp"
#include "preview/child.hpp"
#include "preview/plan.hpp"
#include "preview/publisher.hpp"
#include "scope/scheduler.hpp"
#include "scope/cpu/dispatch.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#include "ops/webui_generated.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iostream>
#include <map>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace
{
std::atomic<bool> gStop{false};

void onSignal(int)
{
    gStop.store(true);
}

std::map<std::string, std::string> environmentMap()
{
    std::map<std::string, std::string> env;
    for (char** cursor = environ; cursor != nullptr && *cursor != nullptr; ++cursor)
    {
        std::string const entry(*cursor);
        auto const eq = entry.find('=');
        if (eq != std::string::npos)
        {
            env.emplace(entry.substr(0, eq), entry.substr(eq + 1));
        }
    }
    return env;
}

constexpr bool kNmosCompiled =
#if defined(SCOPES_WITH_NMOS)
    true;
#else
    false;
#endif
} // namespace

int main(int argc, char** argv)
{
    auto env = environmentMap();
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::cout << "mxl-scopes " << SCOPES_VERSION << " (MXL " << SCOPES_MXL_REVISION << ")\n"
                      << "Usage: mxl-scopes [--config FILE] [--version]\n"
                      << "Configuration: environment > SCOPES_CONFIG_FILE (else <SCOPES_STATE_DIR>/config.json) > defaults.\n"
                      << "See README.md and SPECIFICATION.md for the settings, ports and exit codes.\n";
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0)
        {
            std::cout << SCOPES_VERSION << "\n";
            return 0;
        }
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc)
        {
            env["SCOPES_CONFIG_FILE"] = argv[++i];
        }
    }
    scopes::setLogFormatJson(true);
    try
    {
        scopes::ConfigStore store(env);
        auto config = store.effectiveConfig();
        std::error_code ec;
        std::filesystem::create_directories(config.stateDir, ec);
        if (ec || access(config.stateDir.c_str(), W_OK) != 0)
        {
            scopes::logError("state_dir", {{"path", config.stateDir}, {"error", ec ? ec.message() : "not writable"}});
            return 75;
        }
        store.useFile(config.stateDir + "/config.json");
        config = store.effectiveConfig();
        scopes::setLogLevel(scopes::parseLogLevel(config.logLevel));
        scopes::setLogFormatJson(config.logFormat != "text");
        if (config.nmosEnable && !kNmosCompiled)
        {
            scopes::logError("config", {{"error", "NMOS_ENABLE=true but this binary was built without nmos-cpp"}});
            return 78;
        }
        auto const simd = scopes::chooseSimd(config.simd);
        if (simd.fellBack)
        {
            scopes::logWarn("simd_fallback", {{"requested", config.simd}, {"level", scopes::simdName(simd.level)}});
        }
        scopes::logInfo("start", {{"version", SCOPES_VERSION}, {"mxl", SCOPES_MXL_REVISION}, {"simd", scopes::simdName(simd.level)},
                                     {"cpu", simd.cpu + (simd.family.empty() ? "" : " (" + simd.family + ")")}, {"state_dir", config.stateDir}, {"output_domain", config.outputDomainDir}});

        scopes::Metrics metrics;
        metrics.set("info", {{"version", SCOPES_VERSION}, {"mxl_revision", SCOPES_MXL_REVISION}, {"backend", "cpu"}, {"simd", scopes::simdName(simd.level)}}, 1);
        metrics.set("simd_level", {{"level", scopes::simdName(simd.level)}}, 1);
        metrics.set("nmos_registry_up", {}, 0);
        metrics.inc("nmos_activations_total", {}, 0);
        for (char const* name : {"updates_skipped_total", "updates_torn_total", "output_frames_total", "output_frames_late_total", "output_frames_missed_total",
                 "input_grains_total", "input_resyncs_total", "accumulate_dropped_total"})
        {
            metrics.inc(name, {}, 0);
        }
        metrics.inc("updates_total", {{"mode", config.display.mode}}, 0);

        scopes::AppState state(config.display);
        state.setOutputSettings(scopes::outputSettingsOf(config));
        if (auto const restored = scopes::loadRoute(config.stateDir))
        {
            state.setRoute(*restored);
            scopes::logInfo("route_restored", {{"enable", restored->enable ? "true" : "false"}, {"flow", restored->flowId}});
        }
        scopes::MediaIo io(config, state, metrics, simd.level);
        scopes::NmosNode node(config, [&](scopes::Route const& route) {
            state.setRoute(route);
            if (!scopes::saveRoute(config.stateDir, route))
            {
                scopes::logError("routes_save_failed", {{"path", config.stateDir}});
            }
            metrics.inc("nmos_activations_total", {});
        });
        io.setFlowCallback([&](scopes::OutputFlow const& flow) { node.setOutputFlow(flow, io.domainId()); });
        scopes::Api api(config, store, state, metrics, scopes::BuildInfo{simd, kNmosCompiled});
        api.setIndexPage(std::string(scopes::webui::indexHtml()));
        scopes::HttpServer http;
        try
        {
            io.start();
            node.start();
            node.restoreRoute(state.route());
            http.start(config.webPort, [&](scopes::HttpRequest const& request) { return api.handle(request); });
        }
        catch (scopes::ConfigError const& ex)
        {
            scopes::logError("startup", {{"error", ex.what()}});
            return 78;
        }
        catch (std::exception const& ex)
        {
            scopes::logError("startup", {{"error", ex.what()}});
            return 75;
        }
        // The WebRTC preview (§2.5): own mode runs a MediaMTX child with a config in the state
        // directory; the publisher sends the scope picture to it (or to the shared MediaMTX).
        auto const plan = scopes::previewPlan(config);
        std::unique_ptr<scopes::ChildProcess> mediamtx;
        std::unique_ptr<scopes::PreviewPublisher> publisher;
        std::uint64_t previewFrames = 0;
        if (plan.webrtc)
        {
            if (plan.ownMediamtx)
            {
                auto const yml = (std::filesystem::path(config.stateDir) / "mediamtx.yml").string();
                std::ofstream(yml, std::ios::trunc) << scopes::renderMediamtxConfig(config);
                mediamtx = std::make_unique<scopes::ChildProcess>("mediamtx");
                mediamtx->start({"mediamtx", yml});
            }
            publisher = std::make_unique<scopes::PreviewPublisher>(
                plan.publishUrl, config.previewHeight, [&] { return state.snapshot(); },
                [&] {
                    // SCOPES_PREVIEW_FPS, never more than the update rate (§2.5).
                    auto const s = state.settings();
                    auto const in = state.input();
                    double const inputHz = in.rateNum > 0 && in.rateDen > 0 ? static_cast<double>(in.rateNum) / in.rateDen / (in.interlaced ? 2 : 1) : 0;
                    double const rate = scopes::modeUpdateRate(s->mode, s->updateRate, inputHz);
                    double const updates = rate > 0 ? rate : (inputHz > 0 ? inputHz : 50.0);
                    return std::min<double>(s->previewFps, updates);
                });
            publisher->start([&](double seconds) { metrics.observe("preview_encode_seconds", {}, seconds); });
            metrics.set("preview_publish_mode", {{"mode", plan.ownMediamtx ? "own" : "shared"}}, 1);
            metrics.inc("preview_frames_total", {}, 0);
        }
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        std::signal(SIGPIPE, SIG_IGN);
        scopes::logInfo("ready", {{"web", std::to_string(config.webPort)}, {"nmos", std::to_string(config.nmosPort)}});

        auto nextEvents = std::chrono::steady_clock::now();
        auto nextRegistry = std::chrono::steady_clock::now();
        while (!gStop.load())
        {
            auto const now = std::chrono::steady_clock::now();
            if (now >= nextRegistry)
            {
                // A Query API lookup; at most once a second (readiness, §9.4).
                bool const up = node.registered();
                state.setNmosUp(up);
                metrics.set("nmos_registry_up", {}, up ? 1 : 0);
                nextRegistry = now + std::chrono::seconds(1);
            }
            if (now >= nextEvents)
            {
                if (publisher)
                {
                    auto status = publisher->status();
                    if (mediamtx)
                    {
                        status.mediamtxRunning = mediamtx->running();
                        status.mediamtxRestarts = mediamtx->restarts();
                    }
                    for (char const* name : {"connecting", "publishing", "error"})
                    {
                        metrics.set("preview_publish_state", {{"state", name}}, status.state == name ? 1 : 0);
                    }
                    if (!status.encoder.empty())
                    {
                        metrics.set("preview_encoder", {{"encoder", status.encoder}}, 1);
                    }
                    metrics.inc("preview_frames_total", {}, static_cast<double>(status.frames - previewFrames));
                    previewFrames = status.frames;
                    state.setPreview(std::move(status));
                }
                http.broadcast(api.eventsJson());
                nextEvents = now + std::chrono::milliseconds(200);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        scopes::logInfo("shutdown", {});
        std::atomic<bool> finished{false};
        std::thread watchdog([&] {
            for (int i = 0; i < config.shutdownTimeoutS * 10 && !finished.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!finished.load())
            {
                _exit(143);
            }
        });
        http.stop();
        if (publisher)
        {
            publisher->stop();
        }
        if (mediamtx)
        {
            mediamtx->stop();
        }
        io.stop();
        node.stop();
        if (config.cleanupOnExit)
        {
            io.removeOwnDomain();
        }
        finished.store(true);
        watchdog.join();
        return 143;
    }
    catch (scopes::ConfigError const& ex)
    {
        scopes::logError("config", {{"error", ex.what()}});
        return 78;
    }
    catch (std::exception const& ex)
    {
        scopes::logError("fatal", {{"error", ex.what()}});
        return 75;
    }
}
