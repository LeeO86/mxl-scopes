#include <doctest/doctest.h>

#include "config/config.hpp"
#include "config/store.hpp"
#include "nmos/ids.hpp"
#include "util/uuid.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace scopes;

namespace
{
std::map<std::string, std::string> base()
{
    return {{"HOST_ID", "test-host"}, {"NMOS_HOST_ADDRESS", "192.0.2.10"}};
}

std::string tempDir()
{
    auto dir = std::filesystem::temp_directory_path() / ("scopes-test-" + std::to_string(::getpid()) + "-" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    return dir.string();
}
} // namespace

TEST_CASE("defaults")
{
    auto const cfg = loadConfig(base(), {});
    CHECK(cfg.display.mode == "luma");
    CHECK(cfg.display.updateRate == 25);
    CHECK(cfg.display.graticuleUnits == "mv");
    CHECK(cfg.display.traceColor == "green");
    CHECK(cfg.outputWidth == 1920);
    CHECK(cfg.outputHeight == 1080);
    CHECK_FALSE(cfg.outputRate.has_value());
    CHECK(cfg.webPort == 8170);
    CHECK(cfg.nmosPort == 3322);
    CHECK(cfg.mediamtxRtspPort == 8574);
    CHECK(cfg.mediamtxWhepPort == 8579);
    CHECK(cfg.mediamtxHlsPort == 8578);
    CHECK(cfg.mediamtxIcePort == 8279);
    CHECK(cfg.nmosSeed == "test-host-scopes");
    CHECK(cfg.outputDomainId == makeNmosIds("test-host-scopes").domain);
    CHECK(cfg.outputDomainDir == "/Volumes/mxl/scopes-" + cfg.outputDomainId.substr(0, 8));
    CHECK(cfg.stateDir == "/config");
    CHECK(cfg.nmosQueryPort == 3211);
}

TEST_CASE("environment beats the file, the file beats the defaults")
{
    auto env = base();
    env["SCOPES_UPDATE_RATE"] = "frame";
    auto const cfg = loadConfig(env, {{"SCOPES_UPDATE_RATE", "10"}, {"SCOPES_INTENSITY", "150"}});
    CHECK(cfg.display.updateRate == 0);
    CHECK(cfg.display.intensity == 150);
}

TEST_CASE("invalid values are configuration errors (exit 78)")
{
    std::vector<std::pair<std::string, std::string>> bad = {
        {"SCOPES_MODE", "waveform"},
        {"SCOPES_MODE", "vectorscope"},
        {"SCOPES_MODE", "ycbcr-overlay"},
        {"SCOPES_MODE", "vectorscope"},
        {"SCOPES_UPDATE_RATE", "0.5"},
        {"SCOPES_UPDATE_RATE", "61"},
        {"SCOPES_UPDATE_RATE", "fast"},
        {"SCOPES_LINE_STEP", "9"},
        {"SCOPES_PIXEL_STEP", "4"},
        {"SCOPES_RANGE", "wide"},
        {"SCOPES_GRATICULE_LEVEL", "101"},
        {"SCOPES_INTENSITY", "0"},
        {"SCOPES_TRACE_COLOR", "red"},
        {"SCOPES_OUTPUT_FORMAT", "1921x1080"},
        {"SCOPES_OUTPUT_FORMAT", "7680x4320"},
        {"SCOPES_OUTPUT_RATE", "120"},
        {"SCOPES_BACKEND", "cuda"},
        {"SCOPES_SIMD", "avx512vl"},
        {"SCOPES_THREADS", "9"},
        {"SCOPES_PREVIEW_HEIGHT", "541"},
        {"WEB_PORT", "0"},
        {"NMOS_HOST_ADDRESS", "127.0.0.1"},
        {"NMOS_TAGS", "[1]"},
        {"WIDGET_FRAME_ANCESTORS", "'self'; script-src *"},
        {"PREVIEW_PUBLISH_URL", "http://host"},
        {"LOG_LEVEL", "loud"},
    };
    for (auto const& [key, value] : bad)
    {
        CAPTURE(key);
        CAPTURE(value);
        auto env = base();
        env[key] = value;
        CHECK_THROWS_AS(loadConfig(env, {}), ConfigError);
    }
    CHECK_THROWS_AS(loadConfig(base(), {{"NOT_A_KEY", "1"}}), ConfigError);
}

TEST_CASE("SCOPES_THREADS 1-8 (strip workers, M2)")
{
    for (auto const* value : {"1", "2", "8"})
    {
        auto env = base();
        env["SCOPES_THREADS"] = value;
        CHECK(loadConfig(env, {}).threads == std::stoi(value));
    }
    auto env = base();
    env["SCOPES_THREADS"] = "0";
    CHECK_THROWS_AS(loadConfig(env, {}), ConfigError);
}

TEST_CASE("ports must not collide; MediaMTX ports count only in own mode")
{
    auto env = base();
    env["WEB_PORT"] = "3322";
    CHECK_THROWS_AS(loadConfig(env, {}), ConfigError);
    env = base();
    env["NMOS_PORT"] = "8169";
    env["WEB_PORT"] = "8170";
    CHECK_THROWS_AS(loadConfig(env, {}), ConfigError); // NMOS_PORT + 1
    env = base();
    env["MEDIAMTX_WHEP_PORT"] = "8170";
    CHECK_THROWS_AS(loadConfig(env, {}), ConfigError);
    env["PREVIEW_PUBLISH_URL"] = "rtsp://mediamtx:8554";
    CHECK_NOTHROW(loadConfig(env, {}));
}

TEST_CASE("unknown environment variables are ignored")
{
    auto env = base();
    env["MXL_REF"] = "abc";
    env["SCOPES_SOMETHING"] = "x";
    CHECK_NOTHROW(loadConfig(env, {}));
}

TEST_CASE("rates and update rates")
{
    CHECK(parseRateToken("5994")->num == 60000);
    CHECK(parseRateToken("5994")->den == 1001);
    CHECK(parseRateToken("30000/1001")->den == 1001);
    CHECK_FALSE(parseRateToken("12").has_value());
    CHECK(rateToken(Rate{50, 1}) == "50");
    CHECK(rateToken(Rate{60000, 1001}) == "5994");
    CHECK(parseUpdateRate("frame") == 0);
    CHECK(parseUpdateRate("12.5") == 12.5);
    auto env = base();
    env["SCOPES_OUTPUT_RATE"] = "2997";
    CHECK(loadConfig(env, {}).outputRate->num == 30000);
}

TEST_CASE("UUIDv5 ids are stable and distinct")
{
    auto const a = makeNmosIds("seed");
    auto const b = makeNmosIds("seed");
    CHECK(a.node == b.node);
    CHECK(isUuid(a.node));
    CHECK(a.node != a.device);
    CHECK(a.receiver != a.sender);
    CHECK(a.flow("1920x1080p50") != a.flow("1920x1080p5994"));
    CHECK(a.node[14] == '5');
    // RFC 4122 URL namespace, name "mxl-scopes/seed/node".
    CHECK(a.node == uuidV5(kUuidNamespaceUrl, "mxl-scopes/seed/node"));
}

TEST_CASE("store: file layer, atomic writes, live keys over the environment")
{
    auto const dir = tempDir();
    auto env = base();
    env["SCOPES_MODE"] = "luma";
    ConfigStore store(env);
    store.useFile(dir + "/config.json");
    auto updated = store.update({{"SCOPES_INTENSITY", std::string("200")}});
    REQUIRE(std::holds_alternative<ConfigStore::UpdateResult>(updated));
    CHECK(store.effectiveConfig().display.intensity == 200);
    CHECK(store.sourceOf("SCOPES_INTENSITY") == SettingSource::File);
    CHECK(std::filesystem::exists(dir + "/config.json"));
    // A key set in the environment cannot be saved to the file ...
    CHECK(std::holds_alternative<std::string>(store.update({{"SCOPES_MODE", std::string("luma")}})));
    // ... but the scope PATCH applies it live until the restart.
    auto live = store.applyLive({{"SCOPES_UPDATE_RATE", "frame"}, {"SCOPES_MODE", "luma"}});
    REQUIRE(std::holds_alternative<ConfigStore::UpdateResult>(live));
    CHECK(std::get<ConfigStore::UpdateResult>(live).notPersisted == std::vector<std::string>{"SCOPES_MODE"});
    CHECK(store.effectiveConfig().display.updateRate == 0);
    // Invalid values change nothing.
    CHECK(std::holds_alternative<std::string>(store.applyLive({{"SCOPES_INTENSITY", "999"}})));
    CHECK(store.effectiveConfig().display.intensity == 200);
    // Restart keys are not live.
    CHECK(std::holds_alternative<std::string>(store.applyLive({{"WEB_PORT", "8000"}})));
    auto restart = store.update({{"WEB_PORT", std::string("8000")}});
    REQUIRE(std::holds_alternative<ConfigStore::UpdateResult>(restart));
    CHECK(std::get<ConfigStore::UpdateResult>(restart).restartRequired == std::vector<std::string>{"WEB_PORT"});
    // A new store reads the file back.
    ConfigStore again(env);
    again.useFile(dir + "/config.json");
    CHECK(again.effectiveConfig().display.intensity == 200);
    CHECK(again.effectiveConfig().display.updateRate == 0);
    CHECK(again.renderEnvBlock().find("SCOPES_INTENSITY=200\n") != std::string::npos);
    std::filesystem::remove_all(dir);
}
