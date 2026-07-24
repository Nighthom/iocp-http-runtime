#include "echo_server/configuration.h"
#include "core/logging.h"
#include "platform/windows/socket_handle.h"
#include "platform/windows/winsock_runtime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <WS2tcpip.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{

using iocp::platform::windows::SocketHandle;
using iocp::platform::windows::WinsockRuntime;

void ConfigureTimeouts(
    const SOCKET socket,
    const iocp::application::EchoClientOptions& options)
{
    const DWORD receive_timeout =
        static_cast<DWORD>(options.receive_timeout.count());
    if (::setsockopt(
            socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&receive_timeout),
            sizeof(receive_timeout)) == SOCKET_ERROR)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "setsockopt(SO_RCVTIMEO)");
    }
    const DWORD send_timeout =
        static_cast<DWORD>(options.send_timeout.count());
    if (::setsockopt(
            socket,
            SOL_SOCKET,
            SO_SNDTIMEO,
            reinterpret_cast<const char*>(&send_timeout),
            sizeof(send_timeout)) == SOCKET_ERROR)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "setsockopt(SO_SNDTIMEO)");
    }
}

SocketHandle Connect(
    const iocp::application::EchoClientOptions& options)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(options.port);
    const int lookup_error = ::getaddrinfo(
        options.host.c_str(),
        service.c_str(),
        &hints,
        &addresses);
    if (lookup_error != 0)
    {
        throw std::system_error(
            lookup_error,
            std::system_category(),
            "getaddrinfo");
    }

    struct AddressList final
    {
        addrinfo* value;

        ~AddressList()
        {
            ::freeaddrinfo(value);
        }
    } address_list{addresses};

    int last_error = WSAECONNREFUSED;
    for (const addrinfo* address = addresses;
         address != nullptr;
         address = address->ai_next)
    {
        SocketHandle candidate(::socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol));
        if (!candidate)
        {
            last_error = ::WSAGetLastError();
            continue;
        }

        ConfigureTimeouts(candidate.Get(), options);
        if (::connect(
                candidate.Get(),
                address->ai_addr,
                static_cast<int>(address->ai_addrlen)) == 0)
        {
            return candidate;
        }
        last_error = ::WSAGetLastError();
    }

    throw std::system_error(
        last_error,
        std::system_category(),
        "connect");
}

void SendAll(const SOCKET socket, const std::string_view payload)
{
    std::size_t offset = 0;
    while (offset < payload.size())
    {
        const std::size_t remaining = payload.size() - offset;
        const int chunk_size = static_cast<int>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int sent = ::send(
            socket,
            payload.data() + offset,
            chunk_size,
            0);
        if (sent == SOCKET_ERROR)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "send");
        }
        if (sent == 0)
        {
            throw std::runtime_error(
                "send가 progress 없이 완료됐습니다");
        }
        offset += static_cast<std::size_t>(sent);
    }
}

std::string ReceiveExact(
    const SOCKET socket,
    const std::size_t expected_bytes)
{
    std::string echoed(expected_bytes, '\0');
    std::size_t offset = 0;
    while (offset < echoed.size())
    {
        const std::size_t remaining = echoed.size() - offset;
        const int chunk_size = static_cast<int>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int received = ::recv(
            socket,
            echoed.data() + offset,
            chunk_size,
            0);
        if (received == SOCKET_ERROR)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "recv");
        }
        if (received == 0)
        {
            throw std::runtime_error(
                "echo를 모두 받기 전에 server가 연결을 종료했습니다");
        }
        offset += static_cast<std::size_t>(received);
    }
    return echoed;
}

std::string Exchange(
    const SOCKET socket,
    const std::string_view payload)
{
    SendAll(socket, payload);
    return ReceiveExact(socket, payload.size());
}

void RunInteractive(const SOCKET socket)
{
    std::cout << "메시지를 입력하세요. /quit 또는 Ctrl+Z로 종료합니다.\n";

    std::string line;
    for (;;)
    {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line) || line == "/quit")
        {
            break;
        }

        const std::string echoed = Exchange(socket, line);
        std::cout << "echo: " << echoed << '\n';
    }
}

} // namespace

int main(int argc, char* argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    try
    {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index)
        {
            arguments.emplace_back(argv[index]);
        }

        const auto options =
            iocp::application::LoadEchoClientOptions(arguments);
        if (options.show_help)
        {
            std::cout << iocp::application::EchoClientUsage();
            return 0;
        }

        auto logger = std::make_shared<iocp::core::Logger>();
        WinsockRuntime winsock(logger);
        SocketHandle socket = Connect(options);

        std::cout << options.host << ':' << options.port
                  << "에 연결했습니다.\n";

        if (options.one_shot)
        {
            std::cout << "echo: "
                      << Exchange(socket.Get(), options.message)
                      << '\n';
        }
        else
        {
            RunInteractive(socket.Get());
        }

        ::shutdown(socket.Get(), SD_BOTH);
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Echo client 실행 실패: "
                  << exception.what() << '\n';
        return 1;
    }
}
