#include "ops/widgets.hpp"

#include "version.hpp"

#include <cctype>
#include <sstream>

namespace scopes
{
std::string widgetsJson()
{
    return std::string(R"([{"id":"scope","title":"Video scope","params":{"type":"object","properties":{)") +
           R"("controls":{"type":"boolean","default":false,"title":"Mode and update rate"},)" +
           R"("measurements":{"type":"boolean","default":false,"title":"Luma min/max and out-of-range share"}}},)" +
           R"("min_size":{"w":320,"h":180},"version":")" + SCOPES_VERSION + R"("}])";
}

bool knownWidget(std::string const& id)
{
    return id == "scope";
}

std::string widgetQueryError(std::map<std::string, std::string> const& query)
{
    for (auto const* name : {"controls", "measurements"})
    {
        if (auto const it = query.find(name); it != query.end())
        {
            auto const& value = it->second;
            if (value != "true" && value != "false" && value != "1" && value != "0")
            {
                return std::string(name) + " must be true or false";
            }
        }
    }
    if (auto const it = query.find("theme"); it != query.end() && it->second != "dark" && it->second != "light" && it->second != "transparent")
    {
        return "theme must be dark, light or transparent";
    }
    for (auto const& [name, value] : query)
    {
        if (name != "controls" && name != "measurements" && name != "theme")
        {
            return "unknown parameter " + name;
        }
    }
    return {};
}

bool listedOrigin(std::string const& sources, std::string origin)
{
    auto const normal = [](std::string text) {
        for (char& c : text)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        while (!text.empty() && text.back() == '/')
        {
            text.pop_back();
        }
        return text;
    };
    origin = normal(origin);
    if (origin.empty() || origin == "null")
    {
        return false;
    }
    // A host wildcard as in frame-ancestors: `https://*.example.com` (or `*.example.com`, any
    // scheme) matches every subdomain, not example.com itself; a port must match as written.
    auto const scheme = origin.find("://");
    if (scheme == std::string::npos)
    {
        return false;
    }
    auto const hostPort = origin.substr(scheme + 3);
    auto const wildcard = [&](std::string const& token) {
        auto const at = token.find("://");
        auto const host = at == std::string::npos ? token : token.substr(at + 3);
        if (host.rfind("*.", 0) != 0 || (at != std::string::npos && token.compare(0, at, origin, 0, scheme) != 0))
        {
            return false;
        }
        auto const suffix = host.substr(1);
        bool const port = suffix.find(':') != std::string::npos;
        return hostPort.size() > suffix.size() && hostPort.compare(hostPort.size() - suffix.size(), suffix.size(), suffix) == 0 &&
               (port || hostPort.find(':') == std::string::npos);
    };
    std::istringstream tokens(sources);
    std::string token;
    while (tokens >> token)
    {
        if (token == "*" || normal(token) == origin || wildcard(normal(token)))
        {
            return true;
        }
    }
    return false;
}

std::map<std::string, std::string> parseQuery(std::string const& query)
{
    auto const decode = [](std::string const& text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '+')
            {
                out += ' ';
            }
            else if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                     std::isxdigit(static_cast<unsigned char>(text[i + 2])))
            {
                out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
                i += 2;
            }
            else
            {
                out += text[i];
            }
        }
        return out;
    };
    std::map<std::string, std::string> out;
    std::istringstream parts(query);
    std::string part;
    while (std::getline(parts, part, '&'))
    {
        if (part.empty())
        {
            continue;
        }
        auto const eq = part.find('=');
        out[decode(part.substr(0, eq))] = eq == std::string::npos ? std::string{} : decode(part.substr(eq + 1));
    }
    return out;
}
} // namespace scopes
