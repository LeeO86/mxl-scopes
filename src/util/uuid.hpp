#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scopes
{
std::optional<std::string> parseUuid(std::string_view text);
bool isUuid(std::string_view text);

// RFC 4122 name-based UUID (version 5, SHA-1).
std::string uuidV5(std::string_view namespaceUuid, std::string_view name);
} // namespace scopes
