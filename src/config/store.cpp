#include "config/store.hpp"

#include "util/jsonutil.hpp"

#include <cstdio>
#include <fstream>

namespace scopes
{
namespace
{
std::map<std::string, std::string> readFlatFile(std::string const& path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw ConfigError("cannot read config file " + path);
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    auto const root = json::parse(body, error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("config file " + path + " is not a JSON object");
    }
    std::map<std::string, std::string> out;
    for (auto const& [key, value] : root.get<picojson::object>())
    {
        if (!value.is<std::string>())
        {
            throw ConfigError("config value for " + key + " must be a string");
        }
        out.emplace(key, value.get<std::string>());
    }
    return out;
}

void writeFlatFile(std::string const& path, std::map<std::string, std::string> const& values)
{
    picojson::object obj;
    for (auto const& [key, value] : values)
    {
        obj[key] = picojson::value(value);
    }
    auto const tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            throw ConfigError("cannot write config file " + path);
        }
        out << picojson::value(obj).serialize(true);
        if (!out.flush())
        {
            throw ConfigError("cannot write config file " + path);
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        throw ConfigError("cannot replace config file " + path);
    }
}
} // namespace

char const* settingSourceName(SettingSource source)
{
    switch (source)
    {
    case SettingSource::Env:
        return "environment";
    case SettingSource::File:
        return "file";
    case SettingSource::Live:
        return "live";
    case SettingSource::Default:
        return "default";
    }
    return "default";
}

ConfigStore::ConfigStore(std::map<std::string, std::string> env)
    : env_(std::move(env))
{
    if (auto const it = env_.find("SCOPES_CONFIG_FILE"); it != env_.end() && !it->second.empty())
    {
        useFile(it->second);
    }
}

void ConfigStore::useFile(std::string path)
{
    std::lock_guard lock{mutex_};
    if (filePath_)
    {
        return;
    }
    filePath_ = std::move(path);
    std::ifstream probe(*filePath_);
    if (probe)
    {
        file_ = readFlatFile(*filePath_);
    }
}

std::map<std::string, std::string> ConfigStore::envLayer() const
{
    auto layer = env_;
    for (auto const& [key, value] : live_)
    {
        layer[key] = value;
    }
    return layer;
}

Config ConfigStore::effectiveConfig() const
{
    std::lock_guard lock{mutex_};
    return loadConfig(envLayer(), file_);
}

SettingSource ConfigStore::sourceOf(std::string const& key) const
{
    std::lock_guard lock{mutex_};
    if (live_.count(key) != 0)
    {
        return SettingSource::Live;
    }
    if (env_.count(key) != 0)
    {
        return SettingSource::Env;
    }
    if (file_.count(key) != 0)
    {
        return SettingSource::File;
    }
    return SettingSource::Default;
}

std::string ConfigStore::effectiveValue(std::string const& key) const
{
    std::lock_guard lock{mutex_};
    if (auto const it = live_.find(key); it != live_.end())
    {
        return it->second;
    }
    if (auto const it = env_.find(key); it != env_.end())
    {
        return it->second;
    }
    if (auto const it = file_.find(key); it != file_.end())
    {
        return it->second;
    }
    auto const def = findSetting(key);
    return def ? def->defaultValue : "";
}

std::optional<std::string> ConfigStore::filePath() const
{
    std::lock_guard lock{mutex_};
    return filePath_;
}

std::string ConfigStore::renderEnvBlock() const
{
    auto const cfg = effectiveConfig();
    std::string out;
    for (auto const& def : settingSchema())
    {
        std::string value = effectiveValue(def.name);
        // Defaults that are derived at start are written out as their effective value.
        std::string const name = def.name;
        if (value.empty())
        {
            if (name == "HOST_ID")
            {
                value = cfg.hostId;
            }
            else if (name == "NMOS_SEED")
            {
                value = cfg.nmosSeed;
            }
            else if (name == "MXL_OUTPUT_DOMAIN_DIR")
            {
                value = cfg.outputDomainDir;
            }
            else if (name == "MXL_OUTPUT_DOMAIN_ID")
            {
                value = cfg.outputDomainId;
            }
            else if (name == "NMOS_HOST_ADDRESS")
            {
                value = cfg.nmosHostAddress;
            }
        }
        out += name + "=" + value + "\n";
    }
    return out;
}

std::variant<ConfigStore::UpdateResult, std::string> ConfigStore::commit(std::map<std::string, std::string> const& nextFile,
    std::map<std::string, std::string> const& nextLive, std::vector<std::string> const& keys)
{
    try
    {
        auto layer = env_;
        for (auto const& [key, value] : nextLive)
        {
            layer[key] = value;
        }
        UpdateResult result;
        result.config = loadConfig(layer, nextFile);
        if (nextFile != file_)
        {
            if (!filePath_)
            {
                return std::string("no config file (SCOPES_CONFIG_FILE or the state directory)");
            }
            writeFlatFile(*filePath_, nextFile);
        }
        file_ = nextFile;
        live_ = nextLive;
        for (auto const& key : keys)
        {
            result.changedKeys.push_back(key);
            auto const def = findSetting(key);
            if (def && def->apply == Apply::Restart)
            {
                result.restartRequired.push_back(key);
            }
            if (def && def->apply == Apply::Flow)
            {
                result.flowKeys.push_back(key);
            }
        }
        return result;
    }
    catch (ConfigError const& ex)
    {
        return std::string(ex.what());
    }
}

std::variant<ConfigStore::UpdateResult, std::string> ConfigStore::update(std::map<std::string, std::optional<std::string>> const& changes)
{
    std::lock_guard lock{mutex_};
    auto next = file_;
    std::vector<std::string> keys;
    for (auto const& [key, value] : changes)
    {
        if (!findSetting(key))
        {
            return std::string("unknown config key " + key);
        }
        if (env_.count(key) != 0)
        {
            return std::string(key + " is set via the environment");
        }
        if (value)
        {
            next[key] = *value;
        }
        else
        {
            next.erase(key);
        }
        keys.push_back(key);
    }
    // A key saved to the file replaces an earlier live value of the same key.
    auto live = live_;
    for (auto const& key : keys)
    {
        live.erase(key);
    }
    return commit(next, live, keys);
}

std::variant<ConfigStore::UpdateResult, std::string> ConfigStore::applyLive(std::map<std::string, std::string> const& changes)
{
    std::lock_guard lock{mutex_};
    auto next = file_;
    auto live = live_;
    std::vector<std::string> keys;
    std::vector<std::string> notPersisted;
    for (auto const& [key, value] : changes)
    {
        auto const def = findSetting(key);
        if (!def || def->apply != Apply::Live)
        {
            return std::string(key + " is not a live display setting");
        }
        if (env_.count(key) != 0)
        {
            live[key] = value;
            notPersisted.push_back(key);
        }
        else
        {
            next[key] = value;
            live.erase(key);
        }
        keys.push_back(key);
    }
    auto result = commit(next, live, keys);
    if (auto* ok = std::get_if<UpdateResult>(&result))
    {
        ok->notPersisted = notPersisted;
    }
    return result;
}
} // namespace scopes
