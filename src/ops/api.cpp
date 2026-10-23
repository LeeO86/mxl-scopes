#include "ops/api.hpp"

#include "media/png.hpp"
#include "media/timebase.hpp"
#include "nmos/ids.hpp"
#include "ops/widgets.hpp"
#include "scope/render.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "util/uuid.hpp"
#include "version.hpp"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace scopes
{
namespace
{
constexpr std::uint64_t kHeartbeatNs = 5'000'000'000ull;

std::string quote(std::string const& text)
{
    return "\"" + jsonEscape(text) + "\"";
}

std::string boolean(bool value)
{
    return value ? "true" : "false";
}

std::string number(double value)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}

HttpResponse jsonResponse(int status, std::string body)
{
    HttpResponse response;
    response.status = status;
    response.contentType = "application/json";
    response.body = std::move(body);
    return response;
}

HttpResponse error(int status, std::string const& message)
{
    return jsonResponse(status, "{\"error\":" + quote(message) + "}");
}

std::string stringList(std::vector<std::string> const& items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        out += (i != 0 ? "," : "") + quote(items[i]);
    }
    return out + "]";
}

std::string rateText(int num, int den)
{
    if (num <= 0)
    {
        return "";
    }
    return den == 1 ? std::to_string(num) : std::to_string(num) + "/" + std::to_string(den);
}

// A JSON value of PATCH /api/v1/scope as the string the configuration stores.
std::optional<std::string> settingText(picojson::value const& value)
{
    if (value.is<std::string>())
    {
        return value.get<std::string>();
    }
    if (value.is<bool>())
    {
        return std::string(value.get<bool>() ? "true" : "false");
    }
    if (value.is<double>())
    {
        double const v = value.get<double>();
        if (std::floor(v) == v && std::fabs(v) < 1e9)
        {
            return std::to_string(static_cast<long long>(v));
        }
        return number(v);
    }
    return std::nullopt;
}
} // namespace

Api::Api(Config config, ConfigStore& store, AppState& state, Metrics& metrics, BuildInfo build)
    : config_(std::move(config))
    , store_(store)
    , state_(state)
    , metrics_(metrics)
    , build_(std::move(build))
{
}

void Api::setIndexPage(std::string page)
{
    indexPage_ = std::move(page);
}

void Api::setSettingsCallback(std::function<void(DisplaySettings const&)> callback)
{
    onSettings_ = std::move(callback);
}

bool Api::live() const
{
    auto const now = taiNowNs();
    auto const renderer = state_.rendererBeat();
    auto const writer = state_.writerBeat();
    return renderer != 0 && writer != 0 && now - std::min(now, renderer) < kHeartbeatNs && now - std::min(now, writer) < kHeartbeatNs;
}

bool Api::ready(std::string* reason) const
{
    auto const now = taiNowNs();
    auto const writer = state_.writerBeat();
    if (writer == 0 || now - std::min(now, writer) >= kHeartbeatNs)
    {
        if (reason != nullptr)
        {
            *reason = "writer_stale";
        }
        return false;
    }
    if (!state_.output().writing)
    {
        if (reason != nullptr)
        {
            *reason = "no_output_flow";
        }
        return false;
    }
    if (config_.nmosEnable && !config_.nmosRegistryAddress.empty() && !state_.nmosUp())
    {
        if (reason != nullptr)
        {
            *reason = "not_registered";
        }
        return false;
    }
    return true;
}

std::string Api::previewJson() const
{
    return scopes::previewJson(config_, state_.preview());
}

std::string Api::infoJson() const
{
    auto const ids = makeNmosIds(config_.nmosSeed);
    auto const output = state_.outputSettings();
    std::ostringstream out;
    out << "{\"name\":\"mxl-scopes\",\"version\":" << quote(SCOPES_VERSION) << ",\"mxl_revision\":" << quote(SCOPES_MXL_REVISION)
        << ",\"label\":" << quote(config_.nmosLabel.empty() ? config_.hostId : config_.nmosLabel) << ",\"backend\":\"cpu\""
        << ",\"simd\":{\"level\":" << quote(simdName(build_.simd.level)) << ",\"requested\":" << quote(build_.simd.requested)
        << ",\"cpu\":" << quote(build_.simd.cpu) << ",\"cpu_family\":" << quote(build_.simd.family) << "}"
        << ",\"modes\":[\"luma\",\"ycbcr-parade\",\"rgb-parade\",\"rgb-overlay\",\"ycbcr-overlay\"],\"node_id\":" << quote(ids.node) << ",\"device_id\":" << quote(ids.device)
        << ",\"receiver_id\":" << quote(ids.receiver) << ",\"sender_id\":" << quote(ids.sender) << ",\"domain_id\":" << quote(config_.outputDomainId)
        << ",\"output\":{\"format\":" << quote(std::to_string(output.width) + "x" + std::to_string(output.height))
        << ",\"rate\":" << quote(output.rate ? rateToken(*output.rate) : "input") << ",\"key\":" << boolean(output.key) << "}"
        << ",\"ports\":{\"web\":" << config_.webPort << ",\"nmos\":" << config_.nmosPort << "}"
        << ",\"nmos\":{\"enabled\":" << boolean(config_.nmosEnable) << ",\"compiled\":" << boolean(build_.nmosCompiled) << "}"
        << ",\"preview\":" << previewJson() << "}";
    return out.str();
}

std::string Api::inputJson() const
{
    auto const in = state_.input();
    auto const opt = [](std::string const& value) { return value.empty() ? std::string("null") : quote(value); };
    std::ostringstream out;
    out << "{\"state\":" << quote(in.state) << ",\"reason\":" << quote(in.reason) << ",\"route\":{\"enable\":" << boolean(in.route.enable)
        << ",\"domain_id\":" << opt(in.route.domainId) << ",\"flow_id\":" << opt(in.route.flowId) << ",\"sender_id\":" << opt(in.route.senderId) << "}"
        << ",\"label\":" << quote(in.label) << ",\"media_type\":" << quote(in.mediaType) << ",\"width\":" << in.width << ",\"height\":" << in.height
        << ",\"rate\":" << quote(rateText(in.rateNum, in.rateDen)) << ",\"interlaced\":" << boolean(in.interlaced)
        << ",\"colorimetry\":" << quote(in.colorimetry) << ",\"colorimetry_warning\":" << boolean(in.colorimetryWarning) << ",\"grains\":" << in.grains
        << ",\"resyncs\":" << in.resyncs << ",\"latency_ms\":" << number(in.latencyMs) << "}";
    return out.str();
}

std::string Api::outputJson() const
{
    auto const o = state_.output();
    std::ostringstream out;
    out << "{\"flow_id\":" << (o.flowId.empty() ? "null" : quote(o.flowId)) << ",\"domain_id\":" << quote(o.domainId) << ",\"width\":" << o.width
        << ",\"height\":" << o.height << ",\"rate\":" << quote(rateText(o.rateNum, o.rateDen)) << ",\"media_type\":"
        << quote(o.key ? "video/v210a" : "video/v210") << ",\"key\":" << boolean(o.key) << ",\"writing\":" << boolean(o.writing)
        << ",\"frames\":" << o.frames << ",\"late\":" << o.late << ",\"missed\":" << o.missed;
    // Where the plot sits in the raster (§5.2), for tests and tools that sample the output.
    // The layout of the picture on the output (its mode), else of the current settings.
    auto const mode = o.mode.empty() ? state_.settings()->mode : o.mode;
    out << ",\"mode\":" << quote(mode);
    auto const layout = makeLayout(o.width, o.height, panelCount(mode));
    out << ",\"plot\":{\"x\":" << layout.plotX << ",\"y\":" << layout.plotY << ",\"width\":" << layout.plotW << ",\"height\":" << layout.plotH
        << ",\"bins\":" << histogramBins(layout) << ",\"panels\":[";
    for (int k = 0; k < layout.panels; ++k)
    {
        out << (k > 0 ? "," : "") << "{\"x\":" << layout.panelX[static_cast<std::size_t>(k)] << ",\"width\":" << layout.panelW << "}";
    }
    out << "]}}";
    return out.str();
}

std::string Api::rendererJson() const
{
    auto const r = state_.renderer();
    std::ostringstream out;
    out << "{\"updates\":" << r.updates << ",\"skipped\":" << r.skipped << ",\"torn\":" << r.torn << ",\"update_rate_hz\":" << number(r.updateHz)
        << ",\"analyse_ms\":" << number(r.analyseMs) << ",\"analyse_cpu_ms\":" << number(r.analyseCpuMs) << ",\"render_ms\":" << number(r.renderMs) << ",\"line_step\":" << r.lineStep << ",\"simd\":" << quote(simdName(build_.simd.level)) << ",\"threads\":" << config_.threads << "}";
    return out.str();
}

std::string Api::statusJson() const
{
    std::string reason;
    bool const isReady = ready(&reason);
    auto const ids = makeNmosIds(config_.nmosSeed);
    std::ostringstream out;
    out << "{\"live\":" << boolean(live()) << ",\"ready\":" << boolean(isReady) << ",\"ready_reason\":" << quote(reason) << ",\"input\":" << inputJson()
        << ",\"output\":" << outputJson() << ",\"renderer\":" << rendererJson() << ",\"preview\":" << previewJson() << ",\"nmos\":{\"enabled\":"
        << boolean(config_.nmosEnable) << ",\"node_id\":" << quote(ids.node) << ",\"registry\":" << quote(config_.nmosRegistryAddress)
        << ",\"registered\":" << boolean(state_.nmosUp()) << "}}";
    return out.str();
}

std::string Api::scopeJson() const
{
    auto const s = state_.settings();
    std::ostringstream out;
    out << "{\"mode\":" << quote(s->mode) << ",\"title\":" << quote(modeTitle(s->mode)) << ",\"update_rate\":"
        << (s->updateRate <= 0 ? std::string("\"frame\"") : number(s->updateRate)) << ",\"accumulate\":" << quote(s->accumulate)
        << ",\"line_step\":" << s->lineStep << ",\"pixel_step\":" << s->pixelStep << ",\"range\":" << quote(s->limitedRange ? "limited" : "full")
        << ",\"graticule_units\":" << quote(s->graticuleUnits) << ",\"graticule_level\":" << s->graticuleLevel << ",\"intensity\":" << s->intensity
        << ",\"trace_min\":" << s->traceMin << ",\"trace_color\":" << quote(s->traceColor) << ",\"caption\":" << boolean(s->caption)
        << ",\"layout\":" << quote(s->layout) << ",\"bg_alpha\":" << s->bgAlpha << ",\"input_offset_grains\":" << s->inputOffsetGrains
        << ",\"hold_ms\":" << s->holdMs << ",\"preview_fps\":" << s->previewFps << ",\"sources\":{";
    bool first = true;
    for (auto const& [field, key] : scopeFields())
    {
        out << (first ? "" : ",") << quote(field) << ":" << quote(settingSourceName(store_.sourceOf(key)));
        first = false;
    }
    out << "}}";
    return out.str();
}

std::string Api::eventsJson() const
{
    return "{\"type\":\"status\",\"status\":" + statusJson() + ",\"scope\":" + scopeJson() + ",\"measurements\":" + measurementsJson(state_.measurements()) + "}";
}

void Api::applied(Config const& config)
{
    state_.setSettings(config.display);
    // The output flow keys apply live: the writer opens a flow with a new id (§7).
    state_.setOutputSettings(outputSettingsOf(config));
    setLogLevel(parseLogLevel(config.logLevel));
    if (onSettings_)
    {
        onSettings_(config.display);
    }
}

HttpResponse Api::patchScope(HttpRequest const& request)
{
    std::string parseError;
    auto const root = json::parse(request.body, parseError);
    if (!parseError.empty() || !root.is<picojson::object>())
    {
        return error(400, "the body must be a JSON object");
    }
    std::map<std::string, std::string> changes;
    for (auto const& [field, value] : root.get<picojson::object>())
    {
        auto const key = scopeFieldKey(field);
        if (!key)
        {
            return error(400, "unknown scope setting " + field);
        }
        auto const text = settingText(value);
        if (!text)
        {
            return error(400, field + " must be a string, number or boolean");
        }
        changes[*key] = *text;
    }
    auto const result = store_.applyLive(changes);
    if (auto const* problem = std::get_if<std::string>(&result))
    {
        return error(400, *problem);
    }
    auto const& ok = std::get<ConfigStore::UpdateResult>(result);
    applied(ok.config);
    std::vector<std::string> fields;
    for (auto const& key : ok.notPersisted)
    {
        for (auto const& [field, name] : scopeFields())
        {
            if (name == key)
            {
                fields.push_back(field);
            }
        }
    }
    logInfo("scope_settings", {{"keys", std::to_string(changes.size())}});
    return jsonResponse(200, "{\"scope\":" + scopeJson() + ",\"not_persisted\":" + stringList(fields) + "}");
}

HttpResponse Api::putConfig(HttpRequest const& request)
{
    std::string parseError;
    auto const root = json::parse(request.body, parseError);
    if (!parseError.empty() || !root.is<picojson::object>())
    {
        return error(400, "the body must be a JSON object");
    }
    std::map<std::string, std::optional<std::string>> changes;
    for (auto const& [key, value] : root.get<picojson::object>())
    {
        if (value.is<picojson::null>())
        {
            changes[key] = std::nullopt;
        }
        else if (value.is<std::string>())
        {
            changes[key] = value.get<std::string>();
        }
        else
        {
            return error(400, "values must be strings or null");
        }
    }
    auto const result = store_.update(changes);
    if (auto const* problem = std::get_if<std::string>(&result))
    {
        return error(400, *problem);
    }
    auto const& ok = std::get<ConfigStore::UpdateResult>(result);
    applied(ok.config);
    // The output flow keys apply live with a new flow id (§7); listed as `flow_keys`.
    auto const& restart = ok.restartRequired;
    return jsonResponse(200, "{\"restart_required\":" + boolean(!restart.empty()) + ",\"keys\":" + stringList(restart) + ",\"flow_keys\":" + stringList(ok.flowKeys) + "}");
}

HttpResponse Api::exportConfig() const
{
    std::ostringstream out;
    out << "{\"version\":1,\"name\":\"mxl-scopes\",\"app_version\":" << quote(SCOPES_VERSION) << ",\"secrets\":false,\"settings\":{";
    bool first = true;
    for (auto const& def : settingSchema())
    {
        out << (first ? "" : ",") << quote(def.name) << ":" << quote(store_.effectiveValue(def.name));
        first = false;
    }
    auto const route = loadRoute(config_.stateDir).value_or(state_.route());
    out << "},\"routes\":" << routeJson(route) << "}";
    return jsonResponse(200, out.str());
}

std::optional<Route> parseRoutes(picojson::value const& value)
{
    if (!value.is<picojson::object>())
    {
        return std::nullopt;
    }
    auto const& obj = value.get<picojson::object>();
    auto const list = obj.find("routes");
    if (list == obj.end() || !list->second.is<picojson::array>())
    {
        return std::nullopt;
    }
    Route route;
    for (auto const& item : list->second.get<picojson::array>())
    {
        if (!item.is<picojson::object>())
        {
            return std::nullopt;
        }
        auto const& entry = item.get<picojson::object>();
        if (auto const video = entry.find("video"); video != entry.end() && video->second.is<bool>() && !video->second.get<bool>())
        {
            continue;
        }
        auto const enable = entry.find("enable");
        if (enable != entry.end() && !enable->second.is<bool>())
        {
            return std::nullopt;
        }
        route.enable = enable != entry.end() && enable->second.get<bool>();
        for (auto const& [key, target] : {std::pair<char const*, std::string*>{"domain_id", &route.domainId}, {"flow_id", &route.flowId}, {"sender_id", &route.senderId}})
        {
            auto const field = entry.find(key);
            if (field == entry.end() || field->second.is<picojson::null>())
            {
                continue;
            }
            if (!field->second.is<std::string>() || (!field->second.get<std::string>().empty() && !isUuid(field->second.get<std::string>())))
            {
                return std::nullopt;
            }
            *target = field->second.get<std::string>();
        }
        break;
    }
    return route;
}

HttpResponse Api::importConfig(HttpRequest const& request)
{
    // A cross-site form can POST text/plain without a preflight; an import must say it is JSON.
    auto const type = request.headers.find("content-type");
    if (type == request.headers.end() || type->second.rfind("application/json", 0) != 0)
    {
        return error(415, "the import needs Content-Type: application/json");
    }
    std::string parseError;
    auto const root = json::parse(request.body, parseError);
    if (!parseError.empty() || !root.is<picojson::object>())
    {
        return error(400, "the import must be a JSON object");
    }
    auto const& obj = root.get<picojson::object>();
    std::vector<std::string> skipped;
    std::vector<std::string> restart;
    std::vector<std::string> flowKeys;
    if (auto const settings = obj.find("settings"); settings != obj.end())
    {
        if (!settings->second.is<picojson::object>())
        {
            return error(400, "settings must be an object of strings");
        }
        std::map<std::string, std::optional<std::string>> changes;
        for (auto const& [key, value] : settings->second.get<picojson::object>())
        {
            if (!value.is<std::string>())
            {
                return error(400, "settings values must be strings");
            }
            // Keys the environment sets win anyway: skipped and listed, not an error.
            if (store_.sourceOf(key) == SettingSource::Env || store_.sourceOf(key) == SettingSource::Live)
            {
                skipped.push_back(key);
                continue;
            }
            if (store_.effectiveValue(key) == value.get<std::string>() && store_.sourceOf(key) == SettingSource::Default)
            {
                continue;
            }
            changes[key] = value.get<std::string>();
        }
        auto const result = store_.update(changes);
        if (auto const* problem = std::get_if<std::string>(&result))
        {
            return error(400, *problem);
        }
        auto const& ok = std::get<ConfigStore::UpdateResult>(result);
        applied(ok.config);
        restart = ok.restartRequired;
        flowKeys = ok.flowKeys;
    }
    bool routesRestart = false;
    if (auto const routes = obj.find("routes"); routes != obj.end())
    {
        auto const route = parseRoutes(routes->second);
        if (!route)
        {
            return error(400, "routes must be {\"routes\":[{\"enable\":bool,\"domain_id\":uuid,\"flow_id\":uuid,\"sender_id\":uuid}]} (ids may be empty)");
        }
        // Written in the canonical form, never the imported text as it came.
        if (!saveRoute(config_.stateDir, *route))
        {
            return error(500, "cannot write routes.json");
        }
        routesRestart = true;
    }
    return jsonResponse(200, "{\"ok\":true,\"secrets\":false,\"skipped\":" + stringList(skipped) + ",\"restart_required\":" +
                                 boolean(!restart.empty() || routesRestart) + ",\"keys\":" + stringList(restart) + ",\"flow_keys\":" + stringList(flowKeys) + ",\"routes_restart\":" +
                                 boolean(routesRestart) + "}");
}

HttpResponse Api::snapshotPng(std::string const& query) const
{
    auto const snapshot = state_.snapshot();
    if (!snapshot || snapshot->v210.empty())
    {
        return error(503, "no scope picture yet");
    }
    // ?scale=2 halves the raster (the web page); the default is the full raster (§9.2).
    int scale = 1;
    if (auto const at = query.find("scale="); at != std::string::npos)
    {
        scale = std::atoi(query.c_str() + at + 6);
        if (scale < 1 || scale > 4)
        {
            return error(400, "scale must be 1 to 4");
        }
    }
    HttpResponse response;
    response.contentType = "image/png";
    response.headers.emplace_back("Cache-Control", "no-store");
    std::lock_guard lock{pngMu_};
    if (pngSource_ != snapshot || pngScale_ != scale)
    {
        Frame422 frame;
        frame.allocate(snapshot->width, snapshot->height);
        unpackV210(snapshot->v210.data(), v210RowBytes(snapshot->width), frame);
        if (snapshot->key.size() >= alphaRowBytes(snapshot->width) * static_cast<std::size_t>(snapshot->height))
        {
            unpackAlpha(snapshot->key.data(), frame);
        }
        png_ = encodePng(frame, scale);
        pngSource_ = snapshot;
        pngScale_ = scale;
    }
    response.body = png_;
    return response;
}

HttpResponse Api::handle(HttpRequest const& request)
{
    auto const& path = request.path;
    auto const& method = request.method;
    bool const mutating = method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE";
    if (!config_.webEnable && (mutating || path == "/" || path == "/index.html"))
    {
        return error(404, "the web UI and changes are off (WEB_ENABLE=false)");
    }
    if ((path == "/" || path == "/index.html") && method == "GET")
    {
        HttpResponse page;
        page.contentType = "text/html; charset=utf-8";
        page.body = indexPage_.empty() ? std::string("<!doctype html><title>mxl-scopes</title><p>The UI is not embedded. See /api/v1/info.</p>") : indexPage_;
        return page;
    }
    // Operator-screen widgets (§9.3): the list with CORS for the listed origins, the pages with
    // `frame-ancestors`; the page is the UI's, which shows only the widget on /widget/<id>.
    if (path == "/widgets")
    {
        if (method != "GET")
        {
            auto response = error(405, "GET only");
            response.headers.emplace_back("Allow", "GET");
            return response;
        }
        auto response = jsonResponse(200, widgetsJson());
        if (auto const origin = request.headers.find("origin");
            origin != request.headers.end() && listedOrigin(config_.widgetFrameAncestors, origin->second))
        {
            response.headers.emplace_back("Access-Control-Allow-Origin", origin->second);
        }
        response.headers.emplace_back("Vary", "Origin");
        return response;
    }
    if (path.rfind("/widget/", 0) == 0 && method == "GET")
    {
        if (!knownWidget(path.substr(8)))
        {
            return error(404, "unknown widget");
        }
        if (auto const problem = widgetQueryError(parseQuery(request.query)); !problem.empty())
        {
            return error(400, problem);
        }
        HttpResponse page;
        page.contentType = "text/html; charset=utf-8";
        page.body = indexPage_.empty() ? std::string("<!doctype html><title>mxl-scopes</title><p>The UI is not embedded.</p>") : indexPage_;
        page.headers.emplace_back("Content-Security-Policy", "frame-ancestors " + config_.widgetFrameAncestors);
        page.headers.emplace_back("Cache-Control", "no-store");
        return page;
    }
    if (path == "/livez")
    {
        return live() ? jsonResponse(200, "{\"status\":\"ok\"}") : jsonResponse(503, "{\"status\":\"stale\"}");
    }
    if (path == "/readyz")
    {
        std::string reason;
        return ready(&reason) ? jsonResponse(200, "{\"status\":\"ready\"}") : jsonResponse(503, "{\"status\":\"not_ready\",\"reason\":" + quote(reason) + "}");
    }
    if (path == "/metrics")
    {
        HttpResponse response;
        response.contentType = "text/plain; version=0.0.4";
        response.body = metrics_.render();
        return response;
    }
    if (path == "/statusz" || path == "/api/v1/status")
    {
        return jsonResponse(200, statusJson());
    }
    if (path == "/api/v1/events" && method == "GET")
    {
        HttpResponse response;
        response.websocket = true;
        return response;
    }
    if (path == "/api/v1/info" && method == "GET")
    {
        return jsonResponse(200, infoJson());
    }
    if (path == "/api/v1/input" && method == "GET")
    {
        return jsonResponse(200, inputJson());
    }
    if (path == "/api/v1/measurements" && method == "GET")
    {
        return jsonResponse(200, measurementsJson(state_.measurements()));
    }
    if (path == "/api/v1/output" && method == "GET")
    {
        return jsonResponse(200, outputJson());
    }
    if (path == "/api/v1/scope")
    {
        if (method == "GET")
        {
            return jsonResponse(200, scopeJson());
        }
        if (method == "PATCH")
        {
            return patchScope(request);
        }
        auto response = error(405, "GET or PATCH");
        response.headers.emplace_back("Allow", "GET, PATCH");
        return response;
    }
    if (path == "/api/v1/snapshot.png" && method == "GET")
    {
        return snapshotPng(request.query);
    }
    if (path == "/api/v1/config")
    {
        if (method == "GET")
        {
            std::ostringstream out;
            out << "{\"file\":" << quote(store_.filePath().value_or("")) << ",\"settings\":[";
            bool first = true;
            for (auto const& def : settingSchema())
            {
                auto const source = store_.sourceOf(def.name);
                char const* apply = def.apply == Apply::Live ? "live" : def.apply == Apply::Flow ? "flow" : "restart";
                out << (first ? "" : ",") << "{\"key\":" << quote(def.name) << ",\"value\":" << quote(store_.effectiveValue(def.name))
                    << ",\"default\":" << quote(def.defaultValue) << ",\"source\":" << quote(settingSourceName(source)) << ",\"apply\":\"" << apply
                    << "\",\"editable\":" << boolean(source != SettingSource::Env && source != SettingSource::Live) << "}";
                first = false;
            }
            out << "]}";
            return jsonResponse(200, out.str());
        }
        if (method == "PUT")
        {
            return putConfig(request);
        }
        auto response = error(405, "GET or PUT");
        response.headers.emplace_back("Allow", "GET, PUT");
        return response;
    }
    if (path == "/api/v1/config/export" && method == "GET")
    {
        return exportConfig();
    }
    if (path == "/api/v1/config/import" && method == "POST")
    {
        return importConfig(request);
    }
    if (path == "/api/v1/config/env" && method == "GET")
    {
        HttpResponse response;
        response.contentType = "text/plain; charset=utf-8";
        response.body = store_.renderEnvBlock();
        return response;
    }
    return error(404, "not found");
}
} // namespace scopes
