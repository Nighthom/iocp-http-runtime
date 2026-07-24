#include "platform/windows/winsock_runtime.h"

#include "core/logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace iocp::platform::windows
{

WinsockRuntime::WinsockRuntime(std::shared_ptr<core::Logger> logger)
    : logger_(std::move(logger))
{
    if (!logger_)
    {
        throw std::invalid_argument("WinsockRuntime에는 Logger가 필요합니다");
    }

    WSADATA data{};
    const int error = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (error != 0)
    {
        const std::string error_text = std::to_string(error);
        logger_->Log(
            core::LogLevel::Critical,
            "winsock.start_failed",
            "Winsock 초기화에 실패했습니다.",
            {{"win32_error", error_text}});
        throw std::system_error(error, std::system_category(), "WSAStartup");
    }

    started_ = true;
    logger_->Log(
        core::LogLevel::Info,
        "winsock.started",
        "Winsock 2.2를 초기화했습니다.");
}

WinsockRuntime::~WinsockRuntime()
{
    if (!started_)
    {
        return;
    }

    if (::WSACleanup() == SOCKET_ERROR)
    {
        try
        {
            const std::string error_text =
                std::to_string(::WSAGetLastError());
            logger_->Log(
                core::LogLevel::Error,
                "winsock.cleanup_failed",
                "Winsock 종료 중 오류가 발생했습니다.",
                {{"win32_error", error_text}});
        }
        catch (...)
        {
            logger_->Log(
                core::LogLevel::Error,
                "winsock.cleanup_failed",
                "Winsock 종료 중 오류가 발생했습니다.");
        }
        return;
    }

    logger_->Log(
        core::LogLevel::Info,
        "winsock.stopped",
        "Winsock을 종료했습니다.");
}

} // namespace iocp::platform::windows
