#include "app/state.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <filesystem>
#include <fstream>

namespace scopes
{
AppState::AppState(DisplaySettings settings)
    : settings_(std::make_shared<DisplaySettings const>(std::move(settings)))
{
}

std::shared_ptr<DisplaySettings const> AppState::settings() const
{
    std::lock_guard lock{mutex_};
    return settings_;
}

void AppState::setSettings(DisplaySettings settings)
{
    auto next = std::make_shared<DisplaySettings const>(std::move(settings));
    std::lock_guard lock{mutex_};
    settings_ = std::move(next);
}

Route AppState::route() const
{
    std::lock_guard lock{mutex_};
    return route_;
}

void AppState::setRoute(Route route)
{
    std::lock_guard lock{mutex_};
    route_ = std::move(route);
}

InputView AppState::input() const
{
    std::lock_guard lock{mutex_};
    return input_;
}

void AppState::setInput(InputView view)
{
    std::lock_guard lock{mutex_};
    input_ = std::move(view);
}

OutputView AppState::output() const
{
    std::lock_guard lock{mutex_};
    return output_;
}

void AppState::setOutput(OutputView view)
{
    std::lock_guard lock{mutex_};
    output_ = std::move(view);
}

RendererView AppState::renderer() const
{
    std::lock_guard lock{mutex_};
    return renderer_;
}

void AppState::setRenderer(RendererView view)
{
    std::lock_guard lock{mutex_};
    renderer_ = view;
}

OutputSettings AppState::outputSettings() const
{
    std::lock_guard lock{mutex_};
    return outputSettings_;
}

void AppState::setOutputSettings(OutputSettings output)
{
    std::lock_guard lock{mutex_};
    outputSettings_ = std::move(output);
}

void AppState::setPreview(PreviewStatus preview)
{
    std::lock_guard lock{mutex_};
    preview_ = std::move(preview);
}

PreviewStatus AppState::preview() const
{
    std::lock_guard lock{mutex_};
    return preview_;
}

void AppState::setMeasurements(Measurements measurements)
{
    std::lock_guard lock{mutex_};
    measurements_ = std::move(measurements);
}

Measurements AppState::measurements() const
{
    std::lock_guard lock{mutex_};
    return measurements_;
}

void AppState::setSnapshot(std::shared_ptr<Snapshot const> snapshot)
{
    {
        std::lock_guard lock{mutex_};
        snapshot_.swap(snapshot);
    }
    // The previous one is released here, outside the lock.
}

std::shared_ptr<Snapshot const> AppState::snapshot() const
{
    std::lock_guard lock{mutex_};
    return snapshot_;
}

void AppState::beatRenderer(std::uint64_t taiNs)
{
    std::lock_guard lock{mutex_};
    rendererBeat_ = taiNs;
}

void AppState::beatWriter(std::uint64_t taiNs)
{
    std::lock_guard lock{mutex_};
    writerBeat_ = taiNs;
}

std::uint64_t AppState::rendererBeat() const
{
    std::lock_guard lock{mutex_};
    return rendererBeat_;
}

std::uint64_t AppState::writerBeat() const
{
    std::lock_guard lock{mutex_};
    return writerBeat_;
}

void AppState::setNmosUp(bool up)
{
    std::lock_guard lock{mutex_};
    nmosUp_ = up;
}

bool AppState::nmosUp() const
{
    std::lock_guard lock{mutex_};
    return nmosUp_;
}

std::string routeJson(Route const& route)
{
    return std::string("{\"routes\":[{\"input\":1,\"video\":true,\"enable\":") + (route.enable ? "true" : "false") + ",\"domain_id\":\"" +
           jsonEscape(route.domainId) + "\",\"flow_id\":\"" + jsonEscape(route.flowId) + "\",\"sender_id\":\"" + jsonEscape(route.senderId) + "\"}]}";
}

std::optional<Route> loadRoute(std::string const& stateDir)
{
    std::ifstream in(std::filesystem::path(stateDir) / "routes.json");
    if (!in)
    {
        return std::nullopt;
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    auto const root = json::parse(body, error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        logWarn("routes_unreadable", {{"error", error}});
        return std::nullopt;
    }
    auto const& obj = root.get<picojson::object>();
    auto const routes = obj.find("routes");
    if (routes == obj.end() || !routes->second.is<picojson::array>())
    {
        return std::nullopt;
    }
    for (auto const& item : routes->second.get<picojson::array>())
    {
        if (!item.is<picojson::object>())
        {
            continue;
        }
        auto const& entry = item.get<picojson::object>();
        auto const video = entry.find("video");
        if (video != entry.end() && video->second.is<bool>() && !video->second.get<bool>())
        {
            continue;
        }
        Route route;
        auto const enable = entry.find("enable");
        route.enable = enable != entry.end() && enable->second.is<bool>() && enable->second.get<bool>();
        route.domainId = json::fieldString(item, "domain_id").value_or("");
        route.flowId = json::fieldString(item, "flow_id").value_or("");
        route.senderId = json::fieldString(item, "sender_id").value_or("");
        return route;
    }
    return std::nullopt;
}

bool saveRoute(std::string const& stateDir, Route const& route)
{
    std::error_code ec;
    std::filesystem::create_directories(stateDir, ec);
    auto const path = std::filesystem::path(stateDir) / "routes.json";
    auto const tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            return false;
        }
        out << routeJson(route) << "\n";
        if (!out.flush())
        {
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}
} // namespace scopes
