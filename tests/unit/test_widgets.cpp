// Operator-screen widgets (SPECIFICATION §9.3): /widgets with CORS for the listed origins only,
// /widget/scope with frame-ancestors and parameter checks, 404 and 405.
#include <doctest/doctest.h>

#include "app/state.hpp"
#include "ops/api.hpp"
#include "ops/widgets.hpp"
#include "util/jsonutil.hpp"

#include <filesystem>
#include <unistd.h>

using namespace scopes;

namespace
{
std::string header(HttpResponse const& response, std::string const& name)
{
    for (auto const& [key, value] : response.headers)
    {
        if (key == name)
        {
            return value;
        }
    }
    return {};
}

struct WidgetFixture
{
    std::string dir;
    std::unique_ptr<ConfigStore> store;
    Config config;
    std::unique_ptr<AppState> state;
    Metrics metrics;
    std::unique_ptr<Api> api;

    WidgetFixture()
    {
        dir = (std::filesystem::temp_directory_path() / ("scopes-widgets-" + std::to_string(::getpid()))).string();
        std::filesystem::create_directories(dir);
        store = std::make_unique<ConfigStore>(std::map<std::string, std::string>{
            {"NMOS_HOST_ADDRESS", "192.0.2.1"}, {"SCOPES_STATE_DIR", dir}, {"WIDGET_FRAME_ANCESTORS", "'self' https://designer.example"}});
        store->useFile(dir + "/config.json");
        config = store->effectiveConfig();
        state = std::make_unique<AppState>(config.display);
        api = std::make_unique<Api>(config, *store, *state, metrics, BuildInfo{});
        api->setIndexPage("<!doctype html><title>ui</title>");
    }
    ~WidgetFixture()
    {
        std::filesystem::remove_all(dir);
    }
    HttpResponse call(std::string method, std::string path, std::string query = {}, std::string origin = {})
    {
        HttpRequest request;
        request.method = std::move(method);
        request.path = std::move(path);
        request.query = std::move(query);
        if (!origin.empty())
        {
            request.headers["origin"] = origin;
        }
        return api->handle(request);
    }
};
} // namespace

TEST_CASE("widgets: the list, CORS only for listed origins, GET only")
{
    WidgetFixture f;
    auto const list = f.call("GET", "/widgets", "", "https://designer.example");
    CHECK(list.status == 200);
    CHECK(header(list, "Access-Control-Allow-Origin") == "https://designer.example");
    CHECK(header(list, "Vary") == "Origin");
    std::string error;
    auto const doc = json::parse(list.body, error);
    REQUIRE(error.empty());
    REQUIRE(doc.is<picojson::array>());
    auto const& scope = doc.get<picojson::array>().at(0);
    CHECK(scope.get("id").get<std::string>() == "scope");
    CHECK(scope.get("min_size").get("w").get<double>() == 320);
    CHECK(scope.get("params").get("properties").get("controls").get("type").get<std::string>() == "boolean");
    auto const other = f.call("GET", "/widgets", "", "https://evil.example");
    CHECK(header(other, "Access-Control-Allow-Origin").empty());
    auto const post = f.call("POST", "/widgets");
    CHECK(post.status == 405);
    CHECK(header(post, "Allow") == "GET");
}

TEST_CASE("widgets: /widget/scope with frame-ancestors, 400 and 404")
{
    WidgetFixture f;
    auto const page = f.call("GET", "/widget/scope", "controls=true&measurements=1&theme=transparent");
    CHECK(page.status == 200);
    CHECK(page.contentType.find("text/html") == 0);
    CHECK(header(page, "Content-Security-Policy") == "frame-ancestors 'self' https://designer.example");
    CHECK(header(page, "X-Frame-Options").empty());
    CHECK(f.call("GET", "/widget/scope", "controls=yes").status == 400);
    CHECK(f.call("GET", "/widget/scope", "theme=blue").status == 400);
    CHECK(f.call("GET", "/widget/scope", "channel=1").status == 400);
    CHECK(f.call("GET", "/widget/nope").status == 404);
}

TEST_CASE("widgets: origins and queries")
{
    CHECK(listedOrigin("'self' https://a.example", "https://A.example/"));
    CHECK_FALSE(listedOrigin("'self' https://a.example", "null"));
    CHECK(listedOrigin("*", "https://any.example"));
    auto const q = parseQuery("controls=true&theme=dark%20x&x");
    CHECK(q.at("controls") == "true");
    CHECK(q.at("theme") == "dark x");
    CHECK(q.at("x").empty());
}
