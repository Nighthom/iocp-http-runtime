// tcp_connection.cpp
// TcpConnection의 IOCP completion 처리, send queue drain, 상태 전이를
// 구현한다. 한 연결당 하나의 WSASend/WSARecv만 유지하는 ordering
// contract를 IOCP worker 내에서 보존한다. close는 pending operation이
// 모두 drain된 후 socket을 닫고 registry에서 제거하는 순서로 진행된다.
#include "transport/tcp_connection.h"

#include "runtime/completion_operation.h"
#include "transport/connection_registry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iocp::transport
{

class TcpConnection::ReceiveOperation final :
    public runtime::CompletionOperation
{
public:
    ReceiveOperation(
        std::shared_ptr<TcpConnection> connection,
        const std::size_t receive_chunk_bytes)
        : connection_(std::move(connection)),
          storage_(receive_chunk_bytes)
    {
        buffer_.buf = reinterpret_cast<char*>(storage_.data());
        buffer_.len = static_cast<ULONG>(storage_.size());
    }

    void Complete(
        const std::uint32_t transferred_bytes,
        std::error_code error,
        std::uintptr_t) noexcept override
    {
        connection_->OnReceiveComplete(
            buffer::ByteView(storage_.data(), storage_.size()),
            transferred_bytes,
            error);
    }

    WSABUF* Buffer() noexcept
    {
        return &buffer_;
    }

    DWORD* Flags() noexcept
    {
        return &flags_;
    }

    DWORD* ImmediateBytes() noexcept
    {
        return &immediate_bytes_;
    }

private:
    std::shared_ptr<TcpConnection> connection_;
    std::vector<std::byte> storage_;
    WSABUF buffer_{};
    DWORD flags_{0};
    DWORD immediate_bytes_{0};
};

class TcpConnection::SendOperation final :
    public runtime::CompletionOperation
{
public:
    SendOperation(
        std::shared_ptr<TcpConnection> connection,
        SendGather gather)
        : connection_(std::move(connection)),
          gather_(std::move(gather))
    {
        buffers_.reserve(gather_.slices.size());
        for (const SendSlice& slice : gather_.slices)
        {
            WSABUF buffer{};
            buffer.buf = reinterpret_cast<char*>(
                const_cast<std::byte*>(
                    slice.buffer->data() + slice.offset));
            buffer.len = static_cast<ULONG>(slice.size);
            buffers_.push_back(buffer);
        }
    }

    void Complete(
        const std::uint32_t transferred_bytes,
        std::error_code error,
        std::uintptr_t) noexcept override
    {
        connection_->OnSendComplete(
            transferred_bytes,
            gather_.total_bytes,
            error);
    }

    WSABUF* Buffers() noexcept
    {
        return buffers_.data();
    }

    DWORD BufferCount() const noexcept
    {
        return static_cast<DWORD>(buffers_.size());
    }

    DWORD* ImmediateBytes() noexcept
    {
        return &immediate_bytes_;
    }

private:
    std::shared_ptr<TcpConnection> connection_;
    SendGather gather_;
    std::vector<WSABUF> buffers_;
    DWORD immediate_bytes_{0};
};

std::shared_ptr<TcpConnection> TcpConnection::Create(
    const ConnectionId id,
    platform::windows::SocketHandle socket,
    std::weak_ptr<ConnectionRegistry> registry,
    std::shared_ptr<core::Logger> logger,
    ReceiveHandler receive_handler,
    ConnectionOptions options)
{
    return std::shared_ptr<TcpConnection>(new TcpConnection(
        id,
        std::move(socket),
        std::move(registry),
        std::move(logger),
        std::move(receive_handler),
        options));
}

TcpConnection::TcpConnection(
    const ConnectionId id,
    platform::windows::SocketHandle socket,
    std::weak_ptr<ConnectionRegistry> registry,
    std::shared_ptr<core::Logger> logger,
    ReceiveHandler receive_handler,
    const ConnectionOptions options)
    : id_(id),
      socket_(std::move(socket)),
      registry_(std::move(registry)),
      logger_(std::move(logger)),
      receive_handler_(std::move(receive_handler)),
      receive_chunk_bytes_(options.receive_chunk_bytes),
      maximum_gather_segments_per_operation_(
          options.maximum_gather_segments_per_operation),
      maximum_gather_bytes_per_operation_(
          options.maximum_gather_bytes_per_operation),
      maximum_outbound_batch_segments_(
          options.maximum_outbound_batch_segments),
      socket_options_(options.socket),
      send_queue_(
          options.maximum_send_queue_items,
          options.maximum_send_queue_bytes),
      connection_timeout_(options.connection_timeout)
{
    if (id_ == 0)
    {
        throw std::invalid_argument("connection id는 0일 수 없습니다");
    }
    if (!socket_)
    {
        throw std::invalid_argument("TcpConnection에는 유효한 socket이 필요합니다");
    }
    if (!logger_)
    {
        throw std::invalid_argument("TcpConnection에는 Logger가 필요합니다");
    }
    if (!receive_handler_)
    {
        throw std::invalid_argument("TcpConnection에는 receive handler가 필요합니다");
    }
    if (receive_chunk_bytes_ == 0 ||
        receive_chunk_bytes_ > std::numeric_limits<ULONG>::max())
    {
        throw std::invalid_argument(
            "receive chunk bytes는 1..ULONG_MAX 범위여야 합니다");
    }
    if (maximum_gather_segments_per_operation_ == 0 ||
        maximum_gather_segments_per_operation_ >
            std::numeric_limits<DWORD>::max())
    {
        throw std::invalid_argument(
            "gather segment 상한은 1..DWORD_MAX 범위여야 합니다");
    }
    if (maximum_gather_bytes_per_operation_ == 0 ||
        maximum_gather_bytes_per_operation_ >
            std::numeric_limits<ULONG>::max())
    {
        throw std::invalid_argument(
            "gather byte 상한은 1..ULONG_MAX 범위여야 합니다");
    }
    if (maximum_outbound_batch_segments_ == 0)
    {
        throw std::invalid_argument(
            "outbound batch segment 상한은 1 이상이어야 합니다");
    }
    if (maximum_gather_segments_per_operation_ >
            send_queue_.MaximumItems() ||
        maximum_outbound_batch_segments_ >
            send_queue_.MaximumItems())
    {
        throw std::invalid_argument(
            "gather/batch segment 상한은 send queue item 상한 이하여야 합니다");
    }
    if (maximum_gather_bytes_per_operation_ >
        send_queue_.MaximumBytes())
    {
        throw std::invalid_argument(
            "gather byte 상한은 send queue byte 상한 이하여야 합니다");
    }
}

TcpConnection::~TcpConnection() = default;

ConnectionId TcpConnection::Id() const noexcept
{
    return id_;
}

bool TcpConnection::Start() noexcept
{
    // connected socket에 TCP_NODELAY, keepalive 등 native option 적용.
    // 실패는 로깅만 하고 연결은 계속 진행한다.
    ApplySocketOptions();

    int error = 0;
    bool remove_from_registry = false;
    try
    {
        std::lock_guard lock(mutex_);
        error = PostReceiveLocked();
        if (error != 0)
        {
            BeginCloseLocked(CloseReason::ReceiveError);
            remove_from_registry = MoveToClosedIfDrainedLocked();
        }
    }
    catch (...)
    {
        BeginClose(CloseReason::ReceiveError);
        return false;
    }

    if (error != 0)
    {
        const std::string id_text = std::to_string(id_);
        const std::string error_text = std::to_string(error);
        logger_->Log(
            core::LogLevel::Error,
            "connection.receive_post_failed",
            "첫 WSARecv 등록에 실패했습니다.",
            {
                {"connection_id", id_text},
                {"win32_error", error_text},
            });
        LogClose(CloseReason::ReceiveError);
    }

    if (remove_from_registry)
    {
        closed_condition_.notify_all();
        RemoveFromRegistry();
    }
    return error == 0;
}

SendStatus TcpConnection::Send(OutboundBytes bytes) noexcept
{
    try
    {
        OutboundBatch batch;
        batch.push_back(std::move(bytes));
        return SendBatch(std::move(batch));
    }
    catch (...)
    {
        BeginClose(CloseReason::HandlerError);
        return SendStatus::StartFailed;
    }
}

SendStatus TcpConnection::SendBatch(OutboundBatch segments) noexcept
{
    return SendBatchInternal(std::move(segments), false);
}

SendStatus TcpConnection::SendBatchAndClose(
    OutboundBatch segments) noexcept
{
    return SendBatchInternal(std::move(segments), true);
}

SendStatus TcpConnection::SendBatchInternal(
    OutboundBatch segments,
    const bool close_after_send) noexcept
{
    const std::size_t segment_count = static_cast<std::size_t>(
        std::count_if(
            segments.begin(),
            segments.end(),
            [](const OutboundBytes& segment) {
                return !segment.empty();
            }));
    if (segment_count == 0)
    {
        if (close_after_send)
        {
            BeginClose(CloseReason::ProtocolClose);
        }
        return SendStatus::Accepted;
    }

    std::vector<SharedSendBuffer> buffers;
    try
    {
        if (segment_count <= maximum_outbound_batch_segments_)
        {
            buffers.reserve(segment_count);
            for (OutboundBytes& segment : segments)
            {
                if (!segment.empty())
                {
                    buffers.push_back(
                        std::make_shared<const OutboundBytes>(
                            std::move(segment)));
                }
            }
        }
    }
    catch (...)
    {
        BeginClose(CloseReason::HandlerError);
        return SendStatus::StartFailed;
    }

    int post_error = 0;
    bool close_started = false;
    bool remove_from_registry = false;
    SendStatus status = SendStatus::Accepted;

    try
    {
        std::lock_guard lock(mutex_);
        if (state_ != ConnectionState::Active || close_after_send_)
        {
            return SendStatus::Closed;
        }

        if (segment_count > maximum_outbound_batch_segments_ ||
            !send_queue_.TryPushBatch(std::move(buffers)))
        {
            close_started = BeginCloseLocked(
                CloseReason::SendQueueOverflow);
            remove_from_registry = MoveToClosedIfDrainedLocked();
            status = SendStatus::QueueOverflow;
        }
        else
        {
            close_after_send_ = close_after_send;
            if (!send_in_flight_)
            {
                post_error = PostSendLocked();
                if (post_error != 0)
                {
                    close_started = BeginCloseLocked(CloseReason::SendError);
                    remove_from_registry = MoveToClosedIfDrainedLocked();
                    status = SendStatus::StartFailed;
                }
            }
        }
    }
    catch (...)
    {
        BeginClose(CloseReason::SendError);
        return SendStatus::StartFailed;
    }

    if (status == SendStatus::QueueOverflow)
    {
        const std::string id_text = std::to_string(id_);
        logger_->Log(
            core::LogLevel::Warning,
            "connection.send_queue_overflow",
            "send queue 상한을 넘어 connection을 종료합니다.",
            {{"connection_id", id_text}});
    }
    else if (post_error != 0)
    {
        const std::string id_text = std::to_string(id_);
        const std::string error_text = std::to_string(post_error);
        logger_->Log(
            core::LogLevel::Error,
            "connection.send_post_failed",
            "WSASend 등록에 실패했습니다.",
            {
                {"connection_id", id_text},
                {"win32_error", error_text},
            });
    }

    if (close_started)
    {
        LogClose(status == SendStatus::QueueOverflow
                     ? CloseReason::SendQueueOverflow
                     : CloseReason::SendError);
    }
    if (remove_from_registry)
    {
        closed_condition_.notify_all();
        RemoveFromRegistry();
    }
    return status;
}

int TcpConnection::PostReceiveLocked()
{
    if (state_ != ConnectionState::Active || receive_in_flight_)
    {
        return state_ == ConnectionState::Active ? WSAEALREADY : WSAESHUTDOWN;
    }

    auto operation = std::make_unique<ReceiveOperation>(
        shared_from_this(),
        receive_chunk_bytes_);

    ++outstanding_operations_;
    receive_in_flight_ = true;
    const int result = ::WSARecv(
        socket_.Get(),
        operation->Buffer(),
        1,
        operation->ImmediateBytes(),
        operation->Flags(),
        operation->NativeHandle(),
        nullptr);

    if (result == SOCKET_ERROR)
    {
        const int error = ::WSAGetLastError();
        if (error != WSA_IO_PENDING)
        {
            if (outstanding_operations_ > 0)
                --outstanding_operations_;
            receive_in_flight_ = false;
            UpdateIdleTimerLocked();
            return error;
        }
    }

    operation.release();
    return 0;
}

int TcpConnection::PostSendLocked()
{
    if (state_ != ConnectionState::Active || send_in_flight_ ||
        send_queue_.Empty())
    {
        return state_ == ConnectionState::Active ? WSAEALREADY : WSAESHUTDOWN;
    }

    // send queue에서 configured gather 상한(segment 수, byte 수)까지
    // slice를 모아 하나의 WSASend로 제출한다. Gather가 반환하는 각
    // slice는 원본 buffer의 shared_ptr을 참조하므로 WSASend completion
    // 시점까지 buffer 수명이 보존된다.
    SendGather gather = send_queue_.Gather(
        maximum_gather_segments_per_operation_,
        maximum_gather_bytes_per_operation_);
    if (gather.Empty())
    {
        return WSAEINVAL;
    }

    auto operation = std::make_unique<SendOperation>(
        shared_from_this(),
        std::move(gather));

    ++outstanding_operations_;
    send_in_flight_ = true;
    const int result = ::WSASend(
        socket_.Get(),
        operation->Buffers(),
        operation->BufferCount(),
        operation->ImmediateBytes(),
        0,
        operation->NativeHandle(),
        nullptr);

    if (result == SOCKET_ERROR)
    {
        const int error = ::WSAGetLastError();
        if (error != WSA_IO_PENDING)
        {
            if (outstanding_operations_ > 0)
                --outstanding_operations_;
            send_in_flight_ = false;
            UpdateIdleTimerLocked();
            return error;
        }
    }

    operation.release();
    return 0;
}

void TcpConnection::OnReceiveComplete(
    const buffer::ByteView storage,
    const std::uint32_t transferred_bytes,
    const std::error_code error) noexcept
{
    bool deliver = false;
    bool post_next_receive = false;
    bool remove_from_registry = false;
    CloseReason close_to_log = CloseReason::None;

    try
    {
        {
            std::lock_guard lock(mutex_);
            if (outstanding_operations_ > 0)
            {
                if (outstanding_operations_ > 0)
                --outstanding_operations_;
            }
            receive_in_flight_ = false;
            UpdateIdleTimerLocked();

            // guard clause chain: error → peer close(0 bytes) → overflow →
            // normal delivery 순으로 검사한다. Active 상태일 때만 상태
            // 변경을 허용해 이미 Closing/Closed인 connection에서 중복
            // close가 발생하지 않도록 한다.
            if (error)
            {
                if (state_ == ConnectionState::Active)
                {
                    BeginCloseLocked(CloseReason::ReceiveError);
                    close_to_log = CloseReason::ReceiveError;
                }
            }
            else if (transferred_bytes == 0)
            {
                // TCP FIN 수신: peer가 정상적으로 연결을 닫았다.
                if (state_ == ConnectionState::Active)
                {
                    BeginCloseLocked(CloseReason::PeerClosed);
                    close_to_log = CloseReason::PeerClosed;
                }
            }
            else if (transferred_bytes > storage.Size())
            {
                // 버퍼 경계를 넘는 전송량은 프로토콜 위반 또는 버그다.
                if (state_ == ConnectionState::Active)
                {
                    BeginCloseLocked(CloseReason::ReceiveError);
                    close_to_log = CloseReason::ReceiveError;
                }
            }
            else if (state_ == ConnectionState::Active)
            {
                received_bytes_ += transferred_bytes;
                deliver = true;
                post_next_receive = true;
            }

            remove_from_registry = MoveToClosedIfDrainedLocked();
        }

        if (close_to_log != CloseReason::None)
        {
            LogClose(close_to_log);
        }

        if (remove_from_registry)
        {
            closed_condition_.notify_all();
            RemoveFromRegistry();
            return;
        }

        if (deliver)
        {
            receive_handler_(
                shared_from_this(),
                storage.SubView(0, transferred_bytes));
        }

        if (post_next_receive)
        {
            int post_error = 0;
            {
                std::lock_guard lock(mutex_);
                if (state_ == ConnectionState::Active)
                {
                    post_error = PostReceiveLocked();
                    if (post_error != 0)
                    {
                        BeginCloseLocked(CloseReason::ReceiveError);
                        remove_from_registry = MoveToClosedIfDrainedLocked();
                    }
                }
            }

            if (post_error != 0)
            {
                const std::string id_text = std::to_string(id_);
                const std::string error_text = std::to_string(post_error);
                logger_->Log(
                    core::LogLevel::Error,
                    "connection.receive_post_failed",
                    "다음 WSARecv 등록에 실패했습니다.",
                    {
                        {"connection_id", id_text},
                        {"win32_error", error_text},
                    });
                LogClose(CloseReason::ReceiveError);
            }

            if (remove_from_registry)
            {
                closed_condition_.notify_all();
                RemoveFromRegistry();
            }
        }
    }
    catch (const std::exception& exception)
    {
        logger_->Log(
            core::LogLevel::Error,
            "connection.receive_handler_failed",
            "receive completion 처리 중 예외가 발생했습니다.",
            {{"exception", exception.what()}});
        BeginClose(CloseReason::HandlerError);
    }
    catch (...)
    {
        logger_->Log(
            core::LogLevel::Error,
            "connection.receive_handler_failed",
            "receive completion 처리 중 알 수 없는 예외가 발생했습니다.");
        BeginClose(CloseReason::HandlerError);
    }
}

void TcpConnection::OnSendComplete(
    const std::uint32_t transferred_bytes,
    const std::size_t submitted_bytes,
    const std::error_code error) noexcept
{
    int post_error = 0;
    bool remove_from_registry = false;
    CloseReason close_to_log = CloseReason::None;

    try
    {
        {
            std::lock_guard lock(mutex_);
            if (outstanding_operations_ > 0)
            {
                if (outstanding_operations_ > 0)
                --outstanding_operations_;
            }
            send_in_flight_ = false;
            UpdateIdleTimerLocked();

            // guard clause chain: send error → partial send/overflow/invalid
            // consume → next send → close_after_send shutdown 순으로 처리.
            // 전송량 검증(0 byte, overflow)은 TCP send completion이 항상 모든
            // byte를 전송하지 않을 수 있음을 고려한 방어 코드다.
            if (error)
            {
                if (state_ == ConnectionState::Active)
                {
                    BeginCloseLocked(CloseReason::SendError);
                    close_to_log = CloseReason::SendError;
                }
            }
            else if (state_ == ConnectionState::Active)
            {
                if (transferred_bytes == 0 ||
                    transferred_bytes > submitted_bytes ||
                    send_queue_.Consume(transferred_bytes) ==
                        SendConsumeResult::Invalid)
                {
                    // 0 byte 전송은 socket 오류, overflow/invalid consume은
                    // 내부 상태 불일치를 의미하므로 연결을 종료한다.
                    BeginCloseLocked(CloseReason::SendError);
                    close_to_log = CloseReason::SendError;
                }
                else
                {
                    sent_bytes_ += transferred_bytes;
                    if (!send_queue_.Empty())
                    {
                        // 아직 보낼 데이터가 남았으면 다음 send를 즉시 등록한다.
                        // send_in_flight_가 false인 상태이므로 PostSendLocked가
                        // 성공할 수 있다.
                        post_error = PostSendLocked();
                        if (post_error != 0)
                        {
                            BeginCloseLocked(CloseReason::SendError);
                            close_to_log = CloseReason::SendError;
                        }
                    }
                    else if (close_after_send_)
                    {
                        // 모든 데이터가 전송된 후 close_after_send_ 플래그가
                        // 설정되어 있으면 shutdown(SD_SEND)로 half-close 후
                        // 연결을 종료한다.
                        static_cast<void>(
                            ::shutdown(socket_.Get(), SD_SEND));
                        BeginCloseLocked(CloseReason::ProtocolClose);
                        close_to_log = CloseReason::ProtocolClose;
                    }
                }
            }

            remove_from_registry = MoveToClosedIfDrainedLocked();
        }

        if (post_error != 0)
        {
            const std::string id_text = std::to_string(id_);
            const std::string error_text = std::to_string(post_error);
            logger_->Log(
                core::LogLevel::Error,
                "connection.send_post_failed",
                "partial send 또는 다음 item의 WSASend 등록에 실패했습니다.",
                {
                    {"connection_id", id_text},
                    {"win32_error", error_text},
                });
        }
        if (close_to_log != CloseReason::None)
        {
            LogClose(close_to_log);
        }
        if (remove_from_registry)
        {
            closed_condition_.notify_all();
            RemoveFromRegistry();
        }
    }
    catch (...)
    {
        BeginClose(CloseReason::SendError);
    }
}

void TcpConnection::BeginClose(const CloseReason reason) noexcept
{
    bool close_started = false;
    bool remove_from_registry = false;
    try
    {
        std::lock_guard lock(mutex_);
        close_started = BeginCloseLocked(reason);
        remove_from_registry = MoveToClosedIfDrainedLocked();
    }
    catch (...)
    {
        return;
    }

    if (close_started)
    {
        LogClose(reason);
    }
    if (remove_from_registry)
    {
        closed_condition_.notify_all();
        RemoveFromRegistry();
    }
}

bool TcpConnection::BeginCloseLocked(const CloseReason reason) noexcept
{
    // 이미 Closing 또는 Closed면 중복 close를 막는다.
    // close reason은 최초 호출 시점의 것을 보존한다.
    if (state_ != ConnectionState::Active)
    {
        return false;
    }

    state_ = ConnectionState::Closing;
    close_reason_ = reason;
    close_after_send_ = false;
    CancelIdleTimer();
    // socket을 먼저 닫아 pending I/O의 cancellation completion을 유도한다.
    // 이 시점부터 PostReceiveLocked와 PostSendLocked는 WSAESHUTDOWN을 반환한다.
    socket_.Reset();
    return true;
}

bool TcpConnection::MoveToClosedIfDrainedLocked() noexcept
{
    // Closing 상태이고 모든 outstanding operation이 완료(drain)되었을 때만
    // Closed로 전이한다. 이 설계는 I/O completion과 close 타이밍 간 race
    // condition을 방지한다: cancellation completion이 도착하기 전에
    // connection이 registry에서 제거되는 것을 막는다.
    if (state_ == ConnectionState::Closing &&
        outstanding_operations_ == 0)
    {
        state_ = ConnectionState::Closed;
        send_queue_.Clear();
        return true;
    }
    return false;
}

void TcpConnection::RemoveFromRegistry() noexcept
{
    if (const auto registry = registry_.lock())
    {
        registry->Remove(id_);
    }
}

void TcpConnection::LogClose(const CloseReason reason) noexcept
{
    try
    {
        const std::string id_text = std::to_string(id_);
        logger_->Log(
            core::LogLevel::Info,
            "connection.close",
            "TCP connection 종료를 시작했습니다.",
            {
                {"connection_id", id_text},
                {"reason", CloseReasonName(reason)},
            });
    }
    catch (...)
    {
        logger_->Log(
            core::LogLevel::Info,
            "connection.close",
            "TCP connection 종료를 시작했습니다.");
    }
}

bool TcpConnection::WaitClosed(const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return closed_condition_.wait_for(lock, timeout, [this] {
        return state_ == ConnectionState::Closed;
    });
}

ConnectionSnapshot TcpConnection::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return ConnectionSnapshot{
        id_,
        state_,
        close_reason_,
        outstanding_operations_,
        receive_in_flight_,
        send_in_flight_,
        send_queue_.ItemCount(),
        send_queue_.QueuedBytes(),
        received_bytes_,
        sent_bytes_,
    };
}

bool TcpConnection::ApplySocketOptions() noexcept
{
    if (!socket_)
    {
        return false;
    }

    const SOCKET s = socket_.Get();
    bool result = true;

    if (socket_options_.tcp_nodelay)
    {
        constexpr BOOL enable = TRUE;
        if (::setsockopt(
                s, IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&enable),
                sizeof(enable)) == SOCKET_ERROR)
        {
            result = false;
        }
    }

    if (socket_options_.keepalive_enabled)
    {
        constexpr BOOL enable = TRUE;
        if (::setsockopt(
                s, SOL_SOCKET, SO_KEEPALIVE,
                reinterpret_cast<const char*>(&enable),
                sizeof(enable)) == SOCKET_ERROR)
        {
            result = false;
        }
    }

    return result;
}

void TcpConnection::SetTimerService(
    std::shared_ptr<core::TimerService> timer)
{
    std::lock_guard lock(mutex_);
    timer_service_ = std::move(timer);
    if (connection_timeout_ > std::chrono::milliseconds::zero())
        UpdateIdleTimerLocked();
}

void TcpConnection::UpdateIdleTimerLocked()
{
    if (!timer_service_ || connection_timeout_ <= std::chrono::milliseconds::zero())
        return;
    if (state_ != ConnectionState::Active)
        return;

    auto self = shared_from_this();
    idle_timer_id_ = timer_service_->Reschedule(
        idle_timer_id_,
        connection_timeout_,
        [self] {
            self->BeginClose(CloseReason::None);
        });
}

void TcpConnection::CancelIdleTimer() noexcept
{
    if (timer_service_ && idle_timer_id_ != 0)
    {
        timer_service_->Cancel(idle_timer_id_);
        idle_timer_id_ = 0;
    }
}

std::string_view CloseReasonName(const CloseReason reason) noexcept
{
    switch (reason)
    {
    case CloseReason::None:
        return "none";
    case CloseReason::LocalShutdown:
        return "local_shutdown";
    case CloseReason::PeerClosed:
        return "peer_closed";
    case CloseReason::ReceiveError:
        return "receive_error";
    case CloseReason::SendError:
        return "send_error";
    case CloseReason::SendQueueOverflow:
        return "send_queue_overflow";
    case CloseReason::HandlerError:
        return "handler_error";
    case CloseReason::ProtocolClose:
        return "protocol_close";
    }
    return "unknown";
}

} // namespace iocp::transport
