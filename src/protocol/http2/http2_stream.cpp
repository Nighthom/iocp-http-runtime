// HTTP/2 session: frame dispatch, stream 관리, flow control, router 연동
#include "protocol/http2/http2_stream.h"
#include "protocol/http2/http2_hpack.h"

#include "buffer/byte_view.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace iocp::protocol::http2
{

// --- H2Stream implementation ---

H2Stream::H2Stream(const std::uint32_t stream_id)
    : stream_id_(stream_id)
{
}

void H2Stream::AppendHeaderFragment(
    const std::byte* data,
    const std::size_t size)
{
    header_block_.insert(
        header_block_.end(), data, data + size);
}

void H2Stream::AppendData(
    const std::byte* data,
    const std::size_t size)
{
    body_.insert(body_.end(), data, data + size);
}

void H2Stream::ConsumeRecvWindow(
    const std::uint32_t bytes) noexcept
{
    if (recv_window_ >= bytes)
    {
        recv_window_ -= bytes;
    }
}

void H2Stream::AddRecvWindow(
    const std::uint32_t bytes) noexcept
{
    recv_window_ += bytes;
}

void H2Stream::ConsumeSendWindow(
    const std::uint32_t bytes) noexcept
{
    if (send_window_ >= bytes)
    {
        send_window_ -= bytes;
    }
}

void H2Stream::AddSendWindow(
    const std::uint32_t bytes) noexcept
{
    send_window_ += bytes;
}

StreamSnapshot H2Stream::Snapshot() const
{
    return StreamSnapshot{
        stream_id_,
        state_,
        recv_window_,
        send_window_,
        header_block_.size(),
        body_.size(),
    };
}

// --- H2Session implementation ---

H2Session::H2Session(
    std::shared_ptr<http::HttpRouter> router,
    std::shared_ptr<execution::IExecutor> executor,
    ResponseSender response_sender,
    FrameSender frame_sender,
    const std::uint64_t connection_id,
    H2ConnectionConfig config)
    : router_(std::move(router))
    , executor_(std::move(executor))
    , response_sender_(std::move(response_sender))
    , frame_sender_(std::move(frame_sender))
    , connection_id_(connection_id)
    , config_(config)
{
    if (!router_)
    {
        throw std::invalid_argument(
            "HTTP/2 session requires a router");
    }
    if (!executor_)
    {
        throw std::invalid_argument(
            "HTTP/2 session requires an executor");
    }
    if (!response_sender_)
    {
        throw std::invalid_argument(
            "HTTP/2 session requires a response sender");
    }
    if (!frame_sender_)
    {
        throw std::invalid_argument(
            "HTTP/2 session requires a frame sender");
    }
}

ProtocolFeedResult H2Session::Feed(
    const buffer::ByteView bytes)
{
    FeedContext ctx;
    ctx.data = reinterpret_cast<const std::byte*>(bytes.Data());
    ctx.size = bytes.Size();

    std::lock_guard lock(mutex_);

    if (state_ == H2ConnectionState::Closed)
    {
        return ProtocolFeedResult{
            ProtocolFeedStatus::Stopped,
            0,
            0,
        };
    }

    ProtocolFeedStatus status = ProtocolFeedStatus::Ready;
    while (ctx.offset < ctx.size)
    {
        if (state_ == H2ConnectionState::ExpectPreface)
        {
            status = ConsumePreface(ctx);
            if (status != ProtocolFeedStatus::Ready)
            {
                return ProtocolFeedResult{status, ctx.dispatching, 0};
            }
            if (ctx.offset == 0)
            {
                // Not enough data for preface
                return ProtocolFeedResult{
                    ProtocolFeedStatus::Ready,
                    ctx.dispatching,
                    0,
                };
            }
        }

        if (state_ == H2ConnectionState::Connected)
        {
            status = ConsumeFrame(ctx);
            if (status != ProtocolFeedStatus::Ready)
            {
                return ProtocolFeedResult{status, ctx.dispatching, 0};
            }
        }

        if (state_ == H2ConnectionState::Closing ||
            state_ == H2ConnectionState::Closed)
        {
            break;
        }

        if (ctx.offset == 0)
        {
            break;
        }
    }

    return ProtocolFeedResult{
        ProtocolFeedStatus::Ready,
        ctx.dispatching,
        0,
    };
}

bool H2Session::IsStopped() const noexcept
{
    std::lock_guard lock(mutex_);
    return state_ == H2ConnectionState::Closed;
}

std::uint32_t H2Session::LastStreamId() const noexcept
{
    std::lock_guard lock(mutex_);
    return last_stream_id_;
}

void H2Session::SendGoaway(const ErrorCode error_code)
{
    std::lock_guard lock(mutex_);
    if (state_ != H2ConnectionState::Connected)
    {
        return;
    }
    state_ = H2ConnectionState::Closing;
    const auto goaway = FrameCodec::EncodeGoaway(
        last_stream_id_, error_code, "");
    frame_sender_(std::move(goaway));
}

void H2Session::Close()
{
    std::lock_guard lock(mutex_);
    state_ = H2ConnectionState::Closed;
    streams_.clear();
}

ProtocolFeedStatus H2Session::ConsumePreface(
    FeedContext& ctx)
{
    const auto preface = FrameCodec::kPreface;
    const std::size_t remaining = ctx.size - ctx.offset;

    if (remaining < preface.size())
    {
        // Check prefix match
        if (memcmp(ctx.data + ctx.offset,
                preface.data(), remaining) != 0)
        {
            state_ = H2ConnectionState::Closing;
            return ProtocolFeedStatus::ProtocolError;
        }
        ctx.offset = 0;
        return ProtocolFeedStatus::Ready;
    }

    if (memcmp(ctx.data + ctx.offset,
            preface.data(), preface.size()) != 0)
    {
        state_ = H2ConnectionState::Closing;
        return ProtocolFeedStatus::ProtocolError;
    }

    ctx.offset += preface.size();
    state_ = H2ConnectionState::Connected;

    // Send server preface (SETTINGS)
    SendSettings();

    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::ConsumeFrame(
    FeedContext& ctx)
{
    const auto* data = ctx.data + ctx.offset;
    const auto remaining = ctx.size - ctx.offset;

    if (remaining < FrameCodec::kHeaderSize)
    {
        ctx.offset = 0;
        return ProtocolFeedStatus::Ready;
    }

    buffer::BufferSequence seq(
        buffer::ByteView(
            const_cast<std::byte*>(ctx.data),
            ctx.size));
    FrameHeader header;
    if (!FrameCodec::DecodeHeader(seq, header))
    {
        state_ = H2ConnectionState::Closing;
        return ProtocolFeedStatus::ProtocolError;
    }

    const std::size_t frame_total =
        FrameCodec::kHeaderSize + header.length;
    if (remaining < frame_total)
    {
        ctx.offset = 0;
        return ProtocolFeedStatus::Ready;
    }

    const auto* payload =
        data + FrameCodec::kHeaderSize;

    // Validate stream_id
    if (header.stream_id != 0 &&
        (header.stream_id <= last_stream_id_ ||
         header.stream_id % 2 != 1))
    {
        // Client-initiated streams must be odd
        if (header.type == FrameType::Headers ||
            header.type == FrameType::Data)
        {
            const auto rst = FrameCodec::EncodeRstStream(
                ErrorCode::ProtocolError);
            FrameHeader rst_header;
            rst_header.type = FrameType::RstStream;
            rst_header.stream_id = header.stream_id;
            rst_header.length =
                static_cast<std::uint32_t>(rst.size());
            auto frame = FrameCodec::EncodeHeader(rst_header);
            frame.insert(frame.end(), rst.begin(), rst.end());
            frame_sender_(std::move(frame));
        }
        ctx.offset += frame_total;
        return ProtocolFeedStatus::Ready;
    }

    ProtocolFeedStatus status = ProtocolFeedStatus::Ready;

    switch (header.type)
    {
    case FrameType::Settings:
        status = HandleSettings(header, payload);
        break;
    case FrameType::Headers:
    case FrameType::Continuation:
        status = HandleHeaders(header, payload);
        break;
    case FrameType::Data:
        status = HandleData(header, payload);
        break;
    case FrameType::RstStream:
        status = HandleRstStream(header, payload);
        break;
    case FrameType::WindowUpdate:
        status = HandleWindowUpdate(header, payload);
        break;
    case FrameType::Ping:
        status = HandlePing(header, payload);
        break;
    case FrameType::Goaway:
        status = HandleGoaway(header, payload);
        break;
    case FrameType::Priority:
    case FrameType::PushPromise:
        // Accept but ignore for now
        break;
    }

    ctx.offset += frame_total;

    if (header.stream_id > last_stream_id_)
    {
        last_stream_id_ = header.stream_id;
    }

    return status;
}

ProtocolFeedStatus H2Session::HandleSettings(
    const FrameHeader& header,
    const std::byte* payload)
{
    if (header.stream_id != 0)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    if ((header.flags &
         static_cast<std::uint8_t>(FrameFlags::Ack)) != 0)
    {
        // SETTINGS ACK - nothing to do
        return ProtocolFeedStatus::Ready;
    }

    // Parse settings
    for (std::size_t i = 0; i + 6 <= header.length; i += 6)
    {
        const std::uint16_t id =
            (static_cast<std::uint16_t>(
                 static_cast<unsigned char>(payload[i])) << 8) |
            static_cast<std::uint16_t>(
                static_cast<unsigned char>(payload[i + 1]));
        const std::uint32_t value =
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(payload[i + 2])) << 24) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(payload[i + 3])) << 16) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(payload[i + 4])) << 8) |
            static_cast<std::uint32_t>(
                static_cast<unsigned char>(payload[i + 5]));

        switch (id)
        {
        case 1: // SETTINGS_HEADER_TABLE_SIZE
            remote_settings_header_table_size_ = value;
            break;
        case 3: // SETTINGS_MAX_CONCURRENT_STREAMS
            remote_settings_max_concurrent_streams_ = value;
            break;
        case 4: // SETTINGS_INITIAL_WINDOW_SIZE
            if (value > 0x7fffffff)
            {
                return ProtocolFeedStatus::ProtocolError;
            }
            remote_settings_initial_window_size_ = value;
            break;
        case 5: // SETTINGS_MAX_FRAME_SIZE
            if (value < 16384 || value > 16777215)
            {
                return ProtocolFeedStatus::ProtocolError;
            }
            config_.maximum_frame_size = value;
            break;
        case 6: // SETTINGS_MAX_HEADER_LIST_SIZE
            config_.maximum_header_list_size = value;
            break;
        }
    }

    // Send SETTINGS ACK
    FrameHeader ack_header;
    ack_header.type = FrameType::Settings;
    ack_header.flags =
        static_cast<std::uint8_t>(FrameFlags::Ack);
    ack_header.stream_id = 0;
    auto frame = FrameCodec::EncodeHeader(ack_header);
    frame_sender_(std::move(frame));

    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::HandleHeaders(
    const FrameHeader& header,
    const std::byte* payload)
{
    if (header.stream_id == 0)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    const auto end_stream =
        (header.flags &
         static_cast<std::uint8_t>(FrameFlags::EndStream)) != 0;
    const auto end_headers =
        (header.flags &
         static_cast<std::uint8_t>(FrameFlags::EndHeaders)) != 0;

    auto* stream = GetOrCreateStream(header.stream_id);
    if (!stream)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    if (stream->State() == StreamState::Closed)
    {
        const auto rst = FrameCodec::EncodeRstStream(
            ErrorCode::StreamClosed);
        FrameHeader rst_header;
        rst_header.type = FrameType::RstStream;
        rst_header.stream_id = header.stream_id;
        rst_header.length =
            static_cast<std::uint32_t>(rst.size());
        auto frame = FrameCodec::EncodeHeader(rst_header);
        frame.insert(frame.end(), rst.begin(), rst.end());
        frame_sender_(std::move(frame));
        return ProtocolFeedStatus::Ready;
    }

    // Check padding
    std::size_t payload_offset = 0;
    std::uint8_t pad_length = 0;
    if ((header.flags &
         static_cast<std::uint8_t>(FrameFlags::Padded)) != 0)
    {
        pad_length =
            static_cast<std::uint8_t>(payload[0]);
        payload_offset = 1;
    }

    // Skip priority if present
    if ((header.flags & 0x20) != 0)
    {
        payload_offset += 5;
    }

    const auto* header_data = payload + payload_offset;
    const auto header_size =
        header.length - payload_offset - pad_length;

    stream->AppendHeaderFragment(header_data, header_size);

    if (end_headers)
    {
        // Decode headers and dispatch request
        try
        {
            HpackCodec codec;
            codec.SetDynamicTableSize(
                remote_settings_header_table_size_);

            const auto decoded = codec.Decode(
                stream->HeaderBlock().data(),
                stream->HeaderBlock().size());

            // Build HttpRequest from decoded pseudo-headers + headers
            http::HttpRequest request;
            request.connection_id = connection_id_;
            bool has_method = false;
            bool has_path = false;

            for (const auto& hdr : decoded)
            {
                if (hdr.name == ":method")
                {
                    request.method =
                        http::ParseMethod(hdr.value);
                    request.method_text = hdr.value;
                    has_method = true;
                }
                else if (hdr.name == ":path")
                {
                    request.target = hdr.value;
                    const auto qpos =
                        hdr.value.find('?');
                    if (qpos != std::string::npos)
                    {
                        request.path =
                            hdr.value.substr(0, qpos);
                        request.query =
                            hdr.value.substr(qpos + 1);
                    }
                    else
                    {
                        request.path = hdr.value;
                    }
                    has_path = true;
                }
                else if (hdr.name == ":authority")
                {
                    request.headers.push_back(
                        {"host", hdr.value});
                }
                else if (hdr.name == ":scheme")
                {
                    request.headers.push_back(
                        {":scheme", hdr.value});
                }
                else if (hdr.name[0] != ':')
                {
                    request.headers.push_back(hdr);
                }
            }

            if (!has_method || !has_path)
            {
                CloseStream(header.stream_id);
                return ProtocolFeedStatus::ProtocolError;
            }

            if (end_stream)
            {
                stream->SetEndStream();
            }

            // Dispatch to application executor
            const auto exec_status = router_->Dispatch(
                std::move(request),
                executor_,
                [this, stream_id = header.stream_id](
                    http::HttpResponse response) {
                    response_sender_(stream_id,
                        std::move(response));
                },
                !end_stream); // don't close if more data expected

            if (exec_status != http::HttpDispatchStatus::Accepted)
            {
                CloseStream(header.stream_id);
                return ProtocolFeedStatus::ExecutorSaturated;
            }

            stream->SetState(end_stream
                ? StreamState::HalfClosedRemote
                : StreamState::Open);
        }
        catch (const std::exception&)
        {
            const auto rst = FrameCodec::EncodeRstStream(
                ErrorCode::CompressionError);
            FrameHeader rst_header;
            rst_header.type = FrameType::RstStream;
            rst_header.stream_id = header.stream_id;
            rst_header.length =
                static_cast<std::uint32_t>(rst.size());
            auto frame = FrameCodec::EncodeHeader(rst_header);
            frame.insert(frame.end(), rst.begin(), rst.end());
            frame_sender_(std::move(frame));
            CloseStream(header.stream_id);
        }
    }

    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::HandleData(
    const FrameHeader& header,
    const std::byte* payload)
{
    if (header.stream_id == 0)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    auto* stream = GetOrCreateStream(
        header.stream_id, false);
    if (!stream)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    std::size_t payload_offset = 0;
    if ((header.flags &
         static_cast<std::uint8_t>(FrameFlags::Padded)) != 0)
    {
        (void)static_cast<std::uint8_t>(payload[0]);
        payload_offset = 1;
    }

    const auto data_size = header.length - payload_offset -
        ((header.flags &
          static_cast<std::uint8_t>(FrameFlags::Padded)) != 0
             ? static_cast<std::uint8_t>(payload[payload_offset])
             : 0);

    // Check flow control
    if (data_size > stream->RecvWindow() ||
        data_size > connection_recv_window_)
    {
        CloseStream(header.stream_id);
        return ProtocolFeedStatus::ProtocolError;
    }

    stream->ConsumeRecvWindow(
        static_cast<std::uint32_t>(data_size));
    connection_recv_window_ -=
        static_cast<std::uint32_t>(data_size);

    stream->AppendData(
        payload + payload_offset + 1, data_size - 1);

    const auto end_stream =
        (header.flags &
         static_cast<std::uint8_t>(FrameFlags::EndStream)) != 0;

    if (end_stream)
    {
        stream->SetEndStream();
        stream->SetState(StreamState::HalfClosedRemote);
    }

    // Send WINDOW_UPDATE if needed
    if (connection_recv_window_ < 32768)
    {
        const auto wu_bytes = FrameCodec::EncodeWindowUpdate(
            config_.initial_window_size -
            connection_recv_window_);
        connection_recv_window_ =
            config_.initial_window_size;

        FrameHeader wu_header;
        wu_header.type = FrameType::WindowUpdate;
        wu_header.stream_id = 0;
        wu_header.length =
            static_cast<std::uint32_t>(wu_bytes.size());
        auto frame = FrameCodec::EncodeHeader(wu_header);
        frame.insert(
            frame.end(), wu_bytes.begin(), wu_bytes.end());
        frame_sender_(std::move(frame));
    }

    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::HandleRstStream(
    const FrameHeader& header,
    const std::byte* payload)
{
    if (header.length < 4)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    (void)(static_cast<std::uint32_t>(
             static_cast<unsigned char>(payload[0])) << 24);
    (void)(static_cast<std::uint32_t>(
             static_cast<unsigned char>(payload[1])) << 16);
    (void)(static_cast<std::uint32_t>(
             static_cast<unsigned char>(payload[2])) << 8);
    (void)(static_cast<std::uint32_t>(
            static_cast<unsigned char>(payload[3])));

    CloseStream(header.stream_id);
    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::HandleWindowUpdate(
    const FrameHeader& header,
    const std::byte* payload)
{
    if (header.length < 4)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    const std::uint32_t increment =
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(payload[0]) & 0x7f) << 24) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(payload[1])) << 16) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(payload[2])) << 8) |
        static_cast<std::uint32_t>(
            static_cast<unsigned char>(payload[3]));

    if (increment == 0)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    if (header.stream_id == 0)
    {
        connection_send_window_ += increment;
    }
    else
    {
        auto* stream = GetOrCreateStream(
            header.stream_id, false);
        if (stream)
        {
            stream->AddSendWindow(increment);
        }
    }

    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::HandlePing(
    const FrameHeader& header,
    const std::byte* payload)
{
    if (header.stream_id != 0 || header.length != 8)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    if ((header.flags &
         static_cast<std::uint8_t>(FrameFlags::Ack)) == 0)
    {
        // Respond with PING ACK
        std::uint64_t opaque = 0;
        for (int i = 0; i < 8; ++i)
        {
            opaque = (opaque << 8) |
                static_cast<std::uint64_t>(
                    static_cast<unsigned char>(payload[i]));
        }
        const auto pong = FrameCodec::EncodePing(opaque, true);
        frame_sender_(std::move(pong));
    }

    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::HandleGoaway(
    const FrameHeader& header,
    const std::byte* payload)
{
    state_ = H2ConnectionState::Closing;

    if (header.length >= 8)
    {
        (void)(static_cast<std::uint32_t>(
                 static_cast<unsigned char>(payload[0]) & 0x7f) << 24);
        (void)(static_cast<std::uint32_t>(
                 static_cast<unsigned char>(payload[1])) << 16);
        (void)(static_cast<std::uint32_t>(
                 static_cast<unsigned char>(payload[2])) << 8);
        (void)(static_cast<std::uint32_t>(
                static_cast<unsigned char>(payload[3])));
    }

    return ProtocolFeedStatus::Ready;
}

H2Stream* H2Session::GetOrCreateStream(
    const std::uint32_t stream_id,
    const bool remote_initiated)
{
    auto found = streams_.find(stream_id);
    if (found != streams_.end())
    {
        return found->second.get();
    }

    if (streams_.size() >=
        config_.maximum_concurrent_streams)
    {
        return nullptr;
    }

    auto stream = std::make_unique<H2Stream>(stream_id);
    if (remote_initiated)
    {
        stream->SetState(StreamState::Open);
    }
    auto* ptr = stream.get();
    streams_.emplace(stream_id, std::move(stream));
    return ptr;
}

void H2Session::CloseStream(const std::uint32_t stream_id)
{
    auto found = streams_.find(stream_id);
    if (found != streams_.end())
    {
        found->second->SetState(StreamState::Closed);
        streams_.erase(found);
    }
}

void H2Session::SendFrame(
    const FrameHeader& header,
    const std::vector<std::byte>& payload)
{
    auto frame = FrameCodec::EncodeHeader(header);
    if (!payload.empty())
    {
        frame.insert(
            frame.end(), payload.begin(), payload.end());
    }
    frame_sender_(std::move(frame));
}

void H2Session::SendSettings()
{
    // Send empty SETTINGS frame
    std::vector<std::pair<std::uint16_t, std::uint32_t>>
        settings;
    settings.emplace_back(2, 0); // SETTINGS_ENABLE_PUSH = 0
    settings.emplace_back(3,
        static_cast<std::uint32_t>(
            config_.maximum_concurrent_streams));
    settings.emplace_back(4,
        config_.initial_window_size);
    settings.emplace_back(5,
        config_.maximum_frame_size);
    settings.emplace_back(6,
        config_.maximum_header_list_size);

    auto frame = FrameCodec::EncodeSettings(settings);
    frame_sender_(std::move(frame));
}

} // namespace iocp::protocol::http2
