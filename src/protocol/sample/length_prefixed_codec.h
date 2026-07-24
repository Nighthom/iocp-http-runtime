#pragma once

#include "buffer/buffer_sequence.h"
#include "buffer/byte_view.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iocp::protocol
{

struct SampleMessage final
{
    std::uint16_t id{};
    std::vector<std::byte> payload;
};

enum class FrameDecodeStatus : std::uint8_t
{
    Incomplete,
    Complete,
    Error,
};

enum class FrameDecodeError : std::uint8_t
{
    None,
    BodyTooSmall,
    PayloadTooLarge,
};

struct FrameDecodeResult final
{
    FrameDecodeStatus status{FrameDecodeStatus::Incomplete};
    FrameDecodeError error{FrameDecodeError::None};
    std::size_t consumed_bytes{};
    SampleMessage message;
};

/// @brief sample big-endian length prefix를 해석하는 stateless frame decoder다.
///
/// wire format:
/// `[4-byte body length][2-byte message id][payload]`
/// body length는 message id와 payload를 합한 뒤쪽 byte 수다.
class LengthPrefixedFrameDecoder final
{
public:
    explicit LengthPrefixedFrameDecoder(
        std::size_t maximum_payload_bytes);

    FrameDecodeResult Decode(buffer::ByteView input) const;
    FrameDecodeResult Decode(buffer::BufferSequence input) const;
    std::size_t MaximumPayloadBytes() const noexcept;

private:
    std::size_t maximum_payload_bytes_{};
};

/// @brief `SampleMessage`를 length-prefixed wire bytes로 변환한다.
class LengthPrefixedFrameEncoder final
{
public:
    explicit LengthPrefixedFrameEncoder(
        std::size_t maximum_payload_bytes);

    std::vector<std::byte> Encode(
        std::uint16_t message_id,
        buffer::ByteView payload) const;
    std::size_t MaximumPayloadBytes() const noexcept;

private:
    std::size_t maximum_payload_bytes_{};
};

} // namespace iocp::protocol
