#pragma once

#include "core/logging.h"
#include "platform/windows/socket_handle.h"
#include "runtime/io_context.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>

namespace iocp::transport
{

/// @brief 한 outbound TCP 연결의 원격 IPv4 endpoint다.
struct TcpConnectOptions final
{
    std::string address{"127.0.0.1"};
    std::uint16_t port{};
};

enum class TcpConnectorState
{
    Running,
    Stopping,
    Stopped,
};

enum class ConnectStartStatus
{
    Accepted,
    Stopped,
    StartFailed,
};

struct ConnectStartResult final
{
    ConnectStartStatus status{ConnectStartStatus::StartFailed};
    std::error_code error;
};

struct TcpConnectorSnapshot final
{
    TcpConnectorState state{TcpConnectorState::Stopped};
    std::size_t outstanding_connects{};
    std::uint64_t successful_connects{};
    std::uint64_t failed_connects{};
    std::uint64_t cancelled_connects{};
};

/// @brief `ConnectEx` 요청을 IOCP completion으로 전달하는 outbound connector다.
///
/// accepted request의 handler는 성공과 실패를 포함해 정확히 한 번 호출된다.
/// 성공 시 socket은 이미 같은 `IoContext`에 associate되고 connect context가
/// 갱신된 상태다. handler는 이 socket을 `TcpConnection`에 넘길 수 있다.
class TcpConnector final :
    public std::enable_shared_from_this<TcpConnector>
{
public:
    using ConnectHandler = std::function<void(
        platform::windows::SocketHandle socket,
        std::error_code error)>;

    static std::shared_ptr<TcpConnector> Create(
        runtime::IoContext& io_context,
        std::shared_ptr<core::Logger> logger);

    ~TcpConnector();

    TcpConnector(const TcpConnector&) = delete;
    TcpConnector& operator=(const TcpConnector&) = delete;

    /// @brief 하나의 overlapped `ConnectEx`를 등록한다.
    ///
    /// `Accepted`면 handler가 completion path에서 정확히 한 번 호출된다.
    /// `StartFailed` 또는 `Stopped`면 handler는 호출되지 않는다.
    /// DNS resolution은 이 component의 책임이 아니며 numeric IPv4만 받는다.
    ///
    /// @throws std::invalid_argument option 또는 handler가 유효하지 않은 경우.
    /// @throws std::system_error socket의 IOCP association에 실패한 경우.
    /// @throws std::logic_error `IoContext` stop이 먼저 시작된 경우.
    ConnectStartResult Connect(
        TcpConnectOptions options,
        ConnectHandler handler);

    /// @brief 신규 connect를 막고 pending socket을 닫아 cancellation을 요청한다.
    ///
    /// `WaitStopped` barrier는 accepted connect handler의 반환까지 포함한다.
    void Stop() noexcept;

    bool WaitStopped(std::chrono::milliseconds timeout);
    TcpConnectorSnapshot Snapshot() const;

private:
    class ConnectOperation;

    TcpConnector(
        runtime::IoContext& io_context,
        std::shared_ptr<core::Logger> logger);

    void OnConnectComplete(
        std::uint64_t operation_id,
        std::shared_ptr<platform::windows::SocketHandle> socket,
        ConnectHandler handler,
        std::error_code error) noexcept;
    bool MoveToStoppedIfDrainedLocked() noexcept;

    runtime::IoContext& io_context_;
    std::shared_ptr<core::Logger> logger_;

    mutable std::mutex mutex_;
    std::condition_variable stopped_condition_;
    TcpConnectorState state_{TcpConnectorState::Running};
    std::uint64_t next_operation_id_{1};
    std::unordered_map<
        std::uint64_t,
        std::shared_ptr<platform::windows::SocketHandle>>
        pending_sockets_;
    std::size_t outstanding_connects_{0};
    std::uint64_t successful_connects_{0};
    std::uint64_t failed_connects_{0};
    std::uint64_t cancelled_connects_{0};
};

} // namespace iocp::transport
