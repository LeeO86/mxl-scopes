#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "config/config.hpp"

namespace scopes
{
enum class SettingSource
{
    Default,
    File,
    Env,
    // A live change of a key the environment sets: in effect until the restart (PATCH /api/v1/scope).
    Live
};

char const* settingSourceName(SettingSource source);

// env > SCOPES_CONFIG_FILE (else <SCOPES_STATE_DIR>/config.json) > defaults. The file is
// flat JSON with string values and is written atomically.
class ConfigStore
{
public:
    explicit ConfigStore(std::map<std::string, std::string> env);

    [[nodiscard]] Config effectiveConfig() const;
    [[nodiscard]] SettingSource sourceOf(std::string const& key) const;
    [[nodiscard]] std::string effectiveValue(std::string const& key) const;
    [[nodiscard]] std::optional<std::string> filePath() const;
    // Called once the state directory is known, when SCOPES_CONFIG_FILE is empty.
    void useFile(std::string path);
    // KEY=value lines of every setting (GET /api/v1/config/env).
    [[nodiscard]] std::string renderEnvBlock() const;

    struct UpdateResult
    {
        Config config;
        std::vector<std::string> changedKeys;
        std::vector<std::string> restartRequired;
        std::vector<std::string> flowKeys;
        // Keys the environment sets: applied live, not saved (PATCH /api/v1/scope only).
        std::vector<std::string> notPersisted;
    };

    // PUT /api/v1/config: keys the environment sets are refused. A null value removes the key.
    std::variant<UpdateResult, std::string> update(std::map<std::string, std::optional<std::string>> const& changes);
    // PATCH /api/v1/scope: live keys only. Keys the environment sets apply until the restart.
    std::variant<UpdateResult, std::string> applyLive(std::map<std::string, std::string> const& changes);

private:
    [[nodiscard]] std::map<std::string, std::string> envLayer() const;
    std::variant<UpdateResult, std::string> commit(std::map<std::string, std::string> const& nextFile, std::map<std::string, std::string> const& nextLive,
        std::vector<std::string> const& keys);

    std::map<std::string, std::string> env_;
    std::optional<std::string> filePath_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> file_;
    std::map<std::string, std::string> live_;
};
} // namespace scopes
