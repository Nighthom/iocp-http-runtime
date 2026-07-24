#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>

#include <utility>

namespace iocp::platform::windows
{

/// @brief 하나의 Winsock `SOCKET` ownership을 관리한다.
class SocketHandle final
{
public:
    SocketHandle() noexcept = default;

    explicit SocketHandle(const SOCKET socket) noexcept
        : socket_(socket)
    {
    }

    ~SocketHandle()
    {
        Reset();
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& other) noexcept
        : socket_(other.Release())
    {
    }

    SocketHandle& operator=(SocketHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset(other.Release());
        }
        return *this;
    }

    /// @brief 소유 중인 native socket을 반환한다.
    SOCKET Get() const noexcept
    {
        return socket_;
    }

    /// @brief 유효한 socket을 소유하는지 반환한다.
    explicit operator bool() const noexcept
    {
        return socket_ != INVALID_SOCKET;
    }

    /// @brief socket을 닫지 않고 ownership을 호출자에게 넘긴다.
    SOCKET Release() noexcept
    {
        return std::exchange(socket_, INVALID_SOCKET);
    }

    /// @brief 기존 socket을 닫고 새 socket을 소유한다.
    void Reset(const SOCKET socket = INVALID_SOCKET) noexcept
    {
        if (socket_ != INVALID_SOCKET)
        {
            ::closesocket(socket_);
        }
        socket_ = socket;
    }

private:
    SOCKET socket_{INVALID_SOCKET};
};

} // namespace iocp::platform::windows
