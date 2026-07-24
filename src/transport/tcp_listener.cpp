#include "transport/tcp_listener.h"

#include "runtime/completion_operation.h"

#include <WS2tcpip.h>

#include <array>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace iocp::transport
{

namespace
{

constexpr DWORD kAddressLength =
    static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
constexpr std::size_t kAcceptAddressBufferSize = kAddressLength * 2;

std::system_error WinsockError(const int error, const char* operation)
{
    return std::system_error(error, std::system_category(), operation);
}

} // namespace

class TcpListener::AcceptOperation final :
    public runtime::CompletionOperation
{
public:
    AcceptOperation(
        std::shared_ptr<TcpListener> listener,
        platform::windows::SocketHandle accepted_socket)
        : listener_(std::move(listener)),
          accepted_socket_(std::move(accepted_socket))
    {
    }

    void Complete(
        const std::uint32_t transferred_bytes,
        std::error_code error,
        std::uintptr_t) noexcept override
    {
        listener_->OnAcceptComplete(
            std::move(accepted_socket_),
            transferred_bytes,
            error);
    }

    SOCKET Socket() const noexcept
    {
        return accepted_socket_.Get();
    }

    void* AddressBuffer() noexcept
    {
        return address_buffer_.data();
    }

private:
    std::shared_ptr<TcpListener> listener_;
    platform::windows::SocketHandle accepted_socket_;
    std::array<std::byte, kAcceptAddressBufferSize> address_buffer_{};
};

std::shared_ptr<TcpListener> TcpListener::Create(
    runtime::IoContext& io_context,
    std::shared_ptr<core::Logger> logger,
    ListenerOptions options,
    AcceptHandler accept_handler)
{
    auto listener = std::shared_ptr<TcpListener>(new TcpListener(
        io_context,
        std::move(logger),
        std::move(options),
        std::move(accept_handler)));
    listener->Start();
    return listener;
}

TcpListener::TcpListener(
    runtime::IoContext& io_context,
    std::shared_ptr<core::Logger> logger,
    ListenerOptions options,
    AcceptHandler accept_handler)
    : io_context_(io_context),
      logger_(std::move(logger)),
      options_(std::move(options)),
      accept_handler_(std::move(accept_handler))
{
    if (!logger_)
    {
        throw std::invalid_argument("TcpListener에는 Logger가 필요합니다");
    }
    if (!accept_handler_)
    {
        throw std::invalid_argument("TcpListener에는 accept handler가 필요합니다");
    }
    if (options_.backlog <= 0)
    {
        throw std::invalid_argument("listener backlog는 1 이상이어야 합니다");
    }

    listen_socket_.Reset(::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED));
    if (!listen_socket_)
    {
        throw WinsockError(::WSAGetLastError(), "WSASocketW(listener)");
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(options_.port);
    const int address_result = ::InetPtonA(
        AF_INET,
        options_.address.c_str(),
        &endpoint.sin_addr);
    if (address_result != 1)
    {
        throw std::invalid_argument("listener IPv4 address가 유효하지 않습니다");
    }

    if (::bind(
            listen_socket_.Get(),
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)) == SOCKET_ERROR)
    {
        throw WinsockError(::WSAGetLastError(), "bind");
    }

    if (::listen(listen_socket_.Get(), options_.backlog) == SOCKET_ERROR)
    {
        throw WinsockError(::WSAGetLastError(), "listen");
    }

    sockaddr_in local_endpoint{};
    int local_length = sizeof(local_endpoint);
    if (::getsockname(
            listen_socket_.Get(),
            reinterpret_cast<sockaddr*>(&local_endpoint),
            &local_length) == SOCKET_ERROR)
    {
        throw WinsockError(::WSAGetLastError(), "getsockname");
    }
    local_port_ = ::ntohs(local_endpoint.sin_port);

    GUID accept_ex_guid = WSAID_ACCEPTEX;
    DWORD bytes_returned = 0;
    if (::WSAIoctl(
            listen_socket_.Get(),
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &accept_ex_guid,
            sizeof(accept_ex_guid),
            &accept_ex_,
            sizeof(accept_ex_),
            &bytes_returned,
            nullptr,
            nullptr) == SOCKET_ERROR)
    {
        throw WinsockError(::WSAGetLastError(), "WSAIoctl(AcceptEx)");
    }

    io_context_.Associate(
        reinterpret_cast<HANDLE>(listen_socket_.Get()),
        0);
}

TcpListener::~TcpListener()
{
    Stop();
}

void TcpListener::Start()
{
    int post_error = 0;
    {
        std::lock_guard lock(mutex_);
        post_error = PostAcceptLocked();
        if (post_error != 0)
        {
            state_ = ListenerState::Stopping;
            listen_socket_.Reset();
            MoveToStoppedIfDrainedLocked();
        }
    }

    if (post_error != 0)
    {
        stopped_condition_.notify_all();
        throw WinsockError(post_error, "AcceptEx");
    }

    const std::string port_text = std::to_string(local_port_);
    logger_->Log(
        core::LogLevel::Info,
        "listener.started",
        "TCP listener를 시작했습니다.",
        {
            {"address", options_.address},
            {"port", port_text},
        });
}

int TcpListener::PostAcceptLocked()
{
    if (state_ != ListenerState::Running)
    {
        return WSAESHUTDOWN;
    }

    platform::windows::SocketHandle accepted_socket(::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED));
    if (!accepted_socket)
    {
        return ::WSAGetLastError();
    }

    auto operation = std::make_unique<AcceptOperation>(
        shared_from_this(),
        std::move(accepted_socket));

    ++outstanding_accepts_;
    DWORD bytes_received = 0;
    const BOOL accepted = accept_ex_(
        listen_socket_.Get(),
        operation->Socket(),
        operation->AddressBuffer(),
        0,
        kAddressLength,
        kAddressLength,
        &bytes_received,
        operation->NativeHandle());

    if (!accepted)
    {
        const int error = ::WSAGetLastError();
        if (error != WSA_IO_PENDING)
        {
            --outstanding_accepts_;
            return error;
        }
    }

    // immediate success도 IOCP completion packet을 받으므로 여기서 ownership을
    // worker로 넘긴다.
    operation.release();
    return 0;
}

void TcpListener::OnAcceptComplete(
    platform::windows::SocketHandle accepted_socket,
    const std::uint32_t transferred_bytes,
    const std::error_code error) noexcept
{
    try
    {
        OnAcceptCompleteImpl(
            std::move(accepted_socket),
            transferred_bytes,
            error);
    }
    catch (const std::exception& exception)
    {
        logger_->Log(
            core::LogLevel::Error,
            "listener.completion_handler_failed",
            "accept completion 처리 중 예외가 발생해 listener를 중단합니다.",
            {{"exception", exception.what()}});
        Stop();
    }
    catch (...)
    {
        logger_->Log(
            core::LogLevel::Error,
            "listener.completion_handler_failed",
            "accept completion 처리 중 알 수 없는 예외가 발생했습니다.");
        Stop();
    }

    FinishAcceptCompletion();
}

void TcpListener::OnAcceptCompleteImpl(
    platform::windows::SocketHandle accepted_socket,
    const std::uint32_t,
    const std::error_code error)
{
    SOCKET listen_socket = INVALID_SOCKET;
    int next_accept_error = 0;
    bool completion_was_running = false;

    {
        std::lock_guard lock(mutex_);
        completion_was_running = state_ == ListenerState::Running;
        if (completion_was_running)
        {
            listen_socket = listen_socket_.Get();
            next_accept_error = PostAcceptLocked();
            if (next_accept_error != 0)
            {
                ++accept_errors_;
                state_ = ListenerState::Stopping;
                listen_socket_.Reset();
            }
        }
        else if (error)
        {
            // stop에 의해 취소된 completion은 accept error 통계에서 제외한다.
        }
    }

    if (next_accept_error != 0)
    {
        const std::string error_text = std::to_string(next_accept_error);
        logger_->Log(
            core::LogLevel::Error,
            "listener.accept_post_failed",
            "다음 AcceptEx 등록에 실패해 listener를 중단합니다.",
            {{"win32_error", error_text}});
    }

    if (error)
    {
        if (completion_was_running)
        {
            {
                std::lock_guard lock(mutex_);
                ++accept_errors_;
            }
            const std::string error_text = std::to_string(error.value());
            logger_->Log(
                core::LogLevel::Warning,
                "listener.accept_failed",
                "AcceptEx completion이 실패했습니다.",
                {{"win32_error", error_text}});
        }
    }
    else if (completion_was_running && listen_socket != INVALID_SOCKET)
    {
        bool prepared = true;
        if (::setsockopt(
                accepted_socket.Get(),
                SOL_SOCKET,
                SO_UPDATE_ACCEPT_CONTEXT,
                reinterpret_cast<const char*>(&listen_socket),
                sizeof(listen_socket)) == SOCKET_ERROR)
        {
            prepared = false;
            const std::string error_text =
                std::to_string(::WSAGetLastError());
            logger_->Log(
                core::LogLevel::Warning,
                "listener.accept_context_failed",
                "accepted socket의 accept context 갱신에 실패했습니다.",
                {{"win32_error", error_text}});
        }

        if (prepared)
        {
            try
            {
                io_context_.Associate(
                    reinterpret_cast<HANDLE>(accepted_socket.Get()),
                    0);
            }
            catch (const std::exception& exception)
            {
                prepared = false;
                logger_->Log(
                    core::LogLevel::Warning,
                    "listener.accept_associate_failed",
                    "accepted socket을 IO completion port에 연결하지 못했습니다.",
                    {{"exception", exception.what()}});
            }
        }

        if (prepared)
        {
            bool deliver = false;
            {
                std::lock_guard lock(mutex_);
                if (state_ == ListenerState::Running)
                {
                    ++accepted_connections_;
                    deliver = true;
                }
            }

            if (deliver)
            {
                try
                {
                    accept_handler_(std::move(accepted_socket));
                }
                catch (const std::exception& exception)
                {
                    logger_->Log(
                        core::LogLevel::Error,
                        "listener.accept_handler_failed",
                        "accept handler가 예외를 던졌습니다.",
                        {{"exception", exception.what()}});
                }
                catch (...)
                {
                    logger_->Log(
                        core::LogLevel::Error,
                        "listener.accept_handler_failed",
                        "accept handler에서 알 수 없는 예외가 발생했습니다.");
                }
            }
        }
    }

}

void TcpListener::FinishAcceptCompletion() noexcept
{
    bool became_stopped = false;
    try
    {
        std::lock_guard lock(mutex_);
        if (outstanding_accepts_ > 0)
        {
            --outstanding_accepts_;
        }
        became_stopped = MoveToStoppedIfDrainedLocked();
    }
    catch (...)
    {
        return;
    }

    if (became_stopped)
    {
        stopped_condition_.notify_all();
    }
}

void TcpListener::Stop() noexcept
{
    bool became_stopped = false;
    try
    {
        std::lock_guard lock(mutex_);
        if (state_ == ListenerState::Stopped ||
            state_ == ListenerState::Stopping)
        {
            return;
        }

        state_ = ListenerState::Stopping;
        listen_socket_.Reset();
        became_stopped = MoveToStoppedIfDrainedLocked();
    }
    catch (...)
    {
        return;
    }

    logger_->Log(
        core::LogLevel::Info,
        "listener.stop_requested",
        "TCP listener 중단을 요청했습니다.");

    if (became_stopped)
    {
        stopped_condition_.notify_all();
    }
}

bool TcpListener::WaitStopped(const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return stopped_condition_.wait_for(lock, timeout, [this] {
        return state_ == ListenerState::Stopped;
    });
}

ListenerSnapshot TcpListener::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return ListenerSnapshot{
        state_,
        outstanding_accepts_,
        accepted_connections_,
        accept_errors_,
        local_port_,
    };
}

bool TcpListener::MoveToStoppedIfDrainedLocked() noexcept
{
    if (state_ == ListenerState::Stopping && outstanding_accepts_ == 0)
    {
        state_ = ListenerState::Stopped;
        return true;
    }
    return false;
}

} // namespace iocp::transport
