#include "transport/tcp_connector.h"

#include "runtime/completion_operation.h"

#include <MSWSock.h>
#include <WS2tcpip.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace iocp::transport
{

namespace
{

std::error_code WinsockErrorCode(const int error) noexcept
{
    return std::error_code(error, std::system_category());
}

ConnectStartResult StartFailure(const int error) noexcept
{
    return ConnectStartResult{
        ConnectStartStatus::StartFailed,
        WinsockErrorCode(error),
    };
}

} // namespace

class TcpConnector::ConnectOperation final :
    public runtime::CompletionOperation
{
public:
    ConnectOperation(
        std::shared_ptr<TcpConnector> connector,
        const std::uint64_t operation_id,
        std::shared_ptr<platform::windows::SocketHandle> socket,
        ConnectHandler handler)
        : connector_(std::move(connector)),
          operation_id_(operation_id),
          socket_(std::move(socket)),
          handler_(std::move(handler))
    {
    }

    void Complete(
        std::uint32_t,
        std::error_code error,
        std::uintptr_t) noexcept override
    {
        connector_->OnConnectComplete(
            operation_id_,
            std::move(socket_),
            std::move(handler_),
            error);
    }

private:
    std::shared_ptr<TcpConnector> connector_;
    std::uint64_t operation_id_{};
    std::shared_ptr<platform::windows::SocketHandle> socket_;
    ConnectHandler handler_;
};

std::shared_ptr<TcpConnector> TcpConnector::Create(
    runtime::IoContext& io_context,
    std::shared_ptr<core::Logger> logger)
{
    return std::shared_ptr<TcpConnector>(
        new TcpConnector(io_context, std::move(logger)));
}

TcpConnector::TcpConnector(
    runtime::IoContext& io_context,
    std::shared_ptr<core::Logger> logger)
    : io_context_(io_context),
      logger_(std::move(logger))
{
    if (!logger_)
    {
        throw std::invalid_argument("TcpConnector에는 Logger가 필요합니다");
    }
}

TcpConnector::~TcpConnector()
{
    Stop();
}

ConnectStartResult TcpConnector::Connect(
    TcpConnectOptions options,
    ConnectHandler handler)
{
    if (!handler)
    {
        throw std::invalid_argument("TcpConnector에는 connect handler가 필요합니다");
    }
    if (options.address.empty())
    {
        throw std::invalid_argument("connect IPv4 address는 비어 있을 수 없습니다");
    }
    if (options.port == 0)
    {
        throw std::invalid_argument("connect port는 1 이상이어야 합니다");
    }
    {
        std::lock_guard lock(mutex_);
        if (state_ != TcpConnectorState::Running)
        {
            return ConnectStartResult{ConnectStartStatus::Stopped, {}};
        }
    }

    sockaddr_in remote_endpoint{};
    remote_endpoint.sin_family = AF_INET;
    remote_endpoint.sin_port = ::htons(options.port);
    if (::InetPtonA(
            AF_INET,
            options.address.c_str(),
            &remote_endpoint.sin_addr) != 1)
    {
        throw std::invalid_argument("connect IPv4 address가 유효하지 않습니다");
    }

    auto socket =
        std::make_shared<platform::windows::SocketHandle>(::WSASocketW(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP,
            nullptr,
            0,
            WSA_FLAG_OVERLAPPED));
    if (!*socket)
    {
        return StartFailure(::WSAGetLastError());
    }

    sockaddr_in local_endpoint{};
    local_endpoint.sin_family = AF_INET;
    local_endpoint.sin_addr.s_addr = ::htonl(INADDR_ANY);
    local_endpoint.sin_port = 0;
    if (::bind(
            socket->Get(),
            reinterpret_cast<const sockaddr*>(&local_endpoint),
            sizeof(local_endpoint)) == SOCKET_ERROR)
    {
        return StartFailure(::WSAGetLastError());
    }

    LPFN_CONNECTEX connect_ex = nullptr;
    GUID connect_ex_guid = WSAID_CONNECTEX;
    DWORD bytes_returned = 0;
    if (::WSAIoctl(
            socket->Get(),
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &connect_ex_guid,
            sizeof(connect_ex_guid),
            &connect_ex,
            sizeof(connect_ex),
            &bytes_returned,
            nullptr,
            nullptr) == SOCKET_ERROR)
    {
        return StartFailure(::WSAGetLastError());
    }

    io_context_.Associate(
        reinterpret_cast<HANDLE>(socket->Get()),
        0);

    std::unique_ptr<ConnectOperation> operation;
    std::uint64_t operation_id = 0;
    int immediate_error = 0;
    {
        std::lock_guard lock(mutex_);
        if (state_ != TcpConnectorState::Running)
        {
            return ConnectStartResult{ConnectStartStatus::Stopped, {}};
        }

        operation_id = next_operation_id_++;
        operation = std::make_unique<ConnectOperation>(
            shared_from_this(),
            operation_id,
            socket,
            std::move(handler));
        pending_sockets_.emplace(operation_id, socket);
        ++outstanding_connects_;

        DWORD bytes_sent = 0;
        const BOOL started = connect_ex(
            socket->Get(),
            reinterpret_cast<const sockaddr*>(&remote_endpoint),
            sizeof(remote_endpoint),
            nullptr,
            0,
            &bytes_sent,
            operation->NativeHandle());
        if (!started)
        {
            immediate_error = ::WSAGetLastError();
            if (immediate_error != WSA_IO_PENDING)
            {
                --outstanding_connects_;
                pending_sockets_.erase(operation_id);
                ++failed_connects_;
            }
        }
    }

    if (immediate_error != 0 && immediate_error != WSA_IO_PENDING)
    {
        const std::string error_text = std::to_string(immediate_error);
        logger_->Log(
            core::LogLevel::Warning,
            "connector.connect_start_failed",
            "ConnectEx 등록에 실패했습니다.",
            {
                {"address", options.address},
                {"port", std::to_string(options.port)},
                {"win32_error", error_text},
            });
        return StartFailure(immediate_error);
    }

    operation.release();
    return ConnectStartResult{ConnectStartStatus::Accepted, {}};
}

void TcpConnector::OnConnectComplete(
    const std::uint64_t operation_id,
    std::shared_ptr<platform::windows::SocketHandle> socket,
    ConnectHandler handler,
    std::error_code error) noexcept
{
    platform::windows::SocketHandle connected_socket;
    bool became_stopped = false;

    try
    {
        {
            std::lock_guard lock(mutex_);
            pending_sockets_.erase(operation_id);

            if (state_ != TcpConnectorState::Running)
            {
                ++cancelled_connects_;
                error = std::make_error_code(
                    std::errc::operation_canceled);
            }
            else if (!error)
            {
                if (::setsockopt(
                        socket->Get(),
                        SOL_SOCKET,
                        SO_UPDATE_CONNECT_CONTEXT,
                        nullptr,
                        0) == SOCKET_ERROR)
                {
                    error = WinsockErrorCode(::WSAGetLastError());
                    ++failed_connects_;
                }
                else
                {
                    connected_socket = std::move(*socket);
                    ++successful_connects_;
                }
            }
            else
            {
                ++failed_connects_;
            }
        }
    }
    catch (...)
    {
        error = std::make_error_code(std::errc::io_error);
        logger_->Log(
            core::LogLevel::Critical,
            "connector.completion_handler_failed",
            "connect completion 처리 중 알 수 없는 예외가 발생했습니다.");
    }

    if (error)
    {
        try
        {
            const std::string error_text = std::to_string(error.value());
            logger_->Log(
                core::LogLevel::Warning,
                "connector.connect_failed",
                "outbound TCP 연결에 실패했습니다.",
                {{"error", error_text}});
        }
        catch (...)
        {
            logger_->Log(
                core::LogLevel::Warning,
                "connector.connect_failed",
                "outbound TCP 연결에 실패했습니다.");
        }
    }

    try
    {
        handler(std::move(connected_socket), error);
    }
    catch (const std::exception& exception)
    {
        logger_->Log(
            core::LogLevel::Error,
            "connector.connect_handler_failed",
            "connect handler가 예외를 던졌습니다.",
            {{"exception", exception.what()}});
    }
    catch (...)
    {
        logger_->Log(
            core::LogLevel::Error,
            "connector.connect_handler_failed",
            "connect handler에서 알 수 없는 예외가 발생했습니다.");
    }

    try
    {
        std::lock_guard lock(mutex_);
        if (outstanding_connects_ > 0)
        {
            --outstanding_connects_;
        }
        became_stopped = MoveToStoppedIfDrainedLocked();
    }
    catch (...)
    {
        logger_->Log(
            core::LogLevel::Critical,
            "connector.completion_drain_failed",
            "connect handler 이후 outstanding count를 정리하지 못했습니다.");
    }

    if (became_stopped)
    {
        stopped_condition_.notify_all();
    }
}

void TcpConnector::Stop() noexcept
{
    bool became_stopped = false;
    try
    {
        std::lock_guard lock(mutex_);
        if (state_ == TcpConnectorState::Stopped ||
            state_ == TcpConnectorState::Stopping)
        {
            return;
        }

        state_ = TcpConnectorState::Stopping;
        for (auto& [operation_id, socket] : pending_sockets_)
        {
            static_cast<void>(operation_id);
            socket->Reset();
        }
        became_stopped = MoveToStoppedIfDrainedLocked();
    }
    catch (...)
    {
        return;
    }

    logger_->Log(
        core::LogLevel::Info,
        "connector.stop_requested",
        "TCP connector 중단을 요청했습니다.");

    if (became_stopped)
    {
        stopped_condition_.notify_all();
    }
}

bool TcpConnector::WaitStopped(
    const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return stopped_condition_.wait_for(lock, timeout, [this] {
        return state_ == TcpConnectorState::Stopped;
    });
}

TcpConnectorSnapshot TcpConnector::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return TcpConnectorSnapshot{
        state_,
        outstanding_connects_,
        successful_connects_,
        failed_connects_,
        cancelled_connects_,
    };
}

bool TcpConnector::MoveToStoppedIfDrainedLocked() noexcept
{
    if (state_ == TcpConnectorState::Stopping &&
        outstanding_connects_ == 0)
    {
        state_ = TcpConnectorState::Stopped;
        return true;
    }
    return false;
}

} // namespace iocp::transport
