#include "domain/scan.hpp"

#include "util/jsonutil.hpp"

#include <filesystem>
#include <fstream>

namespace scopes
{
namespace
{
std::optional<picojson::value> readJson(std::filesystem::path const& file)
{
    std::ifstream in(file);
    if (!in)
    {
        return std::nullopt;
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    auto const root = json::parse(body, error);
    if (!error.empty())
    {
        return std::nullopt;
    }
    return root;
}

bool mirrorFlag(picojson::value const& root)
{
    if (!root.is<picojson::object>())
    {
        return false;
    }
    auto const& obj = root.get<picojson::object>();
    auto const it = obj.find("x-mxl-fabrics-agent");
    if (it == obj.end() || !it->second.is<picojson::object>())
    {
        return false;
    }
    auto const& marker = it->second.get<picojson::object>();
    auto const flag = marker.find("mirror");
    return flag != marker.end() && flag->second.is<bool>() && flag->second.get<bool>();
}
} // namespace

std::optional<std::string> readDomainId(std::string const& domainDir)
{
    auto const root = readJson(std::filesystem::path(domainDir) / "domain_def.json");
    if (!root)
    {
        return std::nullopt;
    }
    return json::fieldString(*root, "id");
}

bool isMirrorDomain(std::string const& domainDir)
{
    auto const root = readJson(std::filesystem::path(domainDir) / "domain_def.json");
    return root && mirrorFlag(*root);
}

std::vector<DomainInfo> scanDomains(std::string const& root)
{
    std::vector<DomainInfo> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
    {
        return out;
    }
    for (auto const& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec || !entry.is_directory())
        {
            continue;
        }
        auto const def = readJson(entry.path() / "domain_def.json");
        if (!def)
        {
            continue;
        }
        auto const id = json::fieldString(*def, "id");
        if (!id || id->empty())
        {
            continue;
        }
        DomainInfo info;
        info.path = entry.path().string();
        info.id = *id;
        info.label = json::fieldString(*def, "label").value_or("");
        info.mirror = mirrorFlag(*def);
        out.push_back(std::move(info));
    }
    return out;
}

std::optional<DomainInfo> resolveDomain(std::string const& root, std::string const& id)
{
    for (auto const& domain : scanDomains(root))
    {
        if (domain.id == id)
        {
            return domain;
        }
    }
    return std::nullopt;
}
} // namespace scopes
