#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include <picojson/picojson.h>
#include <string>

#include "app/state.hpp"
#include "config/store.hpp"
#include "ops/httpserver.hpp"
#include "ops/metrics.hpp"
#include "scope/cpu/dispatch.hpp"

namespace scopes
{
// What /api/v1/info reports besides the configuration.
struct BuildInfo
{
    SimdChoice simd;
    bool nmosCompiled = false;
};

// The route of an imported routes document, validated; nullopt when malformed.
std::optional<Route> parseRoutes(picojson::value const& value);

// REST, probes, metrics and the web UI on WEB_PORT (SPECIFICATION §9).
class Api
{
public:
    Api(Config config, ConfigStore& store, AppState& state, Metrics& metrics, BuildInfo build);
    void setIndexPage(std::string page);
    // Called after a live change of the display settings, for the NMOS flow description.
    void setSettingsCallback(std::function<void(DisplaySettings const&)> callback);
    [[nodiscard]] HttpResponse handle(HttpRequest const& request);
    // The WebSocket message of /api/v1/events: state and settings.
    [[nodiscard]] std::string eventsJson() const;
    [[nodiscard]] std::string scopeJson() const;
    [[nodiscard]] std::string statusJson() const;
    [[nodiscard]] bool live() const;
    [[nodiscard]] bool ready(std::string* reason = nullptr) const;

private:
    [[nodiscard]] std::string infoJson() const;
    [[nodiscard]] std::string inputJson() const;
    [[nodiscard]] std::string outputJson() const;
    [[nodiscard]] std::string rendererJson() const;
    [[nodiscard]] std::string previewJson() const;
    HttpResponse patchScope(HttpRequest const& request);
    HttpResponse putConfig(HttpRequest const& request);
    HttpResponse exportConfig() const;
    HttpResponse importConfig(HttpRequest const& request);
    HttpResponse snapshotPng(std::string const& query) const;
    void applied(Config const& config);

    Config config_;
    ConfigStore& store_;
    AppState& state_;
    Metrics& metrics_;
    BuildInfo build_;
    std::string indexPage_;
    std::function<void(DisplaySettings const&)> onSettings_;
    // The last encoded snapshot: PNG encoding of a 1080p picture takes about 100 ms.
    mutable std::mutex pngMu_;
    mutable std::shared_ptr<Snapshot const> pngSource_;
    mutable int pngScale_ = 0;
    mutable std::string png_;
};
} // namespace scopes
