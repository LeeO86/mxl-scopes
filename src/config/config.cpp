#include "config/config.hpp"

#include "nmos/ids.hpp"
#include "util/jsonutil.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>

namespace scopes
{
namespace
{
std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool parseBool(std::string const& text, bool& out)
{
    auto const v = lower(text);
    if (v == "1" || v == "true" || v == "yes" || v == "on")
    {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off")
    {
        out = false;
        return true;
    }
    return false;
}

bool parseInt(std::string const& text, long& out)
{
    if (text.empty())
    {
        return false;
    }
    char* end = nullptr;
    out = std::strtol(text.c_str(), &end, 10);
    return end != nullptr && *end == '\0';
}

bool parseDouble(std::string const& text, double& out)
{
    if (text.empty())
    {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(out);
}

int requireInt(std::string const& key, std::string const& text, long min, long max)
{
    long value = 0;
    if (!parseInt(text, value) || value < min || value > max)
    {
        throw ConfigError(key + " must be an integer in [" + std::to_string(min) + "," + std::to_string(max) + "]");
    }
    return static_cast<int>(value);
}

bool requireBool(std::string const& key, std::string const& text)
{
    bool value = false;
    if (!parseBool(text, value))
    {
        throw ConfigError(key + " must be a boolean");
    }
    return value;
}

std::string requireEnum(std::string const& key, std::string const& text, std::vector<std::string> const& allowed)
{
    auto const value = lower(text);
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
    {
        std::string names;
        for (auto const& name : allowed)
        {
            names += (names.empty() ? "" : ", ") + name;
        }
        throw ConfigError(key + " must be one of " + names);
    }
    return value;
}

// Empty, or an absolute URL of one of `schemes` with a host, an optional port, and no path.
std::string normalizeBaseUrl(std::string const& key, std::string const& value, std::vector<std::string> const& schemes)
{
    if (value.empty())
    {
        return {};
    }
    auto const sep = value.find("://");
    auto const scheme = sep == std::string::npos ? std::string{} : lower(value.substr(0, sep));
    if (std::find(schemes.begin(), schemes.end(), scheme) == schemes.end())
    {
        throw ConfigError(key + " must be an absolute " + schemes.front() + " URL");
    }
    auto rest = value.substr(sep + 3);
    if (!rest.empty() && rest.back() == '/')
    {
        rest.pop_back();
    }
    if (rest.empty() || rest.find_first_of("/@?# ") != std::string::npos)
    {
        throw ConfigError(key + " must be an absolute URL with a host and no path");
    }
    return scheme + "://" + rest;
}

std::string normalizePathPrefix(std::string const& value)
{
    auto const first = value.find_first_not_of('/');
    if (first == std::string::npos)
    {
        return Config{}.previewPathPrefix;
    }
    auto const prefix = value.substr(first, value.find_last_not_of('/') - first + 1);
    std::stringstream stream(prefix);
    std::string segment;
    while (std::getline(stream, segment, '/'))
    {
        if (segment.empty() || segment == "." || segment == ".." ||
            segment.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~-") != std::string::npos)
        {
            throw ConfigError("PREVIEW_PATH_PREFIX must be path segments of letters, digits, '.', '_', '~' and '-' joined by '/'");
        }
    }
    return prefix;
}

void checkPorts(Config const& cfg)
{
    std::vector<std::pair<std::string, int>> ports{{"WEB_PORT", cfg.webPort}};
    if (cfg.nmosEnable)
    {
        ports.emplace_back("NMOS_PORT", cfg.nmosPort);
        ports.emplace_back("NMOS_PORT+1", cfg.nmosPort + 1);
    }
    // The MediaMTX ports count only in own mode (no PREVIEW_PUBLISH_URL).
    if (cfg.previewMode == "webrtc" && cfg.previewPublishUrl.empty())
    {
        ports.emplace_back("MEDIAMTX_RTSP_PORT", cfg.mediamtxRtspPort);
        ports.emplace_back("MEDIAMTX_WHEP_PORT", cfg.mediamtxWhepPort);
        ports.emplace_back("MEDIAMTX_HLS_PORT", cfg.mediamtxHlsPort);
        ports.emplace_back("MEDIAMTX_ICE_UDP_PORT", cfg.mediamtxIcePort);
    }
    for (std::size_t i = 0; i < ports.size(); ++i)
    {
        for (std::size_t j = i + 1; j < ports.size(); ++j)
        {
            if (ports[i].second == ports[j].second)
            {
                throw ConfigError(ports[i].first + " collides with " + ports[j].first);
            }
        }
    }
}

std::string firstNonLoopbackIpv4()
{
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
    {
        return {};
    }
    std::string found;
    for (auto* item = list; item != nullptr; item = item->ifa_next)
    {
        if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        char buffer[INET_ADDRSTRLEN] = {};
        auto const* address = reinterpret_cast<sockaddr_in const*>(item->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)) == nullptr)
        {
            continue;
        }
        if (announceableIpv4(buffer))
        {
            found = buffer;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

std::map<std::string, std::vector<std::string>> parseNmosTags(std::string const& text)
{
    std::map<std::string, std::vector<std::string>> tags;
    if (text.empty() || text == "{}")
    {
        return tags;
    }
    std::string error;
    auto const root = json::parse(text, error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("NMOS_TAGS must be a JSON object of string arrays");
    }
    for (auto const& [name, value] : root.get<picojson::object>())
    {
        if (!value.is<picojson::array>())
        {
            throw ConfigError("NMOS_TAGS value for " + name + " must be an array of strings");
        }
        for (auto const& item : value.get<picojson::array>())
        {
            if (!item.is<std::string>())
            {
                throw ConfigError("NMOS_TAGS value for " + name + " must be an array of strings");
            }
            tags[name].push_back(item.get<std::string>());
        }
    }
    return tags;
}
} // namespace

bool announceableIpv4(std::string const& text)
{
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1)
    {
        return false;
    }
    return text != "0.0.0.0" && text.rfind("127.", 0) != 0;
}

std::string hostnameString()
{
    char buffer[256] = {};
    if (gethostname(buffer, sizeof(buffer) - 1) != 0)
    {
        return "scopes";
    }
    return buffer;
}

std::optional<Rate> parseRateToken(std::string const& token)
{
    if (token == "2398")
    {
        return Rate{24000, 1001};
    }
    if (token == "2997")
    {
        return Rate{30000, 1001};
    }
    if (token == "5994")
    {
        return Rate{60000, 1001};
    }
    long num = 0;
    long den = 1;
    auto const slash = token.find('/');
    if (slash != std::string::npos)
    {
        if (!parseInt(token.substr(0, slash), num) || !parseInt(token.substr(slash + 1), den))
        {
            return std::nullopt;
        }
    }
    else if (!parseInt(token, num))
    {
        return std::nullopt;
    }
    if (num <= 0 || den <= 0 || num > 1000000 || den > 1000000)
    {
        return std::nullopt;
    }
    Rate rate{static_cast<int>(num), static_cast<int>(den)};
    if (rate.hz() < 23.0 || rate.hz() > 60.5)
    {
        return std::nullopt;
    }
    return rate;
}

std::string rateToken(Rate const& rate)
{
    if (rate.den == 1)
    {
        return std::to_string(rate.num);
    }
    if (rate.den == 1001 && (rate.num == 24000 || rate.num == 30000 || rate.num == 60000))
    {
        return rate.num == 24000 ? "2398" : rate.num == 30000 ? "2997" : "5994";
    }
    return std::to_string(rate.num) + "/" + std::to_string(rate.den);
}

double parseUpdateRate(std::string const& text)
{
    auto const value = lower(text);
    if (value == "frame")
    {
        return 0;
    }
    double hz = 0;
    if (!parseDouble(value, hz) || hz < 1 || hz > 60)
    {
        throw ConfigError("SCOPES_UPDATE_RATE must be frame or a rate in Hz from 1 to 60");
    }
    return hz;
}

std::vector<SettingDef> const& settingSchema()
{
    static std::vector<SettingDef> const schema = {
        {"SCOPES_MODE", "luma", Apply::Live},
        {"SCOPES_UPDATE_RATE", "25", Apply::Live},
        {"SCOPES_ACCUMULATE", "latest", Apply::Live},
        {"SCOPES_LINE_STEP", "1", Apply::Live},
        {"SCOPES_PIXEL_STEP", "1", Apply::Live},
        {"SCOPES_RANGE", "limited", Apply::Live},
        {"SCOPES_GRATICULE_UNITS", "mv", Apply::Live},
        {"SCOPES_GRATICULE_LEVEL", "60", Apply::Live},
        {"SCOPES_INTENSITY", "100", Apply::Live},
        {"SCOPES_TRACE_MIN", "96", Apply::Live},
        {"SCOPES_TRACE_COLOR", "green", Apply::Live},
        {"SCOPES_CAPTION", "true", Apply::Live},
        {"SCOPES_LAYOUT", "single", Apply::Live},
        {"SCOPES_OUTPUT_FORMAT", "1920x1080", Apply::Flow},
        {"SCOPES_OUTPUT_RATE", "input", Apply::Flow},
        {"SCOPES_OUTPUT_KEY", "false", Apply::Flow},
        {"SCOPES_BG_ALPHA", "0", Apply::Live},
        {"SCOPES_INPUT_OFFSET_GRAINS", "0", Apply::Live},
        {"SCOPES_HOLD_MS", "1000", Apply::Live},
        {"SCOPES_THREADS", "1", Apply::Restart},
        {"SCOPES_BACKEND", "auto", Apply::Restart},
        {"SCOPES_SIMD", "auto", Apply::Restart},
        {"SCOPES_PREVIEW_MODE", "webrtc", Apply::Restart},
        {"SCOPES_PREVIEW_FPS", "25", Apply::Live},
        {"SCOPES_PREVIEW_HEIGHT", "540", Apply::Restart},
        {"SCOPES_STATE_DIR", "/config", Apply::Restart},
        {"SCOPES_CONFIG_FILE", "", Apply::Restart},
        {"HOST_ID", "", Apply::Restart},
        {"MXL_DOMAIN_SCAN_PATH", "/Volumes/mxl", Apply::Restart},
        {"MXL_OUTPUT_DOMAIN_DIR", "", Apply::Restart},
        {"MXL_OUTPUT_DOMAIN_ID", "", Apply::Restart},
        {"MXL_HISTORY_DURATION_NS", "200000000", Apply::Restart},
        {"MXL_CLEANUP_ON_EXIT", "false", Apply::Restart},
        {"NMOS_ENABLE", "true", Apply::Restart},
        {"NMOS_REGISTRY_ADDRESS", "", Apply::Restart},
        {"NMOS_REGISTRY_PORT", "3210", Apply::Restart},
        {"NMOS_QUERY_ADDRESS", "", Apply::Restart},
        {"NMOS_QUERY_PORT", "", Apply::Restart},
        {"NMOS_DNS_SD", "false", Apply::Restart},
        {"NMOS_PORT", "3322", Apply::Restart},
        {"NMOS_SEED", "", Apply::Restart},
        {"NMOS_LABEL", "", Apply::Restart},
        {"NMOS_HOST_ADDRESS", "", Apply::Restart},
        {"NMOS_TAGS", "{}", Apply::Restart},
        {"WEB_ENABLE", "true", Apply::Restart},
        {"WEB_PORT", "8170", Apply::Restart},
        {"WIDGET_FRAME_ANCESTORS", "'self'", Apply::Restart},
        {"PREVIEW_PUBLISH_URL", "", Apply::Restart},
        {"PREVIEW_PATH_PREFIX", "mxl-scopes", Apply::Restart},
        {"PREVIEW_WHEP_URL", "", Apply::Restart},
        {"PREVIEW_HLS_URL", "", Apply::Restart},
        {"MEDIAMTX_RTSP_PORT", "8574", Apply::Restart},
        {"MEDIAMTX_WHEP_PORT", "8579", Apply::Restart},
        {"MEDIAMTX_HLS_PORT", "8578", Apply::Restart},
        {"MEDIAMTX_ICE_UDP_PORT", "8279", Apply::Restart},
        {"LOG_LEVEL", "info", Apply::Live},
        {"LOG_FORMAT", "json", Apply::Restart},
        {"SHUTDOWN_TIMEOUT_S", "10", Apply::Restart},
    };
    return schema;
}

std::optional<SettingDef> findSetting(std::string const& key)
{
    for (auto const& def : settingSchema())
    {
        if (key == def.name)
        {
            return def;
        }
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> const& scopeFields()
{
    static std::vector<std::pair<std::string, std::string>> const fields = {
        {"mode", "SCOPES_MODE"},
        {"update_rate", "SCOPES_UPDATE_RATE"},
        {"accumulate", "SCOPES_ACCUMULATE"},
        {"line_step", "SCOPES_LINE_STEP"},
        {"pixel_step", "SCOPES_PIXEL_STEP"},
        {"range", "SCOPES_RANGE"},
        {"graticule_units", "SCOPES_GRATICULE_UNITS"},
        {"graticule_level", "SCOPES_GRATICULE_LEVEL"},
        {"intensity", "SCOPES_INTENSITY"},
        {"trace_min", "SCOPES_TRACE_MIN"},
        {"trace_color", "SCOPES_TRACE_COLOR"},
        {"caption", "SCOPES_CAPTION"},
        {"layout", "SCOPES_LAYOUT"},
        {"bg_alpha", "SCOPES_BG_ALPHA"},
        {"input_offset_grains", "SCOPES_INPUT_OFFSET_GRAINS"},
        {"hold_ms", "SCOPES_HOLD_MS"},
        {"preview_fps", "SCOPES_PREVIEW_FPS"},
    };
    return fields;
}

std::optional<std::string> scopeFieldKey(std::string const& field)
{
    for (auto const& [name, key] : scopeFields())
    {
        if (name == field)
        {
            return key;
        }
    }
    return std::nullopt;
}

Config loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file)
{
    for (auto const& [key, value] : file)
    {
        (void)value;
        if (!findSetting(key))
        {
            throw ConfigError("unknown config key " + key);
        }
    }
    auto raw = [&](char const* key) {
        if (auto const it = env.find(key); it != env.end())
        {
            return it->second;
        }
        if (auto const it = file.find(key); it != file.end())
        {
            return it->second;
        }
        auto const def = findSetting(key);
        return def ? std::string(def->defaultValue) : std::string{};
    };

    Config cfg;
    auto& d = cfg.display;
    // Stage-1 modes after luma arrive in M2; stage-2 modes are not in v1 (§5.1, §8.1).
    d.mode = requireEnum("SCOPES_MODE", raw("SCOPES_MODE"), {"luma", "ycbcr-parade", "rgb-parade", "rgb-overlay", "ycbcr-overlay"});
    if (d.mode != "luma")
    {
        throw ConfigError("SCOPES_MODE " + d.mode + " is not implemented in this build (luma only until M2)");
    }
    d.updateRate = parseUpdateRate(raw("SCOPES_UPDATE_RATE"));
    d.accumulate = requireEnum("SCOPES_ACCUMULATE", raw("SCOPES_ACCUMULATE"), {"latest", "all"});
    if (d.accumulate != "latest")
    {
        throw ConfigError("SCOPES_ACCUMULATE=all is not implemented in this build (M1)");
    }
    d.lineStep = requireInt("SCOPES_LINE_STEP", raw("SCOPES_LINE_STEP"), 1, 8);
    d.pixelStep = requireInt("SCOPES_PIXEL_STEP", raw("SCOPES_PIXEL_STEP"), 1, 6);
    if (6 % d.pixelStep != 0)
    {
        throw ConfigError("SCOPES_PIXEL_STEP must be 1, 2, 3 or 6");
    }
    d.limitedRange = requireEnum("SCOPES_RANGE", raw("SCOPES_RANGE"), {"limited", "full"}) == "limited";
    d.graticuleUnits = requireEnum("SCOPES_GRATICULE_UNITS", raw("SCOPES_GRATICULE_UNITS"), {"mv", "percent", "code"});
    d.graticuleLevel = requireInt("SCOPES_GRATICULE_LEVEL", raw("SCOPES_GRATICULE_LEVEL"), 0, 100);
    d.intensity = requireInt("SCOPES_INTENSITY", raw("SCOPES_INTENSITY"), 1, 400);
    d.traceMin = requireInt("SCOPES_TRACE_MIN", raw("SCOPES_TRACE_MIN"), 0, 1023);
    d.traceColor = requireEnum("SCOPES_TRACE_COLOR", raw("SCOPES_TRACE_COLOR"), {"green", "white", "amber"});
    d.caption = requireBool("SCOPES_CAPTION", raw("SCOPES_CAPTION"));
    d.layout = requireEnum("SCOPES_LAYOUT", raw("SCOPES_LAYOUT"), {"single"});
    d.bgAlpha = requireInt("SCOPES_BG_ALPHA", raw("SCOPES_BG_ALPHA"), 0, 1023);
    d.inputOffsetGrains = requireInt("SCOPES_INPUT_OFFSET_GRAINS", raw("SCOPES_INPUT_OFFSET_GRAINS"), 0, 30);
    d.holdMs = requireInt("SCOPES_HOLD_MS", raw("SCOPES_HOLD_MS"), 0, 60000);
    d.previewFps = requireInt("SCOPES_PREVIEW_FPS", raw("SCOPES_PREVIEW_FPS"), 1, 30);

    auto const format = lower(raw("SCOPES_OUTPUT_FORMAT"));
    auto const x = format.find('x');
    long width = 0;
    long height = 0;
    if (x == std::string::npos || !parseInt(format.substr(0, x), width) || !parseInt(format.substr(x + 1), height))
    {
        throw ConfigError("SCOPES_OUTPUT_FORMAT must look like 1920x1080");
    }
    if (width % 2 != 0 || height % 2 != 0 || width < 320 || height < 180 || width > 3840 || height > 2160)
    {
        throw ConfigError("SCOPES_OUTPUT_FORMAT must be even, at least 320x180 and at most 3840x2160");
    }
    cfg.outputWidth = static_cast<int>(width);
    cfg.outputHeight = static_cast<int>(height);
    auto const outputRate = lower(raw("SCOPES_OUTPUT_RATE"));
    if (outputRate != "input")
    {
        cfg.outputRate = parseRateToken(outputRate);
        if (!cfg.outputRate)
        {
            throw ConfigError("SCOPES_OUTPUT_RATE must be input or a rate token (25, 50, 5994, N/D) between 23.98 and 60");
        }
    }
    cfg.outputKey = requireBool("SCOPES_OUTPUT_KEY", raw("SCOPES_OUTPUT_KEY"));
    if (cfg.outputKey)
    {
        throw ConfigError("SCOPES_OUTPUT_KEY=true is not implemented in this build (M2)");
    }
    cfg.threads = requireInt("SCOPES_THREADS", raw("SCOPES_THREADS"), 1, 8);
    if (cfg.threads != 1)
    {
        throw ConfigError("SCOPES_THREADS above 1 is not implemented in this build (M2)");
    }
    cfg.backend = requireEnum("SCOPES_BACKEND", raw("SCOPES_BACKEND"), {"auto", "cpu", "cuda"});
    if (cfg.backend == "cuda")
    {
        throw ConfigError("cuda_backend_not_built: SCOPES_BACKEND=cuda is reserved");
    }
    cfg.simd = requireEnum("SCOPES_SIMD", raw("SCOPES_SIMD"), {"auto", "avx512", "avx2", "scalar"});
    cfg.previewMode = requireEnum("SCOPES_PREVIEW_MODE", raw("SCOPES_PREVIEW_MODE"), {"webrtc", "off"});
    cfg.previewHeight = requireInt("SCOPES_PREVIEW_HEIGHT", raw("SCOPES_PREVIEW_HEIGHT"), 270, 1080);
    if (cfg.previewHeight % 2 != 0)
    {
        throw ConfigError("SCOPES_PREVIEW_HEIGHT must be even");
    }
    cfg.stateDir = raw("SCOPES_STATE_DIR");
    if (cfg.stateDir.empty())
    {
        throw ConfigError("SCOPES_STATE_DIR must not be empty");
    }
    cfg.configFile = raw("SCOPES_CONFIG_FILE");

    cfg.hostId = raw("HOST_ID");
    if (cfg.hostId.empty())
    {
        cfg.hostId = hostnameString();
    }
    cfg.scanPath = raw("MXL_DOMAIN_SCAN_PATH");
    if (cfg.scanPath.empty())
    {
        throw ConfigError("MXL_DOMAIN_SCAN_PATH must not be empty");
    }
    cfg.nmosSeed = raw("NMOS_SEED");
    if (cfg.nmosSeed.empty())
    {
        cfg.nmosSeed = cfg.hostId + "-scopes";
    }
    auto const ids = makeNmosIds(cfg.nmosSeed);
    cfg.outputDomainDir = raw("MXL_OUTPUT_DOMAIN_DIR");
    if (cfg.outputDomainDir.empty())
    {
        // §8.2: /Volumes/mxl/scopes-<seed-short>, the first eight hex digits of the domain id.
        cfg.outputDomainDir = cfg.scanPath + "/scopes-" + ids.domain.substr(0, 8);
    }
    cfg.outputDomainId = raw("MXL_OUTPUT_DOMAIN_ID");
    if (cfg.outputDomainId.empty())
    {
        cfg.outputDomainId = ids.domain;
    }
    long history = 0;
    if (!parseInt(raw("MXL_HISTORY_DURATION_NS"), history) || history < 1000000)
    {
        throw ConfigError("MXL_HISTORY_DURATION_NS must be at least 1000000");
    }
    cfg.historyNs = static_cast<std::uint64_t>(history);
    cfg.cleanupOnExit = requireBool("MXL_CLEANUP_ON_EXIT", raw("MXL_CLEANUP_ON_EXIT"));

    cfg.nmosEnable = requireBool("NMOS_ENABLE", raw("NMOS_ENABLE"));
    cfg.nmosRegistryAddress = raw("NMOS_REGISTRY_ADDRESS");
    cfg.nmosRegistryPort = requireInt("NMOS_REGISTRY_PORT", raw("NMOS_REGISTRY_PORT"), 1, 65535);
    cfg.nmosQueryAddress = raw("NMOS_QUERY_ADDRESS");
    if (cfg.nmosQueryAddress.empty())
    {
        cfg.nmosQueryAddress = cfg.nmosRegistryAddress;
    }
    auto const queryPort = raw("NMOS_QUERY_PORT");
    if (queryPort.empty())
    {
        if (cfg.nmosRegistryPort >= 65535)
        {
            throw ConfigError("NMOS_QUERY_PORT defaults to NMOS_REGISTRY_PORT + 1, which is not a valid port; set NMOS_QUERY_PORT");
        }
        cfg.nmosQueryPort = cfg.nmosRegistryPort + 1;
    }
    else
    {
        cfg.nmosQueryPort = requireInt("NMOS_QUERY_PORT", queryPort, 1, 65535);
    }
    cfg.nmosDnsSd = requireBool("NMOS_DNS_SD", raw("NMOS_DNS_SD"));
    cfg.nmosPort = requireInt("NMOS_PORT", raw("NMOS_PORT"), 1, 65534);
    cfg.nmosLabel = raw("NMOS_LABEL");
    cfg.nmosHostAddress = raw("NMOS_HOST_ADDRESS");
    if (cfg.nmosHostAddress.empty())
    {
        cfg.nmosHostAddress = firstNonLoopbackIpv4();
    }
    if (cfg.nmosEnable && !announceableIpv4(cfg.nmosHostAddress))
    {
        throw ConfigError("NMOS_HOST_ADDRESS must be a non-loopback IPv4 address");
    }
    cfg.nmosTags = parseNmosTags(raw("NMOS_TAGS"));
    cfg.webEnable = requireBool("WEB_ENABLE", raw("WEB_ENABLE"));
    cfg.webPort = requireInt("WEB_PORT", raw("WEB_PORT"), 1, 65535);
    cfg.widgetFrameAncestors = raw("WIDGET_FRAME_ANCESTORS");
    if (cfg.widgetFrameAncestors.empty())
    {
        cfg.widgetFrameAncestors = Config{}.widgetFrameAncestors;
    }
    for (unsigned char const c : cfg.widgetFrameAncestors)
    {
        if (c < 0x20 || c == 0x7f || c == ';' || c == ',')
        {
            throw ConfigError("WIDGET_FRAME_ANCESTORS must be a CSP source list, e.g. 'self' https://designer.example");
        }
    }
    cfg.previewPublishUrl = normalizeBaseUrl("PREVIEW_PUBLISH_URL", raw("PREVIEW_PUBLISH_URL"), {"rtsp", "rtsps"});
    cfg.previewPathPrefix = normalizePathPrefix(raw("PREVIEW_PATH_PREFIX"));
    cfg.previewWhepUrl = normalizeBaseUrl("PREVIEW_WHEP_URL", raw("PREVIEW_WHEP_URL"), {"http", "https"});
    cfg.previewHlsUrl = normalizeBaseUrl("PREVIEW_HLS_URL", raw("PREVIEW_HLS_URL"), {"http", "https"});
    cfg.mediamtxRtspPort = requireInt("MEDIAMTX_RTSP_PORT", raw("MEDIAMTX_RTSP_PORT"), 1, 65535);
    cfg.mediamtxWhepPort = requireInt("MEDIAMTX_WHEP_PORT", raw("MEDIAMTX_WHEP_PORT"), 1, 65535);
    cfg.mediamtxHlsPort = requireInt("MEDIAMTX_HLS_PORT", raw("MEDIAMTX_HLS_PORT"), 1, 65535);
    cfg.mediamtxIcePort = requireInt("MEDIAMTX_ICE_UDP_PORT", raw("MEDIAMTX_ICE_UDP_PORT"), 1, 65535);
    cfg.logLevel = requireEnum("LOG_LEVEL", raw("LOG_LEVEL"), {"trace", "debug", "info", "warn", "error"});
    cfg.logFormat = requireEnum("LOG_FORMAT", raw("LOG_FORMAT"), {"json", "text"});
    cfg.shutdownTimeoutS = requireInt("SHUTDOWN_TIMEOUT_S", raw("SHUTDOWN_TIMEOUT_S"), 1, 120);
    checkPorts(cfg);
    return cfg;
}
} // namespace scopes
