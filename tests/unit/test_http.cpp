// The HTTP server never blocks on a slow client: a WebSocket client that stops reading is
// dropped, and the probes keep answering (SPECIFICATION §9.4).
#include <doctest/doctest.h>

#include "ops/httpserver.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

using namespace scopes;

namespace
{
int connectTo(int port)
{
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

// Sends `request` and reads until the peer closes or `timeoutMs` passes.
std::string exchange(int port, std::string const& request, int timeoutMs)
{
    int const fd = connectTo(port);
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string answer;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 50) <= 0)
        {
            continue;
        }
        char buf[4096];
        auto const n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        answer.append(buf, buf + n);
    }
    ::close(fd);
    return answer;
}
} // namespace

TEST_CASE("a WebSocket client that stops reading does not stall the server")
{
    HttpServer server;
    server.start(0, [](HttpRequest const& request) {
        HttpResponse response;
        if (request.path == "/ws")
        {
            response.websocket = true;
            return response;
        }
        response.body = "ok";
        return response;
    });
    int const port = server.port();
    REQUIRE(port > 0);
    // The stalled client: upgrades, then never reads again.
    int const stalled = connectTo(port);
    std::string const upgrade = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    ::send(stalled, upgrade.data(), upgrade.size(), MSG_NOSIGNAL);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string const big(64 * 1024, 'x');
    auto const t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i)
    {
        server.broadcast(big); // 12.8 MB in all: far more than the socket buffers hold
    }
    auto const broadcastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(broadcastMs < 1000);
    auto const t1 = std::chrono::steady_clock::now();
    auto const answer = exchange(port, "GET /livez HTTP/1.1\r\nHost: x\r\n\r\n", 2000);
    auto const probeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    CHECK(answer.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(probeMs < 1000);
    ::close(stalled);
    server.stop();
}

TEST_CASE("an oversized request head is refused with 431")
{
    HttpServer server;
    server.start(0, [](HttpRequest const&) { return HttpResponse{}; });
    std::string request = "GET / HTTP/1.1\r\nX-Big: " + std::string(70 * 1024, 'a');
    auto const answer = exchange(server.port(), request, 2000);
    CHECK(answer.rfind("HTTP/1.1 431", 0) == 0);
    server.stop();
}

TEST_CASE("a client that half-closes after its request gets the answer")
{
    HttpServer server;
    server.start(0, [](HttpRequest const&) {
        HttpResponse response;
        response.body = std::string(256 * 1024, 'y'); // more than one send
        return response;
    });
    int const fd = connectTo(server.port());
    std::string const request = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    ::shutdown(fd, SHUT_WR); // EOF on the server side while the reply drains
    std::size_t received = 0;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        char buf[65536];
        auto const n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        received += static_cast<std::size_t>(n);
    }
    ::close(fd);
    CHECK(received > 256 * 1024);
    server.stop();
}
