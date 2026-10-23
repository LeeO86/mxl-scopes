#include <doctest/doctest.h>

#include "app/state.hpp"
#include "media/timebase.hpp"
#include "ops/api.hpp"
#include "util/jsonutil.hpp"

#include <filesystem>
#include <unistd.h>

using namespace scopes;

namespace
{
struct Fixture
{
    std::string dir;
    std::map<std::string, std::string> env;
    std::unique_ptr<ConfigStore> store;
    Config config;
    std::unique_ptr<AppState> state;
    Metrics metrics;
    std::unique_ptr<Api> api;

    Fixture()
    {
        dir = (std::filesystem::temp_directory_path() / ("scopes-api-" + std::to_string(::getpid()) + "-" + std::to_string(std::rand()))).string();
        std::filesystem::create_directories(dir);
        env = {{"HOST_ID", "test"}, {"NMOS_HOST_ADDRESS", "192.0.2.1"}, {"SCOPES_STATE_DIR", dir}, {"SCOPES_TRACE_COLOR", "white"}};
        store = std::make_unique<ConfigStore>(env);
        store->useFile(dir + "/config.json");
        config = store->effectiveConfig();
        state = std::make_unique<AppState>(config.display);
        api = std::make_unique<Api>(config, *store, *state, metrics, BuildInfo{});
    }
    ~Fixture()
    {
        std::filesystem::remove_all(dir);
    }
    HttpResponse call(std::string method, std::string path, std::string body = {}, std::string type = "application/json")
    {
        HttpRequest request;
        request.method = std::move(method);
        request.path = std::move(path);
        request.body = std::move(body);
        if (!type.empty())
        {
            request.headers["content-type"] = type;
        }
        return api->handle(request);
    }
};

picojson::value parse(std::string const& body)
{
    std::string error;
    auto value = json::parse(body, error);
    REQUIRE(error.empty());
    return value;
}
} // namespace

TEST_CASE("probes follow the heartbeats and the output flow")
{
    Fixture f;
    CHECK(f.call("GET", "/livez").status == 503);
    CHECK(f.call("GET", "/readyz").status == 503);
    f.state->beatRenderer(taiNowNs());
    f.state->beatWriter(taiNowNs());
    CHECK(f.call("GET", "/livez").status == 200);
    CHECK(f.call("GET", "/readyz").status == 503); // no output flow yet
    OutputView out;
    out.writing = true;
    f.state->setOutput(out);
    CHECK(f.call("GET", "/readyz").status == 200);
}

TEST_CASE("PATCH /api/v1/scope: valid subsets apply and persist, invalid ones change nothing")
{
    Fixture f;
    auto ok = f.call("PATCH", "/api/v1/scope", R"({"update_rate":"frame","intensity":150,"caption":false})");
    CHECK(ok.status == 200);
    CHECK(f.state->settings()->updateRate == 0);
    CHECK(f.state->settings()->intensity == 150);
    CHECK_FALSE(f.state->settings()->caption);
    CHECK(f.store->sourceOf("SCOPES_INTENSITY") == SettingSource::File);
    auto const body = parse(ok.body);
    CHECK(body.get("scope").get("update_rate").get<std::string>() == "frame");
    // Environment keys apply live and are listed as not persisted.
    auto env = f.call("PATCH", "/api/v1/scope", R"({"trace_color":"amber"})");
    CHECK(env.status == 200);
    CHECK(f.state->settings()->traceColor == "amber");
    CHECK(parse(env.body).get("not_persisted").get(0).get<std::string>() == "trace_color");
    CHECK(f.call("PATCH", "/api/v1/scope", R"({"intensity":0})").status == 400);
    CHECK(f.call("PATCH", "/api/v1/scope", R"({"intensity":10,"mode":"rainbow"})").status == 400);
    CHECK(f.state->settings()->intensity == 150);
    CHECK(f.call("PATCH", "/api/v1/scope", R"({"web_port":1})").status == 400);
    CHECK(f.call("PATCH", "/api/v1/scope", "not json").status == 400);
    CHECK(f.call("DELETE", "/api/v1/scope").status == 405);
}

TEST_CASE("config: list, restart keys, export and import")
{
    Fixture f;
    auto const list = parse(f.call("GET", "/api/v1/config").body);
    CHECK(list.get("settings").get<picojson::array>().size() > 40);
    auto const put = f.call("PUT", "/api/v1/config", R"({"NMOS_LABEL":"Scope 1"})");
    CHECK(put.status == 200);
    CHECK(parse(put.body).get("restart_required").get<bool>());
    CHECK(f.call("PUT", "/api/v1/config", R"({"SCOPES_TRACE_COLOR":"green"})").status == 400); // environment
    saveRoute(f.dir, Route{true, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", ""});
    auto const exported = f.call("GET", "/api/v1/config/export");
    CHECK(exported.status == 200);
    auto const doc = parse(exported.body);
    CHECK(doc.get("secrets").get<bool>() == false);
    CHECK(doc.get("settings").get("NMOS_LABEL").get<std::string>() == "Scope 1");
    CHECK(doc.get("routes").get("routes").get(0).get("flow_id").get<std::string>() == "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    auto const imported = f.call("POST", "/api/v1/config/import", exported.body);
    CHECK(imported.status == 200);
    auto const result = parse(imported.body);
    bool skippedColor = false;
    for (auto const& key : result.get("skipped").get<picojson::array>())
    {
        skippedColor = skippedColor || key.get<std::string>() == "SCOPES_TRACE_COLOR";
    }
    CHECK(skippedColor);
    CHECK(loadRoute(f.dir)->flowId == "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    // CSRF: a cross-site form posts text/plain; routes are validated.
    CHECK(f.call("POST", "/api/v1/config/import", exported.body, "text/plain").status == 415);
    CHECK(f.call("POST", "/api/v1/config/import", exported.body, "").status == 415);
    CHECK(f.call("POST", "/api/v1/config/import", R"({"routes":{"routes":[{"enable":true,"flow_id":"../../etc"}]}})").status == 400);
    CHECK(f.call("POST", "/api/v1/config/import", R"({"routes":{"routes":"x"}})").status == 400);
    CHECK(loadRoute(f.dir)->flowId == "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    auto const envText = f.call("GET", "/api/v1/config/env").body;
    CHECK(envText.find("NMOS_LABEL=Scope 1\n") != std::string::npos);
}

TEST_CASE("info, status, metrics, unknown paths")
{
    Fixture f;
    auto const info = parse(f.call("GET", "/api/v1/info").body);
    CHECK(info.get("name").get<std::string>() == "mxl-scopes");
    CHECK(info.get("simd").get("level").is<std::string>());
    CHECK(info.get("preview").get("path").get<std::string>() == "mxl-scopes/scope");
    CHECK(f.call("GET", "/api/v1/status").status == 200);
    CHECK(f.call("GET", "/statusz").status == 200);
    f.metrics.inc("updates_total", {{"mode", "luma"}});
    f.metrics.observe("analyse_seconds", {{"mode", "luma"}}, 0.0015);
    auto const metrics = f.call("GET", "/metrics").body;
    CHECK(metrics.find("mxl_scopes_updates_total{mode=\"luma\"} 1") != std::string::npos);
    CHECK(metrics.find("mxl_scopes_analyse_seconds_bucket{le=\"0.002\",mode=\"luma\"} 1") != std::string::npos);
    CHECK(metrics.find("mxl_scopes_analyse_seconds_bucket{le=\"+Inf\",mode=\"luma\"} 1") != std::string::npos);
    f.metrics.inc("output_frames_total", {}, 1234567);
    CHECK(f.call("GET", "/metrics").body.find("mxl_scopes_output_frames_total 1234567\n") != std::string::npos);
    CHECK(f.call("GET", "/nope").status == 404);
    CHECK(f.call("GET", "/api/v1/snapshot.png").status == 503);
}

TEST_CASE("routes.json round trip")
{
    Fixture f;
    Route route{true, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "cccccccc-cccc-4ccc-8ccc-cccccccccccc"};
    REQUIRE(saveRoute(f.dir, route));
    CHECK(loadRoute(f.dir) == route);
}

TEST_CASE("process metrics: CPU, resident memory, start time, open files without the prefix")
{
    auto const text = Metrics::renderProcess();
    for (auto const* name : {"process_cpu_seconds_total ", "process_resident_memory_bytes ", "process_start_time_seconds ", "process_open_fds "})
    {
        CAPTURE(name);
        auto const at = text.find(std::string("\n") + name);
        REQUIRE(at != std::string::npos);
        // The CPU time of a test that just started can be 0 clock ticks.
        auto const value = text.substr(at + 1 + std::string(name).size(), text.find('\n', at + 1) - at - 1 - std::string(name).size());
        CHECK(std::stod(value) >= (std::string(name).rfind("process_cpu", 0) == 0 ? 0.0 : 1.0));
        CHECK(value.find_first_of("eE") == std::string::npos); // never 1.23457e+06
    }
    Fixture f;
    CHECK(f.call("GET", "/metrics").body.find("\nprocess_resident_memory_bytes ") != std::string::npos);
}
