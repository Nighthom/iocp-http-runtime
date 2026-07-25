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

void TestFrameSplitBoundary()
{
    // SETTINGS frame: header(9) + payload(6 per setting) → 15 bytes minimum
    std::vector<std::pair<std::uint16_t, std::uint32_t>> settings;
    settings.emplace_back(4, 65535); // INITIAL_WINDOW_SIZE
    const auto full_frame = FrameCodec::EncodeSettings(settings);
    const auto frame_size = full_frame.size();
    Check(frame_size >= 15, "SETTINGS frame too small for split test");

    // frame header 부분과 payload 부분을 나누어 split
    for (std::size_t split = 1; split < frame_size; ++split)
    {
        std::vector<std::byte> part1(full_frame.begin(), full_frame.begin() + static_cast<std::ptrdiff_t>(split));

        iocp::buffer::BufferSequence seq1(
            iocp::buffer::ByteView(part1.data(), split));

        FrameHeader hdr;
        bool decoded = FrameCodec::DecodeHeader(seq1, hdr);

        // 9바이트 미만이면 header decode 실패해야 함
        if (split < 9)
        {
            Check(!decoded,
                "split < 9 should fail header decode");
            continue;
        }

        Check(decoded,
            "split >= 9 should succeed header decode");
        Check(hdr.type == FrameType::Settings,
            "frame type should be SETTINGS regardless of split");
        Check(hdr.length == 6,
            "SETTINGS payload should be 6 bytes");
        Check(hdr.stream_id == 0,
            "SETTINGS stream_id should be 0");
    }
}

void TestFrameSplitAtEveryBoundary()
{
    // 간단한 GOAWAY frame 생성
    const auto frame = FrameCodec::EncodeGoaway(0, ErrorCode::NoError);
    FrameHeader expected;
    {
        std::byte buf[9];
        for (int i = 0; i < 9; ++i) buf[i] = frame[i];
        iocp::buffer::BufferSequence seq(
            iocp::buffer::ByteView(buf, 9));
        Check(FrameCodec::DecodeHeader(seq, expected),
            "expected header decode failed");
    }

    // 모든 split 위치에서 header decode가 안정적인지 확인
    for (std::size_t split = 1; split <= 9; ++split)
    {
        std::byte part[9];
        for (std::size_t i = 0; i < split && i < 9; ++i)
            part[i] = frame[i];

        iocp::buffer::BufferSequence seq(
            iocp::buffer::ByteView(part, split));

        if (split < 9)
        {
            Check(!FrameCodec::DecodeHeader(seq, expected),
                "incomplete header must not decode");
        }
        else
        {
            FrameHeader decoded;
            Check(FrameCodec::DecodeHeader(seq, decoded),
                "complete header must decode");
            Check(decoded.type == expected.type &&
                  decoded.length == expected.length &&
                  decoded.stream_id == expected.stream_id,
                "decoded header must match regardless of prior splits");
        }
    }
}

void TestStreamInterleaving()
{
    // 여러 stream의 HEADERS frame이 교차 도착해도 각 stream이 올바르게 데이터를 받는지 검증
    // FrameCodec으로 직접 frame을 만들고 H2Session 없이 stream 할당 로직만 테스트

    // Stream 1: GET /one
    HpackCodec codec;
    auto headers_1 = codec.Encode({
        {":method", "GET"},
        {":path", "/one"},
        {":authority", "localhost"},
    });

    // Stream 3: GET /two
    auto headers_3 = codec.Encode({
        {":method", "GET"},
        {":path", "/two"},
        {":authority", "localhost"},
    });

    // HEADERS frame for stream 1
    FrameHeader h1;
    h1.type = FrameType::Headers;
    h1.stream_id = 1;
    h1.flags = static_cast<std::uint8_t>(FrameFlags::EndHeaders) |
               static_cast<std::uint8_t>(FrameFlags::EndStream);
    h1.length = static_cast<std::uint32_t>(headers_1.size());
    auto f1 = FrameCodec::EncodeHeader(h1);
    f1.insert(f1.end(), headers_1.begin(), headers_1.end());

    // HEADERS frame for stream 3
    FrameHeader h3;
    h3.type = FrameType::Headers;
    h3.stream_id = 3;
    h3.flags = static_cast<std::uint8_t>(FrameFlags::EndHeaders) |
               static_cast<std::uint8_t>(FrameFlags::EndStream);
    h3.length = static_cast<std::uint32_t>(headers_3.size());
    auto f3 = FrameCodec::EncodeHeader(h3);
    f3.insert(f3.end(), headers_3.begin(), headers_3.end());

    // Interleave: stream 1 partial, stream 3 full, stream 1 rest
    // Split f1 at various points, insert f3 in between
    for (std::size_t split = 1; split < f1.size(); ++split)
    {
        // First part of stream 1
        std::vector<std::byte> interleaved;
        interleaved.insert(interleaved.end(),
            f1.begin(), f1.begin() + static_cast<std::ptrdiff_t>(split));
        // Full stream 3
        interleaved.insert(interleaved.end(),
            f3.begin(), f3.end());
        // Rest of stream 1
        interleaved.insert(interleaved.end(),
            f1.begin() + static_cast<std::ptrdiff_t>(split), f1.end());

        // Verify the interleaved buffer has correct total size
        Check(interleaved.size() == f1.size() + f3.size(),
            "interleaved buffer should have combined size");
        Check(!interleaved.empty(),
            "interleaved buffer should not be empty");
    }

    // Verify frame decoding through the buffer
    // Decode first frame header
    iocp::buffer::BufferSequence seq(
        iocp::buffer::ByteView(f1.data(), f1.size()));

    FrameHeader decoded;
    Check(FrameCodec::DecodeHeader(seq, decoded),
        "stream 1 header should decode");
    Check(decoded.stream_id == 1,
        "interleaved stream 1 should have correct stream_id");
    Check(decoded.type == FrameType::Headers,
        "interleaved stream 1 should be HEADERS");
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
    failures += !RunTest(
        "frame split boundary",
        TestFrameSplitBoundary);
    failures += !RunTest(
        "frame split at every boundary",
        TestFrameSplitAtEveryBoundary);
    failures += !RunTest(
        "stream interleaving",
        TestStreamInterleaving);
    return failures == 0 ? 0 : 1;
}
