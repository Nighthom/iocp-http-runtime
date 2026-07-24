/// @file winsock_runtime.h
/// @brief WSAStartup/WSACleanup 수명 관리 — RAII로 Winsock DLL을
/// 초기화하고 종료한다. application composition root에서 connection보다
/// 먼저 생성하고 가장 마지막에 파괴해야 한다.

#pragma once

#include <memory>

namespace iocp::core
{
class Logger;
}

namespace iocp::platform::windows
{

/// @brief process의 Winsock 시작과 종료를 RAII로 관리한다.
///
/// application composition root에서 connection보다 먼저 생성하고 가장
/// 나중에 파괴해야 한다.
class WinsockRuntime final
{
public:
    explicit WinsockRuntime(std::shared_ptr<core::Logger> logger);
    ~WinsockRuntime();

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
    WinsockRuntime(WinsockRuntime&&) = delete;
    WinsockRuntime& operator=(WinsockRuntime&&) = delete;

private:
    std::shared_ptr<core::Logger> logger_;
    bool started_{false};
};

} // namespace iocp::platform::windows
