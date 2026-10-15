#include "ops/httpserver.hpp"

#include "util/sha1.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <list>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace scopes
{
namespace
{
constexpr std::size_t kMaxBodyBytes = 16u * 1024u * 1024u;
// A request head larger than this is refused (431) before it is parsed.
constexpr std::size_t kMaxHeaderBytes = 64u * 1024u;
// A WebSocket client with more than this queued is too slow and is dropped, so one stalled
// client never holds the server or the probes (§9.4).
constexpr std::size_t kMaxWebSocketQueue = 1024u * 1024u;
// An HTTP connection without progress (an incomplete request, a reply nobody reads) is closed
// after this long. Open WebSockets have no idle limit; their queue bound drops slow ones.
constexpr auto kIdleTimeout = std::chrono::seconds(30);

std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string statusText(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 415:
        return "Unsupported Media Type";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}

void setNonBlocking(int fd)
{
    int const flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string websocketFrame(std::string const& text)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    if (text.size() < 126)
    {
        frame.push_back(static_cast<char>(text.size()));
    }
    else if (text.size() < 65536)
    {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((text.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(text.size() & 0xff));
    }
    else
    {
        frame.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            frame.push_back(static_cast<char>((static_cast<std::uint64_t>(text.size()) >> shift) & 0xff));
        }
    }
    frame += text;
    return frame;
}

std::string httpMessage(HttpResponse const& response)
{
    std::string message = "HTTP/1.1 " + std::to_string(response.status) + " " + statusText(response.status) + "\r\nContent-Type: " + response.contentType +
                          "\r\nContent-Length: " + std::to_string(response.body.size()) + "\r\n";
    for (auto const& [name, value] : response.headers)
    {
        message += name + ": " + value + "\r\n";
    }
    message += "Connection: close\r\n\r\n" + response.body;
    return message;
}
} // namespace

// One poll thread owns every socket. All sockets are non-blocking; each connection has its
// own output queue that the poll thread drains when the socket can take more. broadcast()
// only appends to the queues (under connMu) and wakes the poll thread; nothing is ever sent
// while connMu is held.
struct HttpServer::Impl
{
    int listenFd = -1;
    int wakeFd = -1;
    int boundPort = 0;
    std::atomic<bool> run{false};
    std::thread thread;
    HttpHandler handler;
    struct Conn
    {
        int fd = -1;
        bool websocket = false;
        bool closeAfterWrite = false;
        bool peerClosed = false;
        std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
        std::atomic<bool> dead{false};
        std::string in;
        std::string out;
    };
    std::mutex connMu;
    std::list<Conn> conns;

    void wake()
    {
        std::uint64_t one = 1;
        if (wakeFd >= 0)
        {
            [[maybe_unused]] auto n = ::write(wakeFd, &one, sizeof(one));
        }
    }

    // Parses complete requests from conn.in; returns them (the connection is not touched by
    // other threads except broadcast(), which only appends to `out` under connMu).
    std::vector<HttpRequest> parse(Conn& conn)
    {
        std::vector<HttpRequest> requests;
        while (!conn.dead && !conn.websocket && !conn.closeAfterWrite)
        {
            auto const headerEnd = conn.in.find("\r\n\r\n");
            if (headerEnd == std::string::npos)
            {
                if (conn.in.size() > kMaxHeaderBytes)
                {
                    HttpResponse refused;
                    refused.status = 431;
                    refused.contentType = "application/json";
                    refused.body = "{\"error\":\"the request head is too large\"}";
                    std::lock_guard lock{connMu};
                    conn.out += httpMessage(refused);
                    conn.closeAfterWrite = true;
                    conn.in.clear();
                }
                break;
            }
            std::string const head = conn.in.substr(0, headerEnd);
            std::size_t bodyLen = 0;
            HttpRequest request;
            std::istringstream lines(head);
            std::string requestLine;
            std::getline(lines, requestLine);
            if (!requestLine.empty() && requestLine.back() == '\r')
            {
                requestLine.pop_back();
            }
            {
                std::istringstream rl(requestLine);
                std::string target;
                rl >> request.method >> target;
                auto const q = target.find('?');
                request.path = q == std::string::npos ? target : target.substr(0, q);
                request.query = q == std::string::npos ? std::string{} : target.substr(q + 1);
            }
            std::string line;
            while (std::getline(lines, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                auto const colon = line.find(':');
                if (colon == std::string::npos)
                {
                    continue;
                }
                auto key = lower(line.substr(0, colon));
                auto value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ')
                {
                    value.erase(value.begin());
                }
                if (key == "content-length")
                {
                    bodyLen = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
                }
                request.headers[key] = std::move(value);
            }
            if (bodyLen > kMaxBodyBytes)
            {
                HttpResponse refused;
                refused.status = 413;
                refused.contentType = "application/json";
                refused.body = "{\"error\":\"the body is too large\"}";
                std::lock_guard lock{connMu};
                conn.out += httpMessage(refused);
                conn.closeAfterWrite = true;
                conn.in.clear();
                break;
            }
            if (conn.in.size() < headerEnd + 4 + bodyLen)
            {
                break;
            }
            request.body = conn.in.substr(headerEnd + 4, bodyLen);
            conn.in.erase(0, headerEnd + 4 + bodyLen);
            requests.push_back(std::move(request));
            // One request per connection (Connection: close), as the responses say.
            break;
        }
        return requests;
    }

    void respond(Conn& conn, HttpRequest const& request)
    {
        HttpResponse response;
        if (handler)
        {
            response = handler(request);
        }
        std::lock_guard lock{connMu};
        if (response.websocket)
        {
            auto const keyIt = request.headers.find("sec-websocket-key");
            auto const key = keyIt == request.headers.end() ? std::string{} : keyIt->second;
            auto const digest = sha1(std::string_view(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
            conn.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                        base64Encode(digest.data(), digest.size()) + "\r\n\r\n";
            conn.websocket = true;
            return;
        }
        conn.out += httpMessage(response);
        conn.closeAfterWrite = true;
    }

    void loop()
    {
        std::vector<pollfd> fds;
        std::vector<Conn*> polled;
        while (run.load())
        {
            fds.clear();
            polled.clear();
            fds.push_back(pollfd{listenFd, POLLIN, 0});
            fds.push_back(pollfd{wakeFd, POLLIN, 0});
            {
                std::lock_guard lock{connMu};
                for (auto& conn : conns)
                {
                    // After the peer's EOF only the reply is left: POLLIN would fire forever.
                    short events = conn.peerClosed ? 0 : POLLIN;
                    if (!conn.out.empty())
                    {
                        events |= POLLOUT;
                    }
                    fds.push_back(pollfd{conn.fd, events, 0});
                    polled.push_back(&conn);
                }
            }
            if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), 200) < 0)
            {
                continue;
            }
            if ((fds[1].revents & POLLIN) != 0)
            {
                std::uint64_t drained = 0;
                [[maybe_unused]] auto n = ::read(wakeFd, &drained, sizeof(drained));
            }
            if ((fds[0].revents & POLLIN) != 0)
            {
                for (;;)
                {
                    int const fd = ::accept(listenFd, nullptr, nullptr);
                    if (fd < 0)
                    {
                        break;
                    }
                    setNonBlocking(fd);
                    std::lock_guard lock{connMu};
                    auto& conn = conns.emplace_back();
                    conn.fd = fd;
                    conn.lastActivity = std::chrono::steady_clock::now();
                }
            }
            for (std::size_t i = 0; i < polled.size(); ++i)
            {
                auto& conn = *polled[i];
                auto const revents = fds[i + 2].revents;
                if ((revents & (POLLERR | POLLNVAL)) != 0)
                {
                    conn.dead = true;
                    continue;
                }
                if ((revents & (POLLIN | POLLHUP)) != 0)
                {
                    char buf[16384];
                    for (;;)
                    {
                        auto const n = ::recv(conn.fd, buf, sizeof(buf), 0);
                        if (n > 0)
                        {
                            conn.lastActivity = std::chrono::steady_clock::now();
                            if (conn.websocket)
                            {
                                // Clients only ever send close frames to us; anything else is ignored.
                                if ((static_cast<unsigned char>(buf[0]) & 0x0f) == 0x8)
                                {
                                    conn.dead = true;
                                    break;
                                }
                                continue;
                            }
                            conn.in.append(buf, buf + n);
                            if (conn.in.size() > kMaxHeaderBytes + kMaxBodyBytes)
                            {
                                conn.dead = true;
                                break;
                            }
                            continue;
                        }
                        if (n == 0)
                        {
                            conn.peerClosed = true; // the answer may still go out
                        }
                        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        {
                            conn.dead = true;
                        }
                        break;
                    }
                    if (!conn.dead)
                    {
                        for (auto const& request : parse(conn))
                        {
                            respond(conn, request);
                        }
                    }
                }
                // Drain the output queue as far as the socket takes it; never under connMu.
                std::string pending;
                {
                    std::lock_guard lock{connMu};
                    pending.swap(conn.out);
                }
                std::size_t sent = 0;
                while (sent < pending.size())
                {
                    auto const n = ::send(conn.fd, pending.data() + sent, pending.size() - sent, MSG_NOSIGNAL);
                    if (n > 0)
                    {
                        sent += static_cast<std::size_t>(n);
                        conn.lastActivity = std::chrono::steady_clock::now();
                        continue;
                    }
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    {
                        break;
                    }
                    conn.dead = true;
                    break;
                }
                {
                    std::lock_guard lock{connMu};
                    conn.out.insert(0, pending, sent, std::string::npos);
                    if (conn.out.empty() && (conn.closeAfterWrite || conn.peerClosed))
                    {
                        conn.dead = true;
                    }
                }
            }
            auto const now = std::chrono::steady_clock::now();
            std::lock_guard lock{connMu};
            for (auto it = conns.begin(); it != conns.end();)
            {
                if (!it->websocket && now - it->lastActivity > kIdleTimeout)
                {
                    it->dead = true;
                }
                if (it->dead)
                {
                    ::close(it->fd);
                    it = conns.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
};

HttpServer::HttpServer()
    : impl_(new Impl)
{
}

HttpServer::~HttpServer()
{
    stop();
    delete impl_;
}

void HttpServer::start(int port, HttpHandler handler)
{
    stop();
    impl_->handler = std::move(handler);
    impl_->listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listenFd < 0)
    {
        throw std::runtime_error("socket failed");
    }
    int const one = 1;
    ::setsockopt(impl_->listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(impl_->listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(impl_->listenFd, 64) != 0)
    {
        ::close(impl_->listenFd);
        impl_->listenFd = -1;
        throw std::runtime_error("bind failed on port " + std::to_string(port));
    }
    setNonBlocking(impl_->listenFd);
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    ::getsockname(impl_->listenFd, reinterpret_cast<sockaddr*>(&bound), &len);
    impl_->boundPort = ntohs(bound.sin_port);
    impl_->wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    impl_->run.store(true);
    impl_->thread = std::thread([this] { impl_->loop(); });
}

void HttpServer::stop()
{
    if (impl_ == nullptr)
    {
        return;
    }
    impl_->run.store(false);
    impl_->wake();
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
    if (impl_->listenFd >= 0)
    {
        ::close(impl_->listenFd);
        impl_->listenFd = -1;
    }
    if (impl_->wakeFd >= 0)
    {
        ::close(impl_->wakeFd);
        impl_->wakeFd = -1;
    }
    std::lock_guard lock{impl_->connMu};
    for (auto& conn : impl_->conns)
    {
        ::close(conn.fd);
    }
    impl_->conns.clear();
}

int HttpServer::port() const
{
    return impl_->boundPort;
}

void HttpServer::broadcast(std::string const& text)
{
    auto const frame = websocketFrame(text);
    {
        std::lock_guard lock{impl_->connMu};
        for (auto& conn : impl_->conns)
        {
            if (!conn.websocket || conn.dead)
            {
                continue;
            }
            if (conn.out.size() + frame.size() > kMaxWebSocketQueue)
            {
                // Too slow: dropped rather than queued without bound.
                conn.dead = true;
                continue;
            }
            conn.out += frame;
        }
    }
    impl_->wake();
}
} // namespace scopes
