// HTTP/2 frame, HPACK 코드 테스트
#include "protocol/http2/http2_frames.h"
#include "protocol/http2/http2_hpack.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using namespace iocp::protocol::http2;

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestFrameHeaderEncodeDecode()
{
    FrameHeader original;
    original.length = 1024;
    original.type = FrameType::Headers;
    original.flags = static_cast<std::uint8_t>(
        FrameFlags::EndHeaders) |
        static_cast<std::uint8_t>(FrameFlags::EndStream);
    original.stream_id = 1;

    const auto encoded = FrameCodec::EncodeHeader(original);
    Check(
        encoded.size() == 9,
        "frame header must be 9 bytes");

    const auto first_byte =
        static_cast<unsigned char>(encoded[0]);
    Check(first_byte == 0, "first byte of 3-byte length must be 0 for values < 65536");

    // Reconstruct as buffer sequence
    std::byte buffer[9];
    for (std::size_t i = 0; i < 9; ++i)
    {
        buffer[i] = encoded[i];
    }

    iocp::buffer::BufferSequence seq(
        iocp::buffer::ByteView(buffer, 9));
    FrameHeader decoded;
    Check(
        FrameCodec::DecodeHeader(seq, decoded),
        "frame header decode failed");
    Check(
        decoded.length == 1024 &&
            decoded.type == FrameType::Headers &&
            decoded.stream_id == 1,
        "frame header round-trip mismatch");
}

void TestSettingsEncoding()
{
    std::vector<std::pair<std::uint16_t, std::uint32_t>>
        settings;
    settings.emplace_back(3, 100);  // MAX_CONCURRENT_STREAMS
    settings.emplace_back(4, 65535); // INITIAL_WINDOW_SIZE

    const auto frame = FrameCodec::EncodeSettings(settings);
    Check(
        !frame.empty(),
        "SETTINGS frame must not be empty");

    // Check header
    Check(
        static_cast<unsigned char>(frame[3]) == 0x04,
        "SETTINGS type byte must be 0x04");
}

void TestGoawayEncoding()
{
    const auto frame = FrameCodec::EncodeGoaway(
        0, ErrorCode::NoError);
    Check(!frame.empty(), "GOAWAY frame must not be empty");
}

void TestPingEncoding()
{
    const auto ping = FrameCodec::EncodePing(0x12345678ABCDEFULL, false);
    Check(!ping.empty(), "PING frame must not be empty");

    const auto pong = FrameCodec::EncodePing(0x12345678ABCDEFULL, true);
    Check(!pong.empty(), "PING ACK frame must not be empty");
}

void TestRstStreamEncoding()
{
    const auto rst = FrameCodec::EncodeRstStream(
        ErrorCode::Cancel);
    Check(!rst.empty(), "RST_STREAM payload must not be empty");
}

void TestWindowUpdateEncoding()
{
    const auto wu = FrameCodec::EncodeWindowUpdate(65535);
    Check(!wu.empty(), "WINDOW_UPDATE payload must not be empty");
}

void TestHpackStaticTable()
{
    HpackCodec codec;

    // Encode a header that should use the static table
    std::vector<iocp::protocol::http::HttpHeader> headers;
    headers.push_back({":method", "GET"});
    headers.push_back({":path", "/"});
    headers.push_back({"host", "localhost"});

    const auto encoded = codec.Encode(headers);
    Check(!encoded.empty(),
        "HPACK encoded output must not be empty");

    // Decode round-trip
    const auto decoded = codec.Decode(
        encoded.data(), encoded.size());
    Check(decoded.size() == headers.size(),
        "HPACK decode count mismatch");

    for (std::size_t i = 0; i < headers.size(); ++i)
    {
        Check(
            decoded[i].name == headers[i].name &&
                decoded[i].value == headers[i].value,
            "HPACK round-trip mismatch");
    }
}

void TestHpackCustomHeaders()
{
    HpackCodec codec;

    std::vector<iocp::protocol::http::HttpHeader> headers;
    headers.push_back({"x-custom-header", "test-value"});
    headers.push_back({"authorization", "Bearer token123"});

    const auto encoded = codec.Encode(headers);
    Check(!encoded.empty(),
        "HPACK custom header encode must not be empty");

    const auto decoded = codec.Decode(
        encoded.data(), encoded.size());
    Check(decoded.size() == headers.size(),
        "HPACK custom header decode count mismatch");

    for (std::size_t i = 0; i < headers.size(); ++i)
    {
        Check(
            decoded[i].name == headers[i].name &&
                decoded[i].value == headers[i].value,
            "HPACK custom header round-trip mismatch");
    }
}

void TestHpackStringLiteral()
{
    HpackCodec codec;

    std::vector<iocp::protocol::http::HttpHeader> headers;
    headers.push_back(
        {"x-data", "abcdefghijklmnopqrstuvwxyz0123456789"});

    const auto encoded = codec.Encode(headers);
    const auto decoded = codec.Decode(
        encoded.data(), encoded.size());
    Check(decoded.size() == 1,
        "HPACK string literal decode count mismatch");
    Check(
        decoded[0].name == "x-data" &&
            decoded[0].value == "abcdefghijklmnopqrstuvwxyz0123456789",
        "HPACK string literal round-trip mismatch");
}

template <typename Test>
bool RunTest(const char* name, Test test)
{
    try
    {
        test();
        std::cout << "[PASS] " << name << '\n';
        return true;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << name << ": "
                  << exception.what() << '\n';
        return false;
    }
}

} // namespace

int main()
{
    int failures = 0;
    failures += !RunTest(
        "frame header encode/decode",
        TestFrameHeaderEncodeDecode);
    failures += !RunTest(
        "SETTINGS frame encoding",
        TestSettingsEncoding);
    failures += !RunTest(
        "GOAWAY frame encoding",
        TestGoawayEncoding);
    failures += !RunTest(
        "PING frame encoding",
        TestPingEncoding);
    failures += !RunTest(
        "RST_STREAM encoding",
        TestRstStreamEncoding);
    failures += !RunTest(
        "WINDOW_UPDATE encoding",
        TestWindowUpdateEncoding);
    failures += !RunTest(
        "HPACK static table",
        TestHpackStaticTable);
    failures += !RunTest(
        "HPACK custom headers",
        TestHpackCustomHeaders);
    failures += !RunTest(
        "HPACK string literal",
        TestHpackStringLiteral);
    return failures == 0 ? 0 : 1;
}
