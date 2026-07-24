#include "protocol/sample/length_prefixed_codec.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace iocp::protocol
{

namespace
{

constexpr std::size_t kLengthPrefixBytes = 4;
constexpr std::size_t kMessageIdBytes = 2;
constexpr std::size_t kFrameOverheadBytes =
    kLengthPrefixBytes + kMessageIdBytes;

void ValidateMaximumPayload(const std::size_t maximum_payload_bytes)
{
    constexpr std::size_t kMaximumEncodablePayload =
        static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max()) -
        kFrameOverheadBytes;
    if (maximum_payload_bytes > kMaximumEncodablePayload)
    {
        throw std::invalid_argument(
            "sample protocol payload 상한이 wire length 범위를 넘습니다");
    }
}

std::uint32_t ReadBigEndian32(
    const buffer::BufferSequence input,
    const std::size_t offset)
{
    return
        (std::to_integer<std::uint32_t>(input.At(offset)) << 24) |
        (std::to_integer<std::uint32_t>(input.At(offset + 1)) << 16) |
        (std::to_integer<std::uint32_t>(input.At(offset + 2)) << 8) |
        std::to_integer<std::uint32_t>(input.At(offset + 3));
}

std::uint16_t ReadBigEndian16(
    const buffer::BufferSequence input,
    const std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(input.At(offset)) << 8) |
        std::to_integer<std::uint16_t>(input.At(offset + 1)));
}

void WriteBigEndian32(
    std::byte* output,
    const std::uint32_t value) noexcept
{
    output[0] = static_cast<std::byte>((value >> 24) & 0xffu);
    output[1] = static_cast<std::byte>((value >> 16) & 0xffu);
    output[2] = static_cast<std::byte>((value >> 8) & 0xffu);
    output[3] = static_cast<std::byte>(value & 0xffu);
}

void WriteBigEndian16(
    std::byte* output,
    const std::uint16_t value) noexcept
{
    output[0] = static_cast<std::byte>((value >> 8) & 0xffu);
    output[1] = static_cast<std::byte>(value & 0xffu);
}

} // namespace

LengthPrefixedFrameDecoder::LengthPrefixedFrameDecoder(
    const std::size_t maximum_payload_bytes)
    : maximum_payload_bytes_(maximum_payload_bytes)
{
    ValidateMaximumPayload(maximum_payload_bytes_);
}

FrameDecodeResult LengthPrefixedFrameDecoder::Decode(
    const buffer::ByteView input) const
{
    return Decode(buffer::BufferSequence(input));
}

FrameDecodeResult LengthPrefixedFrameDecoder::Decode(
    const buffer::BufferSequence input) const
{
    if (input.Size() < kLengthPrefixBytes)
    {
        return {};
    }

    const std::uint32_t body_bytes = ReadBigEndian32(input, 0);
    if (body_bytes < kMessageIdBytes)
    {
        return FrameDecodeResult{
            FrameDecodeStatus::Error,
            FrameDecodeError::BodyTooSmall,
            0,
            {},
        };
    }

    const std::size_t payload_bytes =
        static_cast<std::size_t>(body_bytes) - kMessageIdBytes;
    if (payload_bytes > maximum_payload_bytes_)
    {
        return FrameDecodeResult{
            FrameDecodeStatus::Error,
            FrameDecodeError::PayloadTooLarge,
            0,
            {},
        };
    }

    const std::size_t total_bytes =
        kLengthPrefixBytes + static_cast<std::size_t>(body_bytes);
    if (input.Size() < total_bytes)
    {
        return {};
    }

    SampleMessage message;
    message.id = ReadBigEndian16(input, kLengthPrefixBytes);
    message.payload.resize(payload_bytes);
    input.CopyTo(
        kFrameOverheadBytes,
        buffer::MutableByteView(
            message.payload.data(),
            message.payload.size()));

    return FrameDecodeResult{
        FrameDecodeStatus::Complete,
        FrameDecodeError::None,
        total_bytes,
        std::move(message),
    };
}

std::size_t LengthPrefixedFrameDecoder::MaximumPayloadBytes() const noexcept
{
    return maximum_payload_bytes_;
}

LengthPrefixedFrameEncoder::LengthPrefixedFrameEncoder(
    const std::size_t maximum_payload_bytes)
    : maximum_payload_bytes_(maximum_payload_bytes)
{
    ValidateMaximumPayload(maximum_payload_bytes_);
}

std::vector<std::byte> LengthPrefixedFrameEncoder::Encode(
    const std::uint16_t message_id,
    const buffer::ByteView payload) const
{
    if (payload.Size() > maximum_payload_bytes_)
    {
        throw std::length_error(
            "sample protocol payload가 설정된 상한을 넘습니다");
    }

    const std::size_t body_bytes =
        kMessageIdBytes + payload.Size();
    std::vector<std::byte> output(
        kLengthPrefixBytes + body_bytes);
    WriteBigEndian32(
        output.data(),
        static_cast<std::uint32_t>(body_bytes));
    WriteBigEndian16(
        output.data() + kLengthPrefixBytes,
        message_id);
    for (std::size_t index = 0; index < payload.Size(); ++index)
    {
        output[kFrameOverheadBytes + index] = payload[index];
    }
    return output;
}

std::size_t LengthPrefixedFrameEncoder::MaximumPayloadBytes() const noexcept
{
    return maximum_payload_bytes_;
}

} // namespace iocp::protocol
