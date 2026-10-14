#pragma once

#include <optional>
#include <string>
#include <vector>

namespace scopes
{
struct DomainInfo
{
    std::string path;
    std::string id;
    std::string label;
    bool mirror = false;
};

// Direct children of root that contain domain_def.json. Identity is the id field.
// Mirror domains (x-mxl-fabrics-agent.mirror == true) are included. Unknown JSON fields are ignored.
std::vector<DomainInfo> scanDomains(std::string const& root);
std::optional<DomainInfo> resolveDomain(std::string const& root, std::string const& id);
bool isMirrorDomain(std::string const& domainDir);
std::optional<std::string> readDomainId(std::string const& domainDir);
} // namespace scopes
