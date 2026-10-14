#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace scopes
{
struct HttpRequest
{
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;
};

struct HttpResponse
{
    int status = 200;
    std::string contentType = "text/plain";
    std::string body;
    bool websocket = false;
    // Extra response headers (CSP of the widget routes, CORS of /widgets).
    std::vector<std::pair<std::string, std::string>> headers{};
};

using HttpHandler = std::function<HttpResponse(HttpRequest const&)>;

class HttpServer
{
public:
    HttpServer();
    ~HttpServer();
    HttpServer(HttpServer const&) = delete;
    HttpServer& operator=(HttpServer const&) = delete;

    void start(int port, HttpHandler handler);
    void stop();
    [[nodiscard]] int port() const;
    void broadcast(std::string const& text);

private:
    struct Impl;
    Impl* impl_;
};
} // namespace scopes
