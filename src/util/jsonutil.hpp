#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <picojson/picojson.h>

namespace scopes::json
{
inline picojson::value parse(std::string_view text, std::string& error)
{
    picojson::value root;
    std::string const body(text);
    error = picojson::parse(root, body);
    return root;
}

inline std::optional<std::string> fieldString(picojson::value const& root, char const* key)
{
    if (!root.is<picojson::object>())
    {
        return std::nullopt;
    }
    auto const& obj = root.get<picojson::object>();
    auto const it = obj.find(key);
    if (it == obj.end() || !it->second.is<std::string>())
    {
        return std::nullopt;
    }
    return it->second.get<std::string>();
}

inline std::string stringify(picojson::value const& value)
{
    return value.serialize();
}
} // namespace scopes::json
