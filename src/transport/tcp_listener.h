#pragma once

/// @file tcp_listener.h
/// @brief AcceptEx 기반의 TCP listening socket 관리자다.
///
/// 항상 하나의 pending AcceptEx를 유지하며, completion에서 accept-before-complete
/// 순서로 다음 AcceptEx를 등록해 listen backlog 소진을 방지한다.
/// Stop은 listen socket을 먼저 닫아 pending AcceptEx의 IOCP cancellation
/// completion을 유도하고, 모든 outstanding accept가 drain된 후 Stopped로
/// 전이한다. pending AcceptOperation이 listener의 shared ownership을
/// 보존하므로, completion이 모두 처리될 때까지 객체가 살아있다.

#include "core/logging.h"
#include "platform/windows/socket_handle.h"
#include "runtime/io_context.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <MSWSock.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace iocp::transport
{

/// @brief M2 listener가 bind할 IPv4 endpoint와 backlog 설정이다.
struct ListenerOptions final
{
    std::string address{"127.0.0.1"};
    std::uint16_t port{0};
    int backlog{SOMAXCONN};
};

enum class ListenerState
{
    Running,
    Stopping,
    Stopped,
};

struct ListenerSnapshot final
{
    ListenerState state{ListenerState::Stopped};
    std::size_t outstanding_accepts{};
    std::uint64_t accepted_connections{};
    std::uint64_t accept_errors{};
    std::uint16_t local_port{};
};

/// @brief 하나의 outstanding `AcceptEx`를 유지하는 TCP listener다.
///
/// `Create`가 listen socket 준비와 첫 accept 등록을 완료한다. pending
/// `AcceptOperation`이 listener의 shared ownership을 보존한다.
class TcpListener final :
    public std::enable_shared_from_this<TcpListener>
{
public:
    using AcceptHandler =
        std::function<void(platform::windows::SocketHandle accepted_socket)>;

    /// @brief listener를 만들고 첫 `AcceptEx`를 등록한다.
    ///
    /// @throws std::invalid_argument option 또는 handler가 유효하지 않은 경우.
    /// @throws std::system_error socket 준비나 첫 accept 등록에 실패한 경우.
    static std::shared_ptr<TcpListener> Create(
        runtime::IoContext& io_context,
        std::shared_ptr<core::Logger> logger,
        ListenerOptions options,
        AcceptHandler accept_handler);

    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// @brief 신규 accept를 중단하고 listen socket을 닫는다.
    ///
    /// pending accept operation은 cancellation completion에서 정리한다.
    void Stop() noexcept;

    /// @brief listener가 `Stopped`가 될 때까지 기다린다.
    bool WaitStopped(std::chrono::milliseconds timeout);

    /// @brief 현재 listener 상태의 snapshot을 반환한다.
    ListenerSnapshot Snapshot() const;

private:
    class AcceptOperation;

    TcpListener(
        runtime::IoContext& io_context,
        std::shared_ptr<core::Logger> logger,
        ListenerOptions options,
        AcceptHandler accept_handler);

    void Start();
    int PostAcceptLocked();
    void OnAcceptComplete(
        platform::windows::SocketHandle accepted_socket,
        std::uint32_t transferred_bytes,
        std::error_code error) noexcept;
    void OnAcceptCompleteImpl(
        platform::windows::SocketHandle accepted_socket,
        std::uint32_t transferred_bytes,
        std::error_code error);
    void FinishAcceptCompletion() noexcept;
    bool MoveToStoppedIfDrainedLocked() noexcept;

    runtime::IoContext& io_context_;
    std::shared_ptr<core::Logger> logger_;
    ListenerOptions options_;
    AcceptHandler accept_handler_;

    mutable std::mutex mutex_;
    std::condition_variable stopped_condition_;
    platform::windows::SocketHandle listen_socket_;
    LPFN_ACCEPTEX accept_ex_{};
    ListenerState state_{ListenerState::Running};
    std::size_t outstanding_accepts_{0};
    std::uint64_t accepted_connections_{0};
    std::uint64_t accept_errors_{0};
    std::uint16_t local_port_{0};
};

} // namespace iocp::transport
