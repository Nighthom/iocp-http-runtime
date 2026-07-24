// HTTP/2 frame header encoding/decoding 및 payload 생성 구현
#include "protocol/http2/http2_frames.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace iocp::protocol::http2
{

namespace
{

std::byte FromChar(const char value) noexcept
{
    return static_cast<std::byte>(value);
}

std::byte FromUint8(const std::uint8_t value) noexcept
{
    return static_cast<std::byte>(value);
}

std::uint32_t MakeUint32(
    const std::byte b0,
    const std::byte b1,
    const std::byte b2,
    const std::byte b3) noexcept
{
    return (static_cast<std::uint32_t>(b0) << 24) |
           (static_cast<std::uint32_t>(b1) << 16) |
           (static_cast<std::uint32_t>(b2) << 8) |
           static_cast<std::uint32_t>(b3);
}

} // namespace

bool FrameCodec::DecodeHeader(
    const buffer::BufferSequence input,
    FrameHeader& header) noexcept
{
    if (input.Size() < kHeaderSize)
    {
        return false;
    }

    const std::uint32_t length = MakeUint32(
        static_cast<std::byte>(0),
        input.At(0),
        input.At(1),
        input.At(2));

    if (length > kMaxFrameSize)
    {
        return false;
    }

    header.length = length;
    header.type = static_cast<FrameType>(
        static_cast<unsigned char>(input.At(3)));
    header.flags = static_cast<std::uint8_t>(input.At(4));
    header.stream_id =
        MakeUint32(
            static_cast<std::byte>(0),
            static_cast<std::byte>(0),
            static_cast<std::byte>(
                static_cast<unsigned char>(input.At(5)) &
                0x7f),
            input.At(6)) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(input.At(7))) << 8) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(input.At(8))) << 0);

    // mask reserved bit
    header.stream_id &= 0x7fffffff;

    return true;
}

std::vector<std::byte> FrameCodec::EncodeHeader(
    const FrameHeader& header)
{
    std::vector<std::byte> result;
    result.reserve(kHeaderSize);

    result.push_back(
        FromUint8(static_cast<std::uint8_t>(
            (header.length >> 16) & 0xff)));
    result.push_back(
        FromUint8(static_cast<std::uint8_t>(
            (header.length >> 8) & 0xff)));
    result.push_back(
        FromUint8(static_cast<std::uint8_t>(
            header.length & 0xff)));
    result.push_back(
        FromUint8(static_cast<std::uint8_t>(header.type)));
    result.push_back(FromUint8(header.flags));
    result.push_back(FromUint8(
        static_cast<std::uint8_t>(
            (header.stream_id >> 24) & 0x7f)));
    result.push_back(
        FromUint8(static_cast<std::uint8_t>(
            (header.stream_id >> 16) & 0xff)));
    result.push_back(
        FromUint8(static_cast<std::uint8_t>(
            (header.stream_id >> 8) & 0xff)));
    result.push_back(
        FromUint8(static_cast<std::uint8_t>(
            header.stream_id & 0xff)));

    return result;
}

std::vector<std::byte> FrameCodec::EncodeSettings(
    const std::vector<std::pair<std::uint16_t, std::uint32_t>>&
        settings)
{
    std::vector<std::byte> payload;
    payload.reserve(settings.size() * 6);

    for (const auto& [id, value] : settings)
    {
        payload.push_back(FromUint8(
            static_cast<std::uint8_t>((id >> 8) & 0xff)));
        payload.push_back(FromUint8(
            static_cast<std::uint8_t>(id & 0xff)));
        payload.push_back(FromUint8(
            static_cast<std::uint8_t>((value >> 24) & 0xff)));
        payload.push_back(FromUint8(
            static_cast<std::uint8_t>((value >> 16) & 0xff)));
        payload.push_back(FromUint8(
            static_cast<std::uint8_t>((value >> 8) & 0xff)));
        payload.push_back(FromUint8(
            static_cast<std::uint8_t>(value & 0xff)));
    }

    FrameHeader header;
    header.type = FrameType::Settings;
    header.stream_id = 0;

    auto frame = EncodeHeader(header);
    frame.reserve(kHeaderSize + payload.size());
    frame.insert(frame.end(), payload.begin(), payload.end());

    return frame;
}

std::vector<std::byte> FrameCodec::EncodeGoaway(
    const std::uint32_t last_stream_id,
    const ErrorCode error_code,
    const std::string& debug_data)
{
    std::vector<std::byte> payload;
    payload.reserve(8 + debug_data.size());

    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((last_stream_id >> 24) & 0x7f)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((last_stream_id >> 16) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((last_stream_id >> 8) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>(last_stream_id & 0xff)));

    const auto code = static_cast<std::uint32_t>(error_code);
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((code >> 24) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((code >> 16) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((code >> 8) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>(code & 0xff)));

    for (const char character : debug_data)
    {
        payload.push_back(FromChar(character));
    }

    FrameHeader header;
    header.type = FrameType::Goaway;
    header.stream_id = 0;
    header.length = static_cast<std::uint32_t>(payload.size());

    auto frame = EncodeHeader(header);
    frame.insert(frame.end(), payload.begin(), payload.end());

    return frame;
}

std::vector<std::byte> FrameCodec::EncodeRstStream(
    const ErrorCode error_code)
{
    const auto code = static_cast<std::uint32_t>(error_code);
    std::vector<std::byte> payload;
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((code >> 24) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((code >> 16) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((code >> 8) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>(code & 0xff)));

    return payload;
}

std::vector<std::byte> FrameCodec::EncodePing(
    const std::uint64_t opaque_data,
    const bool ack)
{
    std::vector<std::byte> payload;
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((opaque_data >> 56) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((opaque_data >> 48) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((opaque_data >> 40) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((opaque_data >> 32) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((opaque_data >> 24) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((opaque_data >> 16) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((opaque_data >> 8) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>(opaque_data & 0xff)));

    FrameHeader header;
    header.type = FrameType::Ping;
    header.stream_id = 0;
    header.flags = ack
        ? static_cast<std::uint8_t>(FrameFlags::Ack)
        : static_cast<std::uint8_t>(0);
    header.length = static_cast<std::uint32_t>(payload.size());

    auto frame = EncodeHeader(header);
    frame.insert(frame.end(), payload.begin(), payload.end());

    return frame;
}

std::vector<std::byte> FrameCodec::EncodeWindowUpdate(
    const std::uint32_t window_size_increment)
{
    const auto masked = window_size_increment & 0x7fffffff;
    std::vector<std::byte> payload;
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((masked >> 24) & 0x7f)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((masked >> 16) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>((masked >> 8) & 0xff)));
    payload.push_back(FromUint8(
        static_cast<std::uint8_t>(masked & 0xff)));

    return payload;
}

std::vector<std::byte> FrameCodec::Flatten(
    const std::vector<std::vector<std::byte>>& frames)
{
    std::vector<std::byte> result;
    std::size_t total = 0;
    for (const auto& frame : frames)
    {
        total += frame.size();
    }
    result.reserve(total);
    for (const auto& frame : frames)
    {
        result.insert(result.end(), frame.begin(), frame.end());
    }
    return result;
}

const char* FrameTypeName(const FrameType type) noexcept
{
    switch (type)
    {
    case FrameType::Data:          return "DATA";
    case FrameType::Headers:       return "HEADERS";
    case FrameType::Priority:      return "PRIORITY";
    case FrameType::RstStream:     return "RST_STREAM";
    case FrameType::Settings:      return "SETTINGS";
    case FrameType::PushPromise:   return "PUSH_PROMISE";
    case FrameType::Ping:          return "PING";
    case FrameType::Goaway:        return "GOAWAY";
    case FrameType::WindowUpdate:  return "WINDOW_UPDATE";
    case FrameType::Continuation:  return "CONTINUATION";
    }
    return "UNKNOWN";
}

const char* ErrorCodeName(const ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::NoError:            return "NO_ERROR";
    case ErrorCode::ProtocolError:      return "PROTOCOL_ERROR";
    case ErrorCode::InternalError:      return "INTERNAL_ERROR";
    case ErrorCode::FlowControlError:   return "FLOW_CONTROL_ERROR";
    case ErrorCode::SettingsTimeout:    return "SETTINGS_TIMEOUT";
    case ErrorCode::StreamClosed:       return "STREAM_CLOSED";
    case ErrorCode::FrameSizeError:     return "FRAME_SIZE_ERROR";
    case ErrorCode::RefusedStream:      return "REFUSED_STREAM";
    case ErrorCode::Cancel:             return "CANCEL";
    case ErrorCode::CompressionError:   return "COMPRESSION_ERROR";
    case ErrorCode::ConnectError:       return "CONNECT_ERROR";
    case ErrorCode::EnhanceYourCalm:    return "ENHANCE_YOUR_CALM";
    case ErrorCode::InadequateSecurity: return "INADEQUATE_SECURITY";
    case ErrorCode::Http11Required:     return "HTTP_1_1_REQUIRED";
    }
    return "UNKNOWN";
}

} // namespace iocp::protocol::http2
