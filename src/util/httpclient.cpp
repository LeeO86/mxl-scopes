#include "util/httpclient.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace scopes
{
HttpResult httpGet(std::string const& url, int timeoutMs)
{
    HttpResult result;
    auto const scheme = url.find("://");
    if (scheme == std::string::npos)
    {
        return result;
    }
    auto rest = url.substr(scheme + 3);
    auto const slash = rest.find('/');
    auto hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    auto const path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
    auto const colon = hostport.find(':');
    auto const host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    auto const port = colon == std::string::npos ? std::string("80") : hostport.substr(colon + 1);
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* info = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &info) != 0)
    {
        return result;
    }
    int fd = -1;
    for (auto* it = info; it != nullptr; it = it->ai_next)
    {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0)
        {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(info);
    if (fd < 0)
    {
        return result;
    }
    auto const request = "GET " + path + " HTTP/1.0\r\nHost: " + hostport + "\r\nConnection: close\r\n\r\n";
    if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) < 0)
    {
        ::close(fd);
        return result;
    }
    std::string raw;
    char buffer[2048];
    for (;;)
    {
        auto const n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
        {
            break;
        }
        raw.append(buffer, buffer + n);
    }
    ::close(fd);
    auto const lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos)
    {
        return result;
    }
    auto const code = raw.find(' ');
    if (code != std::string::npos)
    {
        result.status = std::atoi(raw.c_str() + code + 1);
    }
    auto const body = raw.find("\r\n\r\n");
    if (body != std::string::npos)
    {
        result.body = raw.substr(body + 4);
    }
    return result;
}
} // namespace scopes
