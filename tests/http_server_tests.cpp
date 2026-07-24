#include "core/logging.h"
#include "http_server/http_server.h"
#include "platform/windows/socket_handle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace
{

using iocp::platform::windows::SocketHandle;

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void SendAll(const SOCKET socket, const std::string_view bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const int sent = ::send(
            socket,
            bytes.data() + offset,
            static_cast<int>(bytes.size() - offset),
            0);
        if (sent == SOCKET_ERROR)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "send(HTTP integration test)");
        }
        Check(sent > 0, "HTTP test send made no progress");
        offset += static_cast<std::size_t>(sent);
    }
}

SocketHandle Connect(const std::uint16_t port)
{
    SocketHandle socket(
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Check(static_cast<bool>(socket), "HTTP client socket creation failed");

    const DWORD timeout = 3000;
    Check(
        ::setsockopt(
            socket.Get(),
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout),
            sizeof(timeout)) != SOCKET_ERROR,
        "HTTP client receive timeout setup failed");

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(port);
    endpoint.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(
            socket.Get(),
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)) == SOCKET_ERROR)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "connect(HTTP integration test)");
    }
    return socket;
}

struct HttpWireResponse final
{
    std::string head;
    std::string body;
};

class SocketReader final
{
public:
    explicit SocketReader(const SOCKET socket)
        : socket_(socket)
    {
    }

    HttpWireResponse ReadResponse()
    {
        constexpr std::string_view delimiter = "\r\n\r\n";
        std::size_t header_end = pending_.find(delimiter);
        while (header_end == std::string::npos)
        {
            ReadMore();
            header_end = pending_.find(delimiter);
        }
        header_end += delimiter.size();

        constexpr std::string_view name = "Content-Length: ";
        const std::size_t value_start = pending_.find(name);
        Check(
            value_start != std::string::npos &&
                value_start < header_end,
            "HTTP response omitted Content-Length");
        const std::size_t digits_start =
            value_start + name.size();
        const std::size_t digits_end =
            pending_.find("\r\n", digits_start);
        Check(
            digits_end != std::string::npos &&
                digits_end < header_end,
            "HTTP response Content-Length was malformed");

        std::size_t content_length = 0;
        const char* first = pending_.data() + digits_start;
        const char* last = pending_.data() + digits_end;
        const auto parsed =
            std::from_chars(first, last, content_length);
        Check(
            parsed.ec == std::errc{} && parsed.ptr == last,
            "HTTP response Content-Length was not numeric");

        while (pending_.size() < header_end + content_length)
        {
            ReadMore();
        }

        HttpWireResponse response{
            pending_.substr(0, header_end),
            pending_.substr(header_end, content_length),
        };
        pending_.erase(0, header_end + content_length);
        return response;
    }

    bool WaitForPeerClose()
    {
        if (!pending_.empty())
        {
            return false;
        }
        char byte = '\0';
        const int received = ::recv(socket_, &byte, 1, 0);
        return received == 0;
    }

private:
    void ReadMore()
    {
        char chunk[4096];
        const int received =
            ::recv(socket_, chunk, sizeof(chunk), 0);
        if (received == SOCKET_ERROR)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "recv(HTTP integration test)");
        }
        Check(received > 0, "HTTP peer closed before response completed");
        pending_.append(
            chunk,
            static_cast<std::size_t>(received));
    }

    SOCKET socket_;
    std::string pending_;
};

void TestBasicServiceOverTcp()
{
    auto logger = std::make_shared<iocp::core::Logger>();
    iocp::server::HttpServerOptions options;
    options.listener.port = 0;
    options.io_worker_count = 2;
    options.application_worker_count = 2;
    auto server =
        iocp::server::HttpServer::Create(logger, options);

    SocketHandle client = Connect(server->LocalPort());
    SocketReader reader(client.Get());
    SendAll(
        client.Get(),
        "GET /health HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n"
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello");

    const HttpWireResponse health = reader.ReadResponse();
    Check(
        health.head.find("HTTP/1.1 200 OK\r\n") == 0 &&
            health.head.find("Connection: keep-alive\r\n") !=
                std::string::npos &&
            health.body == "{\"status\":\"ok\"}\n",
        "GET /health response was incorrect");

    const HttpWireResponse echo = reader.ReadResponse();
    Check(
        echo.head.find("HTTP/1.1 200 OK\r\n") == 0 &&
            echo.head.find("Content-Type: text/plain\r\n") !=
                std::string::npos &&
            echo.head.find("Connection: close\r\n") !=
                std::string::npos &&
            echo.body == "hello",
        "POST /echo response was incorrect");
    Check(
        reader.WaitForPeerClose(),
        "HTTP connection did not close after the final response");

    client.Reset();
    Check(server->Stop(), "HTTP server did not stop cleanly");
}

void TestProtocolErrorResponseOverTcp()
{
    auto logger = std::make_shared<iocp::core::Logger>();
    iocp::server::HttpServerOptions options;
    options.listener.port = 0;
    auto server =
        iocp::server::HttpServer::Create(logger, options);

    SocketHandle client = Connect(server->LocalPort());
    SocketReader reader(client.Get());
    SendAll(
        client.Get(),
        "GET /health HTTP/1.1\r\n"
        "\r\n");

    const HttpWireResponse response = reader.ReadResponse();
    Check(
        response.head.find("HTTP/1.1 400 Bad Request\r\n") == 0 &&
            response.head.find("Connection: close\r\n") !=
                std::string::npos &&
            response.body == "invalid HTTP request\n",
        "invalid request did not receive a 400 close response");
    Check(
        reader.WaitForPeerClose(),
        "protocol error connection did not close after its response");

    client.Reset();
    Check(server->Stop(), "HTTP server did not stop after protocol error");
}

template <typename Test>
bool RunTest(const char* name, Test test)
{
    try
    {
        test();
        std::cout << "[PASS] " << name << '\n';
        return true;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << name << ": "
                  << exception.what() << '\n';
        return false;
    }
}

} // namespace

int main()
{
    int failures = 0;
    failures += !RunTest(
        "basic HTTP service over TCP",
        TestBasicServiceOverTcp);
    failures += !RunTest(
        "HTTP protocol error over TCP",
        TestProtocolErrorResponseOverTcp);
    return failures == 0 ? 0 : 1;
}
