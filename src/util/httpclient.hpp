#pragma once

#include <string>

namespace scopes
{
struct HttpResult
{
    int status = 0;
    std::string body;
};

HttpResult httpGet(std::string const& url, int timeoutMs);
} // namespace scopes
