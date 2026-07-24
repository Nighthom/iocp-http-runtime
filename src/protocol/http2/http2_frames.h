/// @file http2_frames.h
/// @brief HTTP/2 frame type, flag, error code, frame header, frame encoder/decoder 정의

#pragma once

#include "buffer/buffer_sequence.h"
#include "buffer/byte_view.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace iocp::protocol::http2
{

enum class FrameType : std::uint8_t
{
    Data          = 0x00,
    Headers       = 0x01,
    Priority      = 0x02,
    RstStream     = 0x03,
    Settings      = 0x04,
    PushPromise   = 0x05,
    Ping          = 0x06,
    Goaway        = 0x07,
    WindowUpdate  = 0x08,
    Continuation  = 0x09,
};

enum class FrameFlags : std::uint8_t
{
    None          = 0x00,
    EndStream     = 0x01,
    Ack           = 0x01,
    EndHeaders    = 0x04,
    Padded        = 0x08,
    Priority      = 0x20,
};

enum class ErrorCode : std::uint32_t
{
    NoError             = 0x00,
    ProtocolError       = 0x01,
    InternalError       = 0x02,
    FlowControlError    = 0x03,
    SettingsTimeout     = 0x04,
    StreamClosed        = 0x05,
    FrameSizeError      = 0x06,
    RefusedStream       = 0x07,
    Cancel              = 0x08,
    CompressionError    = 0x09,
    ConnectError        = 0x0a,
    EnhanceYourCalm     = 0x0b,
    InadequateSecurity  = 0x0c,
    Http11Required      = 0x0d,
};

struct FrameHeader final
{
    std::uint32_t length{};
    FrameType type{FrameType::Data};
    std::uint8_t flags{};
    std::uint32_t stream_id{};
};

/// @brief HTTP/2 frame wire format 인코딩/디코딩을 담당한다.
class FrameCodec final
{
public:
    static constexpr std::size_t kHeaderSize = 9;
    static constexpr std::size_t kMaxFrameSize = 16384;
    static constexpr std::string_view kPreface =
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

    /// @brief 9바이트 frame header를 파싱한다.
    /// @returns 파싱 성공 시 true. input이 9바이트 미만이면 false.
    static bool DecodeHeader(
        buffer::BufferSequence input,
        FrameHeader& header) noexcept;

    /// @brief frame header를 9바이트 wire format으로 인코딩한다.
    static std::vector<std::byte> EncodeHeader(
        const FrameHeader& header);

    /// @brief SETTINGS frame payload를 생성한다.
    static std::vector<std::byte> EncodeSettings(
        const std::vector<std::pair<std::uint16_t, std::uint32_t>>&
            settings);

    /// @brief GOAWAY frame payload를 생성한다.
    static std::vector<std::byte> EncodeGoaway(
        std::uint32_t last_stream_id,
        ErrorCode error_code,
        const std::string& debug_data = {});

    /// @brief RST_STREAM frame payload를 생성한다.
    static std::vector<std::byte> EncodeRstStream(
        ErrorCode error_code);

    /// @brief PING frame payload를 생성한다.
    static std::vector<std::byte> EncodePing(
        std::uint64_t opaque_data,
        bool ack = false);

    /// @brief WINDOW_UPDATE frame payload를 생성한다.
    static std::vector<std::byte> EncodeWindowUpdate(
        std::uint32_t window_size_increment);

    /// @brief frames을 순서대로 이어 하나의 연속된 byte sequence로 만든다.
    static std::vector<std::byte> Flatten(
        const std::vector<std::vector<std::byte>>& frames);
};

const char* FrameTypeName(FrameType type) noexcept;
const char* ErrorCodeName(ErrorCode code) noexcept;

} // namespace iocp::protocol::http2
