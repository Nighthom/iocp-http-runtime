#include "core/logging.h"
#include "http_server/http_server.h"
#include "platform/windows/socket_handle.h"
#include "protocol/http2/http2_frames.h"
#include "protocol/http2/http2_hpack.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

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

void SendAll(
    const SOCKET socket,
    const std::vector<std::byte>& bytes,
    const std::size_t fragment_size = static_cast<std::size_t>(-1))
{
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const std::size_t remaining = bytes.size() - offset;
        const std::size_t submitted =
            (std::min)(remaining, fragment_size);
        const int sent = ::send(
            socket,
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<int>(submitted),
            0);
        if (sent == SOCKET_ERROR)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "send(HTTP/2 integration test)");
        }
        Check(sent > 0, "HTTP/2 test send made no progress");
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

struct H2WireFrame final
{
    iocp::protocol::http2::FrameHeader header;
    std::vector<std::byte> payload;
};

class H2SocketReader final
{
public:
    explicit H2SocketReader(const SOCKET socket)
        : socket_(socket)
    {
    }

    H2WireFrame ReadFrame()
    {
        using iocp::protocol::http2::FrameCodec;
        while (pending_.size() < FrameCodec::kHeaderSize)
        {
            ReadMore();
        }

        iocp::protocol::http2::FrameHeader header;
        Check(
            FrameCodec::DecodeHeader(
                iocp::buffer::BufferSequence(
                    iocp::buffer::ByteView(
                        pending_.data(), pending_.size())),
                header),
            "HTTP/2 response frame header did not decode");

        const std::size_t frame_size =
            FrameCodec::kHeaderSize + header.length;
        while (pending_.size() < frame_size)
        {
            ReadMore();
        }

        H2WireFrame frame;
        frame.header = header;
        frame.payload.insert(
            frame.payload.end(),
            pending_.begin() +
                static_cast<std::ptrdiff_t>(FrameCodec::kHeaderSize),
            pending_.begin() +
                static_cast<std::ptrdiff_t>(frame_size));
        pending_.erase(
            pending_.begin(),
            pending_.begin() +
                static_cast<std::ptrdiff_t>(frame_size));
        return frame;
    }

private:
    void ReadMore()
    {
        std::byte chunk[4096];
        const int received = ::recv(
            socket_,
            reinterpret_cast<char*>(chunk),
            sizeof(chunk),
            0);
        if (received == SOCKET_ERROR)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "recv(HTTP/2 integration test)");
        }
        Check(
            received > 0,
            "HTTP/2 peer closed before response completed");
        pending_.insert(
            pending_.end(),
            chunk,
            chunk + received);
    }

    SOCKET socket_;
    std::vector<std::byte> pending_;
};

std::vector<std::byte> MakeH2Headers(
    iocp::protocol::http2::HpackCodec& encoder,
    const std::uint32_t stream_id,
    const std::string& method,
    const std::string& path,
    const bool end_stream)
{
    using namespace iocp::protocol::http2;
    auto block = encoder.Encode({
        {":method", method},
        {":scheme", "http"},
        {":authority", "localhost"},
        {":path", path},
    });
    FrameHeader header;
    header.type = FrameType::Headers;
    header.stream_id = stream_id;
    header.flags =
        static_cast<std::uint8_t>(FrameFlags::EndHeaders);
    if (end_stream)
    {
        header.flags |=
            static_cast<std::uint8_t>(FrameFlags::EndStream);
    }
    header.length = static_cast<std::uint32_t>(block.size());
    auto frame = FrameCodec::EncodeHeader(header);
    frame.insert(frame.end(), block.begin(), block.end());
    return frame;
}

std::vector<std::byte> MakeH2Data(
    const std::uint32_t stream_id,
    const std::string_view body)
{
    using namespace iocp::protocol::http2;
    FrameHeader header;
    header.type = FrameType::Data;
    header.stream_id = stream_id;
    header.flags =
        static_cast<std::uint8_t>(FrameFlags::EndStream);
    header.length = static_cast<std::uint32_t>(body.size());
    auto frame = FrameCodec::EncodeHeader(header);
    const auto* first =
        reinterpret_cast<const std::byte*>(body.data());
    frame.insert(frame.end(), first, first + body.size());
    return frame;
}

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

void TestHttp2ServiceOverTcp()
{
    using namespace iocp::protocol::http2;

    auto logger = std::make_shared<iocp::core::Logger>();
    iocp::server::HttpServerOptions options;
    options.listener.port = 0;
    options.io_worker_count = 2;
    options.application_worker_count = 2;
    options.enable_http2 = true;
    auto server =
        iocp::server::HttpServer::Create(logger, options);

    SocketHandle client = Connect(server->LocalPort());
    H2SocketReader reader(client.Get());

    const std::string preface(FrameCodec::kPreface);
    std::vector<std::byte> preface_bytes(
        reinterpret_cast<const std::byte*>(preface.data()),
        reinterpret_cast<const std::byte*>(
            preface.data() + preface.size()));
    SendAll(client.Get(), preface_bytes, 1);
    SendAll(client.Get(), FrameCodec::EncodeSettings({}));

    HpackCodec request_encoder;
    const auto stream1 =
        MakeH2Headers(request_encoder, 1, "GET", "/health", true);
    const auto stream3 =
        MakeH2Headers(request_encoder, 3, "POST", "/echo", false);
    const auto stream3_data = MakeH2Data(3, "hello");
    SendAll(client.Get(), stream1, 3);
    SendAll(client.Get(), stream3, 5);
    SendAll(client.Get(), stream3_data, 2);

    HpackCodec response_decoder;
    std::unordered_map<std::uint32_t, std::string> bodies;
    std::unordered_map<std::uint32_t, std::string> statuses;
    std::unordered_map<std::uint32_t, bool> ended;

    for (std::size_t count = 0;
         count < 32 && (!ended[1] || !ended[3]);
         ++count)
    {
        H2WireFrame frame = reader.ReadFrame();
        if (frame.header.type == FrameType::Headers)
        {
            const auto headers = response_decoder.Decode(
                frame.payload.data(), frame.payload.size());
            for (const auto& header : headers)
            {
                if (header.name == ":status")
                {
                    statuses[frame.header.stream_id] =
                        header.value;
                }
            }
        }
        else if (frame.header.type == FrameType::Data)
        {
            bodies[frame.header.stream_id].append(
                reinterpret_cast<const char*>(
                    frame.payload.data()),
                frame.payload.size());
        }

        if ((frame.header.flags &
             static_cast<std::uint8_t>(
                 FrameFlags::EndStream)) != 0)
        {
            ended[frame.header.stream_id] = true;
        }
    }

    Check(
        ended[1] && ended[3],
        "HTTP/2 responses did not finish both streams");
    Check(
        statuses[1] == "200" &&
            bodies[1] == "{\"status\":\"ok\"}\n",
        "HTTP/2 GET /health response was incorrect");
    Check(
        statuses[3] == "200" && bodies[3] == "hello",
        "HTTP/2 POST /echo response was incorrect");

    client.Reset();
    Check(server->Stop(), "HTTP/2 server did not stop cleanly");
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
    failures += !RunTest(
        "HTTP/2 service over TCP",
        TestHttp2ServiceOverTcp);
    return failures == 0 ? 0 : 1;
}
