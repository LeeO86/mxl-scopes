#include "util/logging.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>

namespace scopes
{
namespace
{
std::mutex gMu;
LogLevel gLevel = LogLevel::Info;
bool gJson = true;

char const* levelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}
} // namespace

void setLogLevel(LogLevel level)
{
    std::lock_guard lock{gMu};
    gLevel = level;
}

void setLogFormatJson(bool json)
{
    std::lock_guard lock{gMu};
    gJson = json;
}

LogLevel parseLogLevel(std::string_view text)
{
    if (text == "trace")
    {
        return LogLevel::Trace;
    }
    if (text == "debug")
    {
        return LogLevel::Debug;
    }
    if (text == "warn")
    {
        return LogLevel::Warn;
    }
    if (text == "error")
    {
        return LogLevel::Error;
    }
    return LogLevel::Info;
}

std::string jsonEscape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

void logMessage(LogLevel level, std::string_view event, std::vector<std::pair<std::string, std::string>> const& fields)
{
    std::lock_guard lock{gMu};
    if (static_cast<int>(level) < static_cast<int>(gLevel))
    {
        return;
    }
    auto const now = std::chrono::system_clock::now();
    auto const sec = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&sec, &tm);
    char ts[40];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
        tm.tm_sec);
    if (gJson)
    {
        std::cerr << "{\"ts\":\"" << ts << "\",\"level\":\"" << levelName(level) << "\",\"event\":\"" << jsonEscape(event) << "\"";
        for (auto const& field : fields)
        {
            std::cerr << ",\"" << jsonEscape(field.first) << "\":\"" << jsonEscape(field.second) << "\"";
        }
        std::cerr << "}\n";
    }
    else
    {
        std::cerr << ts << " " << levelName(level) << " " << event;
        for (auto const& field : fields)
        {
            std::cerr << " " << field.first << "=" << field.second;
        }
        std::cerr << "\n";
    }
}
} // namespace scopes
