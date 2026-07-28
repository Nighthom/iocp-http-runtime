#include "protocol/http2/http2_outbound.h"

#include "protocol/http2/http2_frames.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace iocp::protocol::http2
{

namespace
{

constexpr std::int64_t kMaximumWindow = 0x7fffffff;

std::string Lowercase(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool IsConnectionSpecific(const std::string& name)
{
    return name == "connection" ||
           name == "keep-alive" ||
           name == "proxy-connection" ||
           name == "transfer-encoding" ||
           name == "upgrade";
}

} // namespace

H2OutboundScheduler::H2OutboundScheduler(
    FrameSender frame_sender,
    const std::uint32_t initial_window_size,
    const std::uint32_t maximum_frame_size,
    HpackOptions hpack_options)
    : frame_sender_(std::move(frame_sender))
    , initial_window_size_(initial_window_size)
    , maximum_frame_size_(maximum_frame_size)
    , maximum_encoder_table_size_(
        hpack_options.maximum_dynamic_table_size)
    , encoder_(hpack_options)
{
    if (!frame_sender_ ||
        initial_window_size_ > kMaximumWindow ||
        maximum_frame_size_ < 16384 ||
        maximum_frame_size_ > 16777215)
    {
        throw std::invalid_argument(
            "HTTP/2 outbound scheduler options are invalid");
    }
}

void H2OutboundScheduler::OpenStream(const std::uint32_t stream_id)
{
    std::lock_guard lock(mutex_);
    if (closed_)
    {
        return;
    }
    streams_.try_emplace(
        stream_id,
        StreamOutput{
            static_cast<std::int64_t>(initial_window_size_),
            {},
            0,
            false});
}

void H2OutboundScheduler::CloseStream(
    const std::uint32_t stream_id)
{
    std::lock_guard lock(mutex_);
    streams_.erase(stream_id);
}

void H2OutboundScheduler::Close()
{
    std::lock_guard lock(mutex_);
    closed_ = true;
    streams_.clear();
}

bool H2OutboundScheduler::SubmitResponse(
    const std::uint32_t stream_id,
    http::HttpResponse response)
{
    std::vector<std::vector<std::byte>> frames;
    {
        std::lock_guard lock(mutex_);
        const auto found = streams_.find(stream_id);
        if (closed_ || found == streams_.end() ||
            found->second.response_submitted)
        {
            return false;
        }

        StreamOutput& stream = found->second;
        stream.response_submitted = true;
        const bool body_empty = response.body.empty();
        stream.body = std::move(response.body);
        EncodeHeaderBlockLocked(
            stream_id,
            std::move(response),
            body_empty,
            frames);

        if (stream.body.empty())
        {
            streams_.erase(found);
        }
        else if (FlushStreamLocked(stream_id, stream, frames))
        {
            streams_.erase(found);
        }
    }
    SendFrames(std::move(frames));
    return true;
}

bool H2OutboundScheduler::ApplyPeerSettings(
    const std::uint32_t initial_window_size,
    const std::uint32_t maximum_frame_size,
    const std::size_t header_table_size)
{
    if (initial_window_size > kMaximumWindow ||
        maximum_frame_size < 16384 ||
        maximum_frame_size > 16777215)
    {
        return false;
    }

    std::vector<std::vector<std::byte>> frames;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
        {
            return false;
        }

        const std::int64_t delta =
            static_cast<std::int64_t>(initial_window_size) -
            static_cast<std::int64_t>(initial_window_size_);
        for (auto& [stream_id, stream] : streams_)
        {
            (void)stream_id;
            if (stream.window + delta > kMaximumWindow)
            {
                return false;
            }
        }
        for (auto& [stream_id, stream] : streams_)
        {
            (void)stream_id;
            stream.window += delta;
        }

        initial_window_size_ = initial_window_size;
        maximum_frame_size_ = maximum_frame_size;
        encoder_.SetDynamicTableSize((std::min)(
            header_table_size,
            maximum_encoder_table_size_));
        FlushAllLocked(frames);
    }
    SendFrames(std::move(frames));
    return true;
}

bool H2OutboundScheduler::UpdateConnectionWindow(
    const std::uint32_t increment)
{
    std::vector<std::vector<std::byte>> frames;
    {
        std::lock_guard lock(mutex_);
        if (closed_ || increment == 0 ||
            connection_window_ + increment > kMaximumWindow)
        {
            return false;
        }
        connection_window_ += increment;
        FlushAllLocked(frames);
    }
    SendFrames(std::move(frames));
    return true;
}

bool H2OutboundScheduler::UpdateStreamWindow(
    const std::uint32_t stream_id,
    const std::uint32_t increment)
{
    std::vector<std::vector<std::byte>> frames;
    {
        std::lock_guard lock(mutex_);
        const auto found = streams_.find(stream_id);
        if (closed_ || increment == 0 ||
            found == streams_.end() ||
            found->second.window + increment > kMaximumWindow)
        {
            return false;
        }

        found->second.window += increment;
        if (FlushStreamLocked(
                stream_id, found->second, frames))
        {
            streams_.erase(found);
        }
    }
    SendFrames(std::move(frames));
    return true;
}

void H2OutboundScheduler::EncodeHeaderBlockLocked(
    const std::uint32_t stream_id,
    http::HttpResponse response,
    const bool end_stream,
    std::vector<std::vector<std::byte>>& frames)
{
    std::vector<http::HttpHeader> headers;
    headers.push_back({
        ":status", std::to_string(response.status_code)});
    for (auto& header : response.headers)
    {
        header.name = Lowercase(std::move(header.name));
        if (!header.name.empty() &&
            header.name.front() != ':' &&
            !IsConnectionSpecific(header.name))
        {
            headers.push_back(std::move(header));
        }
    }

    const auto block = encoder_.Encode(headers);
    std::size_t offset = 0;
    bool first = true;
    do
    {
        const std::size_t bytes = (std::min)(
            static_cast<std::size_t>(maximum_frame_size_),
            block.size() - offset);
        const bool last = offset + bytes == block.size();

        FrameHeader header;
        header.type = first
            ? FrameType::Headers
            : FrameType::Continuation;
        header.stream_id = stream_id;
        header.length = static_cast<std::uint32_t>(bytes);
        if (last)
        {
            header.flags |= static_cast<std::uint8_t>(
                FrameFlags::EndHeaders);
            if (end_stream)
            {
                header.flags |= static_cast<std::uint8_t>(
                    FrameFlags::EndStream);
            }
        }

        auto frame = FrameCodec::EncodeHeader(header);
        frame.insert(
            frame.end(),
            block.begin() + static_cast<std::ptrdiff_t>(offset),
            block.begin() +
                static_cast<std::ptrdiff_t>(offset + bytes));
        frames.push_back(std::move(frame));
        offset += bytes;
        first = false;
    } while (offset < block.size());
}

bool H2OutboundScheduler::FlushStreamLocked(
    const std::uint32_t stream_id,
    StreamOutput& stream,
    std::vector<std::vector<std::byte>>& frames)
{
    while (stream.offset < stream.body.size() &&
           stream.window > 0 &&
           connection_window_ > 0)
    {
        const std::size_t remaining =
            stream.body.size() - stream.offset;
        const auto bytes = static_cast<std::size_t>((std::min)({
            static_cast<std::int64_t>(remaining),
            stream.window,
            connection_window_,
            static_cast<std::int64_t>(maximum_frame_size_)}));

        FrameHeader header;
        header.type = FrameType::Data;
        header.stream_id = stream_id;
        header.length = static_cast<std::uint32_t>(bytes);
        if (stream.offset + bytes == stream.body.size())
        {
            header.flags = static_cast<std::uint8_t>(
                FrameFlags::EndStream);
        }

        auto frame = FrameCodec::EncodeHeader(header);
        frame.insert(
            frame.end(),
            stream.body.begin() +
                static_cast<std::ptrdiff_t>(stream.offset),
            stream.body.begin() +
                static_cast<std::ptrdiff_t>(
                    stream.offset + bytes));
        frames.push_back(std::move(frame));

        stream.offset += bytes;
        stream.window -= static_cast<std::int64_t>(bytes);
        connection_window_ -= static_cast<std::int64_t>(bytes);
    }

    return stream.offset == stream.body.size();
}

void H2OutboundScheduler::FlushAllLocked(
    std::vector<std::vector<std::byte>>& frames)
{
    for (auto iterator = streams_.begin();
         iterator != streams_.end();)
    {
        if (iterator->second.response_submitted &&
            FlushStreamLocked(
                iterator->first, iterator->second, frames))
        {
            iterator = streams_.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
}

void H2OutboundScheduler::SendFrames(
    std::vector<std::vector<std::byte>> frames)
{
    for (auto& frame : frames)
    {
        frame_sender_(std::move(frame));
    }
}

} // namespace iocp::protocol::http2
