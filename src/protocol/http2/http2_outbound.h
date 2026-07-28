/// @file http2_outbound.h
/// @brief HTTP/2 response HPACK state와 outbound flow control scheduler

#pragma once

#include "protocol/http/http_message.h"
#include "protocol/http2/http2_hpack.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace iocp::protocol::http2
{

class H2OutboundScheduler final
{
public:
    using FrameSender =
        std::function<void(std::vector<std::byte> frame)>;

    H2OutboundScheduler(
        FrameSender frame_sender,
        std::uint32_t initial_window_size,
        std::uint32_t maximum_frame_size,
        HpackOptions hpack_options);

    void OpenStream(std::uint32_t stream_id);
    void CloseStream(std::uint32_t stream_id);
    void Close();

    bool SubmitResponse(
        std::uint32_t stream_id,
        http::HttpResponse response);

    bool ApplyPeerSettings(
        std::uint32_t initial_window_size,
        std::uint32_t maximum_frame_size,
        std::size_t header_table_size);
    bool UpdateConnectionWindow(std::uint32_t increment);
    bool UpdateStreamWindow(
        std::uint32_t stream_id,
        std::uint32_t increment);

private:
    struct StreamOutput final
    {
        std::int64_t window{};
        std::vector<std::byte> body;
        std::size_t offset{};
        bool response_submitted{};
    };

    void EncodeHeaderBlockLocked(
        std::uint32_t stream_id,
        http::HttpResponse response,
        bool end_stream,
        std::vector<std::vector<std::byte>>& frames);
    bool FlushStreamLocked(
        std::uint32_t stream_id,
        StreamOutput& stream,
        std::vector<std::vector<std::byte>>& frames);
    void FlushAllLocked(
        std::vector<std::vector<std::byte>>& frames);
    void SendFrames(
        std::vector<std::vector<std::byte>> frames);

    FrameSender frame_sender_;
    std::mutex mutex_;
    bool closed_{};
    std::int64_t connection_window_{65535};
    std::uint32_t initial_window_size_{65535};
    std::uint32_t maximum_frame_size_{16384};
    std::size_t maximum_encoder_table_size_{4096};
    HpackCodec encoder_;
    std::unordered_map<std::uint32_t, StreamOutput> streams_;
};

} // namespace iocp::protocol::http2
