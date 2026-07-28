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

H2Stream::H2Stream(
    const std::uint32_t stream_id,
    const std::uint32_t initial_window_size)
    : stream_id_(stream_id)
    , recv_window_(initial_window_size)
    , send_window_(initial_window_size)
{
}

void H2Stream::AppendHeaderFragment(
    const std::byte* data,
    const std::size_t size)
{
    if (size == 0)
    {
        return;
    }
    header_block_.insert(
        header_block_.end(), data, data + size);
}

void H2Stream::AppendData(
    const std::byte* data,
    const std::size_t size)
{
    if (size == 0)
    {
        return;
    }
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

void H2Stream::SetRequest(http::HttpRequest request)
{
    request_ = std::move(request);
}

http::HttpRequest H2Stream::TakeRequest()
{
    if (!request_)
    {
        throw std::logic_error(
            "HTTP/2 stream request is not assembled");
    }
    http::HttpRequest request = std::move(*request_);
    request_.reset();
    return request;
}

// --- H2Session implementation ---

H2Session::H2Session(
    std::shared_ptr<http::HttpRouter> router,
    std::shared_ptr<execution::IExecutor> executor,
    FrameSender frame_sender,
    const std::uint64_t connection_id,
    H2ConnectionConfig config)
    : router_(std::move(router))
    , executor_(std::move(executor))
    , frame_sender_(std::move(frame_sender))
    , outbound_(std::make_shared<H2OutboundScheduler>(
        frame_sender_,
        config.initial_window_size,
        config.maximum_frame_size,
        HpackOptions{
            config.header_table_size,
            config.maximum_header_list_size}))
    , connection_id_(connection_id)
    , config_(config)
    , receive_buffer_(
        config.initial_receive_buffer_size,
        config.maximum_receive_buffer_size)
    , hpack_decoder_(HpackOptions{
        config.header_table_size,
        config.maximum_header_list_size})
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
    if (!frame_sender_)
    {
        throw std::invalid_argument(
            "HTTP/2 session requires a frame sender");
    }
    if (config_.initial_receive_buffer_size == 0 ||
        config_.initial_receive_buffer_size >
            config_.maximum_receive_buffer_size ||
        config_.maximum_request_body_size == 0)
    {
        throw std::invalid_argument(
            "HTTP/2 buffer limits are invalid");
    }
    connection_recv_window_ = config_.initial_window_size;
}

ProtocolFeedResult H2Session::Feed(
    const buffer::ByteView bytes)
{
    std::lock_guard lock(mutex_);

    if (state_ == H2ConnectionState::Closed)
    {
        return ProtocolFeedResult{
            ProtocolFeedStatus::Stopped,
            0,
            0,
        };
    }

    if (receive_buffer_.Append(bytes) != buffer::BufferStatus::Ready)
    {
        return FailConnection(ErrorCode::EnhanceYourCalm, 0);
    }

    std::size_t dispatching = 0;
    while (!receive_buffer_.Empty())
    {
        bool consumed = false;
        ProtocolFeedStatus status = ProtocolFeedStatus::Ready;

        if (state_ == H2ConnectionState::ExpectPreface)
        {
            status = ConsumePreface(consumed);
            if (status != ProtocolFeedStatus::Ready)
            {
                if (status == ProtocolFeedStatus::ProtocolError)
                {
                    return FailConnection(
                        ErrorCode::ProtocolError,
                        dispatching);
                }
                return ProtocolFeedResult{
                    status,
                    dispatching,
                    receive_buffer_.ReadableBytes(),
                };
            }
            if (!consumed)
            {
                return ProtocolFeedResult{
                    ProtocolFeedStatus::Ready,
                    dispatching,
                    receive_buffer_.ReadableBytes(),
                };
            }
        }

        if (state_ == H2ConnectionState::Connected)
        {
            consumed = false;
            status = ConsumeFrame(consumed, dispatching);
            if (status != ProtocolFeedStatus::Ready)
            {
                if (status == ProtocolFeedStatus::ProtocolError)
                {
                    return FailConnection(
                        ErrorCode::ProtocolError,
                        dispatching);
                }
                return ProtocolFeedResult{
                    status,
                    dispatching,
                    receive_buffer_.ReadableBytes(),
                };
            }
            if (!consumed)
            {
                break;
            }
        }

        if (state_ == H2ConnectionState::Closing ||
            state_ == H2ConnectionState::Closed)
        {
            break;
        }

    }

    return ProtocolFeedResult{
        ProtocolFeedStatus::Ready,
        dispatching,
        receive_buffer_.ReadableBytes(),
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
    outbound_->Close();
}

ProtocolFeedStatus H2Session::ConsumePreface(bool& consumed)
{
    consumed = false;
    const auto preface = FrameCodec::kPreface;
    const auto readable = receive_buffer_.ReadableSequence();
    const std::size_t available = readable.Size();
    const std::size_t compared =
        (std::min)(available, preface.size());

    for (std::size_t index = 0; index < compared; ++index)
    {
        if (static_cast<char>(readable.At(index)) != preface[index])
        {
            state_ = H2ConnectionState::Closing;
            return ProtocolFeedStatus::ProtocolError;
        }
    }

    if (available < preface.size())
    {
        return ProtocolFeedStatus::Ready;
    }

    receive_buffer_.Consume(preface.size());
    state_ = H2ConnectionState::Connected;
    consumed = true;

    // Send server preface (SETTINGS)
    SendSettings();

    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::ConsumeFrame(
    bool& consumed,
    std::size_t& dispatching)
{
    consumed = false;
    const auto readable = receive_buffer_.ReadableSequence();
    const auto remaining = readable.Size();

    if (remaining < FrameCodec::kHeaderSize)
    {
        return ProtocolFeedStatus::Ready;
    }

    FrameHeader header;
    if (!FrameCodec::DecodeHeader(readable, header))
    {
        state_ = H2ConnectionState::Closing;
        return ProtocolFeedStatus::ProtocolError;
    }

    const std::size_t frame_total =
        FrameCodec::kHeaderSize + header.length;
    if (header.length > config_.maximum_frame_size)
    {
        state_ = H2ConnectionState::Closing;
        return ProtocolFeedStatus::ProtocolError;
    }
    if (remaining < frame_total)
    {
        return ProtocolFeedStatus::Ready;
    }

    std::vector<std::byte> payload(header.length);
    if (!payload.empty())
    {
        readable.CopyTo(
            FrameCodec::kHeaderSize,
            buffer::MutableByteView(
                payload.data(), payload.size()));
    }
    receive_buffer_.Consume(frame_total);
    consumed = true;

    if (!received_initial_settings_ &&
        (header.type != FrameType::Settings ||
         (header.flags &
          static_cast<std::uint8_t>(FrameFlags::Ack)) != 0))
    {
        state_ = H2ConnectionState::Closing;
        return ProtocolFeedStatus::ProtocolError;
    }

    if (continuation_stream_id_ &&
        (header.type != FrameType::Continuation ||
         header.stream_id != *continuation_stream_id_))
    {
        state_ = H2ConnectionState::Closing;
        return ProtocolFeedStatus::ProtocolError;
    }

    ProtocolFeedStatus status = ProtocolFeedStatus::Ready;

    switch (header.type)
    {
    case FrameType::Settings:
        status = HandleSettings(header, payload.data());
        break;
    case FrameType::Headers:
    case FrameType::Continuation:
        status = HandleHeaders(
            header, payload.data(), dispatching);
        break;
    case FrameType::Data:
        status = HandleData(
            header, payload.data(), dispatching);
        break;
    case FrameType::RstStream:
        status = HandleRstStream(header, payload.data());
        break;
    case FrameType::WindowUpdate:
        status = HandleWindowUpdate(header, payload.data());
        break;
    case FrameType::Ping:
        status = HandlePing(header, payload.data());
        break;
    case FrameType::Goaway:
        status = HandleGoaway(header, payload.data());
        break;
    case FrameType::Priority:
        if (header.stream_id == 0 || header.length != 5)
        {
            status = ProtocolFeedStatus::ProtocolError;
        }
        break;
    case FrameType::PushPromise:
        status = ProtocolFeedStatus::ProtocolError;
        break;
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
        if (header.length != 0)
        {
            return ProtocolFeedStatus::ProtocolError;
        }
        return ProtocolFeedStatus::Ready;
    }

    if (header.length % 6 != 0)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    received_initial_settings_ = true;

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
            remote_settings_maximum_frame_size_ = value;
            break;
        case 6: // SETTINGS_MAX_HEADER_LIST_SIZE
            config_.maximum_header_list_size = value;
            break;
        }
    }

    if (!outbound_->ApplyPeerSettings(
        remote_settings_initial_window_size_,
        remote_settings_maximum_frame_size_,
            remote_settings_header_table_size_))
    {
        return ProtocolFeedStatus::ProtocolError;
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
    const std::byte* payload,
    std::size_t& dispatching)
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

    const bool continuation =
        header.type == FrameType::Continuation;
    H2Stream* stream = nullptr;

    if (continuation)
    {
        const auto found = streams_.find(header.stream_id);
        if (!continuation_stream_id_ ||
            *continuation_stream_id_ != header.stream_id ||
            found == streams_.end())
        {
            return ProtocolFeedStatus::ProtocolError;
        }
        stream = found->second.get();
    }
    else
    {
        if (header.stream_id % 2 != 1 ||
            header.stream_id <= last_stream_id_ ||
            streams_.find(header.stream_id) != streams_.end())
        {
            return ProtocolFeedStatus::ProtocolError;
        }
        stream = GetOrCreateStream(header.stream_id);
        if (!stream)
        {
            return ProtocolFeedStatus::ProtocolError;
        }
        last_stream_id_ = header.stream_id;
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

    std::size_t payload_offset = 0;
    std::uint8_t pad_length = 0;
    if (!continuation &&
        (header.flags &
         static_cast<std::uint8_t>(FrameFlags::Padded)) != 0)
    {
        if (header.length == 0)
        {
            return ProtocolFeedStatus::ProtocolError;
        }
        pad_length =
            static_cast<std::uint8_t>(payload[0]);
        payload_offset = 1;
    }

    if (!continuation &&
        (header.flags &
         static_cast<std::uint8_t>(FrameFlags::Priority)) != 0)
    {
        payload_offset += 5;
    }

    if (payload_offset > header.length ||
        pad_length > header.length - payload_offset)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    const auto* header_data = payload + payload_offset;
    const auto header_size =
        header.length - payload_offset - pad_length;

    if (header_size != 0)
    {
        stream->AppendHeaderFragment(header_data, header_size);
    }

    if (!continuation && end_stream)
    {
        stream->SetEndStream();
    }

    if (!end_headers)
    {
        continuation_stream_id_ = header.stream_id;
        return ProtocolFeedStatus::Ready;
    }

    continuation_stream_id_.reset();
    if (stream->HeadersComplete())
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    try
    {
        const auto decoded = hpack_decoder_.Decode(
            stream->HeaderBlock().data(),
            stream->HeaderBlock().size());

        http::HttpRequest request;
        request.connection_id = connection_id_;
        request.request_id = header.stream_id;
        bool has_method = false;
        bool has_path = false;
        bool saw_regular_header = false;

        for (const auto& hdr : decoded)
        {
            if (hdr.name.empty())
            {
                return ProtocolFeedStatus::ProtocolError;
            }
            const bool pseudo = hdr.name.front() == ':';
            if (pseudo && saw_regular_header)
            {
                return ProtocolFeedStatus::ProtocolError;
            }
            saw_regular_header = saw_regular_header || !pseudo;

            if (hdr.name == ":method")
            {
                if (has_method)
                {
                    return ProtocolFeedStatus::ProtocolError;
                }
                request.method = http::ParseMethod(hdr.value);
                request.method_text = hdr.value;
                has_method = true;
            }
            else if (hdr.name == ":path")
            {
                if (has_path)
                {
                    return ProtocolFeedStatus::ProtocolError;
                }
                request.target = hdr.value;
                const auto query = hdr.value.find('?');
                if (query != std::string::npos)
                {
                    request.path = hdr.value.substr(0, query);
                    request.query = hdr.value.substr(query + 1);
                }
                else
                {
                    request.path = hdr.value;
                }
                has_path = true;
            }
            else if (hdr.name == ":authority")
            {
                request.headers.push_back({"host", hdr.value});
            }
            else if (hdr.name == ":scheme")
            {
                // scheme은 common request에 아직 별도 field가 없다.
            }
            else if (pseudo)
            {
                return ProtocolFeedStatus::ProtocolError;
            }
            else
            {
                for (const char character : hdr.name)
                {
                    if (character >= 'A' && character <= 'Z')
                    {
                        return ProtocolFeedStatus::ProtocolError;
                    }
                }
                request.headers.push_back(hdr);
            }
        }

        if (!has_method || !has_path ||
            request.method == http::HttpMethod::Unsupported)
        {
            CloseStream(header.stream_id);
            return ProtocolFeedStatus::ProtocolError;
        }

        stream->SetRequest(std::move(request));
        stream->SetHeadersComplete();
        stream->SetState(stream->EndStream()
            ? StreamState::HalfClosedRemote
            : StreamState::Open);

        if (stream->EndStream())
        {
            return DispatchRequest(
                header.stream_id, dispatching);
        }
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

    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::HandleData(
    const FrameHeader& header,
    const std::byte* payload,
    std::size_t& dispatching)
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
    if (!stream->HeadersComplete() ||
        stream->EndStream() ||
        stream->State() == StreamState::Closed)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    std::size_t payload_offset = 0;
    std::uint8_t pad_length = 0;
    if ((header.flags &
         static_cast<std::uint8_t>(FrameFlags::Padded)) != 0)
    {
        if (header.length == 0)
        {
            return ProtocolFeedStatus::ProtocolError;
        }
        pad_length = static_cast<std::uint8_t>(payload[0]);
        payload_offset = 1;
    }

    if (pad_length > header.length - payload_offset)
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    const auto data_size =
        header.length - payload_offset - pad_length;
    const auto flow_size = header.length;

    // Check flow control
    if (flow_size > stream->RecvWindow() ||
        flow_size > connection_recv_window_)
    {
        CloseStream(header.stream_id);
        return ProtocolFeedStatus::ProtocolError;
    }
    if (data_size > config_.maximum_request_body_size ||
        stream->Body().size() >
            config_.maximum_request_body_size - data_size)
    {
        CloseStream(header.stream_id);
        return ProtocolFeedStatus::BufferLimitExceeded;
    }

    stream->ConsumeRecvWindow(
        static_cast<std::uint32_t>(flow_size));
    connection_recv_window_ -=
        static_cast<std::uint32_t>(flow_size);

    if (data_size != 0)
    {
        stream->AppendData(
            payload + payload_offset, data_size);
    }

    const auto end_stream =
        (header.flags &
         static_cast<std::uint8_t>(FrameFlags::EndStream)) != 0;

    if (end_stream)
    {
        stream->SetEndStream();
        stream->SetState(StreamState::HalfClosedRemote);
    }

    // Send WINDOW_UPDATE if needed
    if (connection_recv_window_ <
        config_.initial_window_size / 2)
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

    if (stream->RecvWindow() <
        config_.initial_window_size / 2)
    {
        const auto increment =
            config_.initial_window_size - stream->RecvWindow();
        const auto bytes =
            FrameCodec::EncodeWindowUpdate(increment);
        stream->AddRecvWindow(increment);

        FrameHeader update;
        update.type = FrameType::WindowUpdate;
        update.stream_id = header.stream_id;
        update.length =
            static_cast<std::uint32_t>(bytes.size());
        SendFrame(update, bytes);
    }

    if (end_stream)
    {
        return DispatchRequest(
            header.stream_id, dispatching);
    }

    return ProtocolFeedStatus::Ready;
}

ProtocolFeedStatus H2Session::HandleRstStream(
    const FrameHeader& header,
    const std::byte* payload)
{
    if (header.stream_id == 0 || header.length != 4)
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
    if (header.length != 4)
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
        if (!outbound_->UpdateConnectionWindow(increment))
        {
            return ProtocolFeedStatus::ProtocolError;
        }
    }
    else
    {
        if (!outbound_->UpdateStreamWindow(
                header.stream_id, increment))
        {
            if (header.stream_id > last_stream_id_)
            {
                return ProtocolFeedStatus::ProtocolError;
            }
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
    if (header.stream_id != 0 || header.length < 8)
    {
        return ProtocolFeedStatus::ProtocolError;
    }
    state_ = H2ConnectionState::Closing;

    (void)(static_cast<std::uint32_t>(
             static_cast<unsigned char>(payload[0]) & 0x7f) << 24);

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

    if (!remote_initiated)
    {
        return nullptr;
    }

    if (streams_.size() >=
        config_.maximum_concurrent_streams)
    {
        return nullptr;
    }

    auto stream = std::make_unique<H2Stream>(
        stream_id, config_.initial_window_size);
    if (remote_initiated)
    {
        stream->SetState(StreamState::Open);
    }
    auto* ptr = stream.get();
    streams_.emplace(stream_id, std::move(stream));
    outbound_->OpenStream(stream_id);
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
    outbound_->CloseStream(stream_id);
}

ProtocolFeedStatus H2Session::DispatchRequest(
    const std::uint32_t stream_id,
    std::size_t& dispatching)
{
    const auto found = streams_.find(stream_id);
    if (found == streams_.end())
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    H2Stream& stream = *found->second;
    if (!stream.HeadersComplete() || !stream.EndStream() ||
        !stream.HasRequest() || stream.Dispatched())
    {
        return ProtocolFeedStatus::ProtocolError;
    }

    http::HttpRequest request = stream.TakeRequest();
    request.body = stream.Body();
    stream.SetDispatched();

    auto outbound = outbound_;
    const auto dispatch_status = router_->Dispatch(
        std::move(request),
        executor_,
        [outbound = std::move(outbound),
         stream_id](http::HttpResponse response) mutable {
            outbound->SubmitResponse(
                stream_id, std::move(response));
        },
        false);

    if (dispatch_status == http::HttpDispatchStatus::Accepted)
    {
        ++dispatching;
        streams_.erase(found);
        return ProtocolFeedStatus::Ready;
    }

    CloseStream(stream_id);
    return dispatch_status == http::HttpDispatchStatus::ExecutorSaturated
        ? ProtocolFeedStatus::ExecutorSaturated
        : ProtocolFeedStatus::ExecutorStopped;
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

ProtocolFeedResult H2Session::FailConnection(
    const ErrorCode error_code,
    const std::size_t dispatching)
{
    if (state_ == H2ConnectionState::Connected ||
        state_ == H2ConnectionState::Closing)
    {
        frame_sender_(FrameCodec::EncodeGoaway(
            last_stream_id_, error_code, ""));
    }
    state_ = H2ConnectionState::Closed;
    receive_buffer_.Clear();
    streams_.clear();
    continuation_stream_id_.reset();
    outbound_->Close();
    return ProtocolFeedResult{
        ProtocolFeedStatus::CloseRequired,
        dispatching,
        0,
    };
}

} // namespace iocp::protocol::http2
