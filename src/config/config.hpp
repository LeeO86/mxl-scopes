#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "scope/settings.hpp"

namespace scopes
{
class ConfigError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// When a changed key takes effect (SPECIFICATION §8): at the next update, live with a new
// output flow id, or after a restart.
enum class Apply
{
    Live,
    Flow,
    Restart
};

struct Rate
{
    int num = 50;
    int den = 1;
    [[nodiscard]] double hz() const
    {
        return static_cast<double>(num) / static_cast<double>(den);
    }
    [[nodiscard]] bool operator==(Rate const& other) const
    {
        return num == other.num && den == other.den;
    }
};

// The output flow (SPECIFICATION §7): raster, rate (empty: the input's) and key. A change mints
// a new flow id; it applies live (the writer opens the new flow, the renderer draws for it).
struct OutputSettings
{
    int width = 1920;
    int height = 1080;
    std::optional<Rate> rate;
    bool key = false;
    [[nodiscard]] bool operator==(OutputSettings const& other) const
    {
        return width == other.width && height == other.height && rate == other.rate && key == other.key;
    }
};

struct Config
{
    DisplaySettings display;

    // Output (§7).
    int outputWidth = 1920;
    int outputHeight = 1080;
    // Empty: follow the input ("input"); else the fixed rate.
    std::optional<Rate> outputRate;
    bool outputKey = false;

    int threads = 1;
    std::string backend = "auto";
    std::string simd = "auto";
    std::string previewMode = "webrtc";
    int previewHeight = 540;
    std::string stateDir = "/config";
    std::string configFile;

    // Platform settings (§8.2).
    std::string hostId;
    std::string scanPath = "/Volumes/mxl";
    std::string outputDomainDir;
    std::string outputDomainId;
    std::uint64_t historyNs = 200000000;
    bool cleanupOnExit = false;
    bool nmosEnable = true;
    std::string nmosRegistryAddress;
    int nmosRegistryPort = 3210;
    std::string nmosQueryAddress;
    int nmosQueryPort = 3211;
    bool nmosDnsSd = false;
    int nmosPort = 3322;
    std::string nmosSeed;
    std::string nmosLabel;
    std::string nmosHostAddress;
    std::map<std::string, std::vector<std::string>> nmosTags;
    bool webEnable = true;
    int webPort = 8170;
    std::string widgetFrameAncestors = "'self'";
    std::string previewPublishUrl;
    std::string previewPathPrefix = "mxl-scopes";
    std::string previewWhepUrl;
    std::string previewHlsUrl;
    int mediamtxRtspPort = 8574;
    int mediamtxWhepPort = 8579;
    int mediamtxHlsPort = 8578;
    int mediamtxIcePort = 8279;
    std::string logLevel = "info";
    std::string logFormat = "json";
    int shutdownTimeoutS = 10;
};
inline OutputSettings outputSettingsOf(Config const& config)
{
    return OutputSettings{config.outputWidth, config.outputHeight, config.outputRate, config.outputKey};
}


struct SettingDef
{
    char const* name;
    char const* defaultValue;
    Apply apply = Apply::Restart;
};

std::vector<SettingDef> const& settingSchema();
std::optional<SettingDef> findSetting(std::string const& key);

// The JSON field of PATCH /api/v1/scope for a live display key and back ("mode" <-> SCOPES_MODE).
std::optional<std::string> scopeFieldKey(std::string const& field);
std::vector<std::pair<std::string, std::string>> const& scopeFields();

// `25`, `50`, `2997`, `5994`, `2398`, `N/D`.
std::optional<Rate> parseRateToken(std::string const& token);
std::string rateToken(Rate const& rate);
// `frame`, or a rate in Hz between 1 and 60 (`25`, `12.5`). 0 means `frame`.
double parseUpdateRate(std::string const& text);

// env overrides file. Unknown keys in `file` throw. Environment variables that are not in
// the schema are ignored, so CI pins such as MXL_REF do not fail startup.
Config loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file);

std::string hostnameString();
bool announceableIpv4(std::string const& text);
} // namespace scopes
