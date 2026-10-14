#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scopes
{
enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warn,
    Error
};

void setLogLevel(LogLevel level);
void setLogFormatJson(bool json);
LogLevel parseLogLevel(std::string_view text);

void logMessage(LogLevel level, std::string_view event, std::vector<std::pair<std::string, std::string>> const& fields = {});

inline void logInfo(std::string_view event, std::vector<std::pair<std::string, std::string>> const& fields = {})
{
    logMessage(LogLevel::Info, event, fields);
}
inline void logWarn(std::string_view event, std::vector<std::pair<std::string, std::string>> const& fields = {})
{
    logMessage(LogLevel::Warn, event, fields);
}
inline void logError(std::string_view event, std::vector<std::pair<std::string, std::string>> const& fields = {})
{
    logMessage(LogLevel::Error, event, fields);
}

std::string jsonEscape(std::string_view text);
} // namespace scopes
