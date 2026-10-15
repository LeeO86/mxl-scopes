#pragma once

#include <string>

namespace scopes
{
inline constexpr char kUuidNamespaceUrl[] = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";

// UUIDv5 ids from NMOS_SEED (SPECIFICATION §7.1): mxl-scopes/<seed>/<name>.
struct NmosIds
{
    std::string seed;
    std::string node;
    std::string device;
    std::string domain;
    std::string receiver;
    std::string source;
    std::string sender;
    // A new output format or rate mints a new flow id (§7).
    [[nodiscard]] std::string flow(std::string const& formatToken) const;
};

NmosIds makeNmosIds(std::string const& seed);
} // namespace scopes
