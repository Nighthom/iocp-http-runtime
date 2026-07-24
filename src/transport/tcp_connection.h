#pragma once

#include "buffer/byte_view.h"
#include "core/logging.h"
#include "platform/windows/socket_handle.h"
#include "transport/send_queue.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace iocp::transport
{

class ConnectionRegistry;

using ConnectionId = std::uint64_t;

enum class ConnectionState
{
    Active,
    Closing,
    Closed,
};

enum class CloseReason
{
    None,
    LocalShutdown,
    PeerClosed,
    ReceiveError,
    SendError,
    SendQueueOverflow,
    HandlerError,
    ProtocolClose,
};

struct ConnectionSnapshot final
{
    ConnectionId id{};
    ConnectionState state{ConnectionState::Closed};
    CloseReason close_reason{CloseReason::None};
    std::size_t outstanding_operations{};
    bool receive_in_flight{};
    bool send_in_flight{};
    std::size_t queued_send_items{};
    std::size_t queued_send_bytes{};
    std::uint64_t received_bytes{};
    std::uint64_t sent_bytes{};
};

struct ConnectionOptions final
{
    std::size_t maximum_send_queue_items{64};
    std::size_t maximum_send_queue_bytes{1024 * 1024};
    std::size_t receive_chunk_bytes{4096};
    std::size_t maximum_gather_segments_per_operation{16};
    std::size_t maximum_gather_bytes_per_operation{64 * 1024};
    std::size_t maximum_outbound_batch_segments{16};
};

enum class SendStatus
{
    Accepted,
    Closed,
    QueueOverflow,
    StartFailed,
};

/// @brief connected socket과 overlapped receive 수명을 관리한다.
class TcpConnection final :
    public std::enable_shared_from_this<TcpConnection>
{
public:
    using OutboundBytes = std::vector<std::byte>;
    using OutboundBatch = std::vector<OutboundBytes>;

    /// @brief receive completion의 borrowed byte 범위를 전달하는 handler다.
    ///
    /// `bytes`는 handler가 반환될 때까지만 유효하다. 이후에도 필요하면
    /// handler 안에서 own storage나 `ReceiveBuffer`로 복사해야 한다.
    /// handler는 connection mutex 밖의 IOCP worker에서 실행된다.
    using ReceiveHandler = std::function<void(
        const std::shared_ptr<TcpConnection>& connection,
        buffer::ByteView bytes)>;

    static std::shared_ptr<TcpConnection> Create(
        ConnectionId id,
        platform::windows::SocketHandle socket,
        std::weak_ptr<ConnectionRegistry> registry,
        std::shared_ptr<core::Logger> logger,
        ReceiveHandler receive_handler,
        ConnectionOptions options = {});

    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    ConnectionId Id() const noexcept;

    /// @brief 첫 receive를 등록한다.
    ///
    /// immediate failure면 connection close를 시작하고 `false`를 반환한다.
    bool Start() noexcept;

    /// @brief immutable send buffer를 bounded FIFO queue에 추가한다.
    ///
    /// 한 connection에는 최대 하나의 `WSASend`만 outstanding으로 둔다.
    /// queue 상한을 넘으면 M2 정책에 따라 connection close를 시작한다.
    SendStatus Send(OutboundBytes bytes) noexcept;

    /// @brief 여러 immutable segment를 하나의 atomic batch로 enqueue한다.
    ///
    /// 모든 non-empty segment가 함께 들어가거나 전부 거부된다. 실제
    /// WSASend는 configured gather count/byte 상한까지 여러 segment를
    /// descriptor 배열로 제출할 수 있다.
    SendStatus SendBatch(OutboundBatch segments) noexcept;

    /// @brief 마지막 batch를 enqueue하고 대기 중인 모든 byte 전송 후 닫는다.
    SendStatus SendBatchAndClose(OutboundBatch segments) noexcept;

    /// @brief 신규 I/O를 막고 socket close를 한 번만 수행한다.
    void BeginClose(CloseReason reason) noexcept;

    bool WaitClosed(std::chrono::milliseconds timeout);
    ConnectionSnapshot Snapshot() const;

private:
    class ReceiveOperation;
    class SendOperation;

    TcpConnection(
        ConnectionId id,
        platform::windows::SocketHandle socket,
        std::weak_ptr<ConnectionRegistry> registry,
        std::shared_ptr<core::Logger> logger,
        ReceiveHandler receive_handler,
        ConnectionOptions options);

    int PostReceiveLocked();
    int PostSendLocked();
    void OnReceiveComplete(
        buffer::ByteView storage,
        std::uint32_t transferred_bytes,
        std::error_code error) noexcept;
    void OnSendComplete(
        std::uint32_t transferred_bytes,
        std::size_t submitted_bytes,
        std::error_code error) noexcept;
    bool BeginCloseLocked(CloseReason reason) noexcept;
    SendStatus SendBatchInternal(
        OutboundBatch segments,
        bool close_after_send) noexcept;
    bool MoveToClosedIfDrainedLocked() noexcept;
    void RemoveFromRegistry() noexcept;
    void LogClose(CloseReason reason) noexcept;

    const ConnectionId id_;
    platform::windows::SocketHandle socket_;
    std::weak_ptr<ConnectionRegistry> registry_;
    std::shared_ptr<core::Logger> logger_;
    ReceiveHandler receive_handler_;
    const std::size_t receive_chunk_bytes_;
    const std::size_t maximum_gather_segments_per_operation_;
    const std::size_t maximum_gather_bytes_per_operation_;
    const std::size_t maximum_outbound_batch_segments_;

    mutable std::mutex mutex_;
    std::condition_variable closed_condition_;
    SendQueue send_queue_;
    ConnectionState state_{ConnectionState::Active};
    CloseReason close_reason_{CloseReason::None};
    std::size_t outstanding_operations_{0};
    bool receive_in_flight_{false};
    bool send_in_flight_{false};
    bool close_after_send_{false};
    std::uint64_t received_bytes_{0};
    std::uint64_t sent_bytes_{0};
};

std::string_view CloseReasonName(CloseReason reason) noexcept;

} // namespace iocp::transport
