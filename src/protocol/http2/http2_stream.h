/// @file http2_stream.h
/// @brief HTTP/2 stream state machine 및 connection-level session management

#pragma once

#include "protocol/http/http_message.h"
#include "protocol/http/http_router.h"
#include "protocol/http2/http2_frames.h"
#include "protocol/http2/http2_hpack.h"
#include "protocol/protocol_session.h"
#include "buffer/ring_receive_buffer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iocp::protocol::http2
{

enum class StreamState
{
    Idle,
    ReservedLocal,
    ReservedRemote,
    Open,
    HalfClosedLocal,
    HalfClosedRemote,
    Closed,
};

struct StreamSnapshot final
{
    std::uint32_t stream_id{};
    StreamState state{StreamState::Idle};
    std::uint32_t recv_window{65535};
    std::uint32_t send_window{65535};
    std::size_t header_bytes_received{};
    std::size_t body_bytes_received{};
};

/// @brief 단일 HTTP/2 stream의 상태와 수신 데이터를 관리한다.
class H2Stream final
{
public:
    explicit H2Stream(std::uint32_t stream_id);
    ~H2Stream() = default;

    H2Stream(const H2Stream&) = delete;
    H2Stream& operator=(const H2Stream&) = delete;

    std::uint32_t Id() const noexcept { return stream_id_; }
    StreamState State() const noexcept { return state_; }
    void SetState(StreamState state) noexcept { state_ = state; }

    void AppendHeaderFragment(const std::byte* data, std::size_t size);
    void AppendData(const std::byte* data, std::size_t size);

    std::uint32_t RecvWindow() const noexcept { return recv_window_; }
    void ConsumeRecvWindow(std::uint32_t bytes) noexcept;
    void AddRecvWindow(std::uint32_t bytes) noexcept;

    std::uint32_t SendWindow() const noexcept { return send_window_; }
    void ConsumeSendWindow(std::uint32_t bytes) noexcept;
    void AddSendWindow(std::uint32_t bytes) noexcept;

    const std::vector<std::byte>& HeaderBlock() const noexcept
    {
        return header_block_;
    }
    const std::vector<std::byte>& Body() const noexcept
    {
        return body_;
    }
    bool EndStream() const noexcept { return end_stream_; }
    void SetEndStream() noexcept { end_stream_ = true; }
    bool HeadersComplete() const noexcept { return headers_complete_; }
    void SetHeadersComplete() noexcept { headers_complete_ = true; }
    bool Dispatched() const noexcept { return dispatched_; }
    void SetDispatched() noexcept { dispatched_ = true; }
    void SetRequest(http::HttpRequest request);
    bool HasRequest() const noexcept { return request_.has_value(); }
    http::HttpRequest TakeRequest();

    StreamSnapshot Snapshot() const;

private:
    std::uint32_t stream_id_;
    StreamState state_{StreamState::Idle};
    std::uint32_t recv_window_{65535};
    std::uint32_t send_window_{65535};
    std::vector<std::byte> header_block_;
    std::vector<std::byte> body_;
    bool end_stream_{};
    bool headers_complete_{};
    bool dispatched_{};
    std::optional<http::HttpRequest> request_;
};

struct H2ConnectionConfig final
{
    std::size_t initial_receive_buffer_size{4096};
    std::size_t maximum_receive_buffer_size{1024 * 1024};
    std::size_t maximum_request_body_size{1024 * 1024};
    std::size_t maximum_concurrent_streams{100};
    std::uint32_t initial_window_size{65535};
    std::uint32_t maximum_frame_size{16384};
    std::uint32_t maximum_header_list_size{16384};
    std::uint32_t header_table_size{4096};
    bool enable_push{false};
};

enum class H2ConnectionState
{
    ExpectPreface,
    Connected,
    Closing,
    Closed,
};

enum class H2FeedStatus
{
    Ready,
    ProtocolError,
    Goaway,
    Stopped,
};

/// @brief HTTP/2 연결 하나의 frame 처리, stream 관리, flow control을 담당한다.
class H2Session final : public IProtocolSession
{
public:
    using ResponseSender = std::function<void(
        std::uint32_t stream_id,
        http::HttpResponse response)>;
    using FrameSender = std::function<void(
        std::vector<std::byte> frame_data)>;

    H2Session(
        std::shared_ptr<http::HttpRouter> router,
        std::shared_ptr<execution::IExecutor> executor,
        ResponseSender response_sender,
        FrameSender frame_sender,
        std::uint64_t connection_id,
        H2ConnectionConfig config = {});

    ProtocolFeedResult Feed(buffer::ByteView bytes) override;

    bool IsStopped() const noexcept;
    std::uint32_t LastStreamId() const noexcept;

    void SendGoaway(
        ErrorCode error_code = ErrorCode::NoError);
    void Close();

private:
    ProtocolFeedStatus ConsumePreface(bool& consumed);
    ProtocolFeedStatus ConsumeFrame(
        bool& consumed,
        std::size_t& dispatching);
    ProtocolFeedStatus HandleSettings(
        const FrameHeader& header,
        const std::byte* payload);
    ProtocolFeedStatus HandleHeaders(
        const FrameHeader& header,
        const std::byte* payload,
        std::size_t& dispatching);
    ProtocolFeedStatus HandleData(
        const FrameHeader& header,
        const std::byte* payload,
        std::size_t& dispatching);
    ProtocolFeedStatus HandleRstStream(
        const FrameHeader& header,
        const std::byte* payload);
    ProtocolFeedStatus HandleWindowUpdate(
        const FrameHeader& header,
        const std::byte* payload);
    ProtocolFeedStatus HandlePing(
        const FrameHeader& header,
        const std::byte* payload);
    ProtocolFeedStatus HandleGoaway(
        const FrameHeader& header,
        const std::byte* payload);

    H2Stream* GetOrCreateStream(
        std::uint32_t stream_id,
        bool remote_initiated = true);
    void CloseStream(std::uint32_t stream_id);
    ProtocolFeedStatus DispatchRequest(
        std::uint32_t stream_id,
        std::size_t& dispatching);

    void SendFrame(const FrameHeader& header,
        const std::vector<std::byte>& payload = {});
    void SendSettings();

    std::shared_ptr<http::HttpRouter> router_;
    std::shared_ptr<execution::IExecutor> executor_;
    ResponseSender response_sender_;
    FrameSender frame_sender_;
    std::uint64_t connection_id_;
    H2ConnectionConfig config_;

    mutable std::mutex mutex_;
    buffer::RingReceiveBuffer receive_buffer_;
    HpackCodec hpack_decoder_;
    HpackCodec hpack_encoder_;
    H2ConnectionState state_{H2ConnectionState::ExpectPreface};
    std::uint32_t last_stream_id_{};
    std::uint32_t remote_settings_header_table_size_{4096};
    std::uint32_t remote_settings_max_concurrent_streams_{100};
    std::uint32_t remote_settings_initial_window_size_{65535};
    std::uint32_t connection_recv_window_{65535};
    std::uint32_t connection_send_window_{65535};
    bool received_initial_settings_{};
    std::optional<std::uint32_t> continuation_stream_id_;

    std::unordered_map<std::uint32_t, std::unique_ptr<H2Stream>> streams_;
};

} // namespace iocp::protocol::http2
