// HTTP/2 frame, HPACK 코드 테스트
#include "execution/manual_executor.h"
#include "protocol/preface_protocol_bootstrap.h"
#include "protocol/http/http_router.h"
#include "protocol/http2/http2_frames.h"
#include "protocol/http2/http2_hpack.h"
#include "protocol/http2/http2_stream.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace iocp::protocol::http2;

class RecordingSession final :
    public iocp::protocol::IProtocolSession
{
public:
    explicit RecordingSession(std::vector<std::byte>& received)
        : received_(received)
    {
    }

    iocp::protocol::ProtocolFeedResult Feed(
        const iocp::buffer::ByteView bytes) override
    {
        received_.insert(
            received_.end(), bytes.begin(), bytes.end());
        return {
            iocp::protocol::ProtocolFeedStatus::Ready,
            0,
            0,
        };
    }

private:
    std::vector<std::byte>& received_;
};

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

std::vector<std::byte> Bytes(const std::string_view text)
{
    const auto* begin = reinterpret_cast<const std::byte*>(
        text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

void TestProtocolBootstrapEverySplit()
{
    const std::string preface(FrameCodec::kPreface);
    const auto tail = Bytes("tail");

    for (std::size_t split = 1; split < preface.size(); ++split)
    {
        std::vector<std::byte> matched;
        std::vector<std::byte> fallback;
        iocp::protocol::PrefaceProtocolBootstrap bootstrap(
            preface,
            [&matched] {
                return std::make_shared<RecordingSession>(matched);
            },
            [&fallback] {
                return std::make_shared<RecordingSession>(fallback);
            });

        const auto input = Bytes(preface);
        const auto first = bootstrap.Feed(
            iocp::buffer::ByteView(input.data(), split));
        Check(
            first.status ==
                iocp::protocol::ProtocolFeedStatus::Ready,
            "partial preface must remain ready");
        Check(
            first.buffered_bytes == split,
            "bootstrap must own partial preface");
        Check(
            matched.empty() && fallback.empty(),
            "partial preface must not select a protocol");

        std::vector<std::byte> rest(
            input.begin() + static_cast<std::ptrdiff_t>(split),
            input.end());
        rest.insert(rest.end(), tail.begin(), tail.end());
        const auto second = bootstrap.Feed(
            iocp::buffer::ByteView(rest.data(), rest.size()));
        Check(
            second.status ==
                iocp::protocol::ProtocolFeedStatus::Ready,
            "completed preface must select matching protocol");

        auto expected = input;
        expected.insert(expected.end(), tail.begin(), tail.end());
        Check(
            matched == expected,
            "matching protocol must receive preface and tail");
        Check(
            fallback.empty(),
            "fallback protocol must not receive matching preface");
    }
}

void TestProtocolBootstrapFallbackPreservesBytes()
{
    std::vector<std::byte> matched;
    std::vector<std::byte> fallback;
    iocp::protocol::PrefaceProtocolBootstrap bootstrap(
        std::string(FrameCodec::kPreface),
        [&matched] {
            return std::make_shared<RecordingSession>(matched);
        },
        [&fallback] {
            return std::make_shared<RecordingSession>(fallback);
        });

    const auto request = Bytes(
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    bootstrap.Feed(
        iocp::buffer::ByteView(request.data(), 2));
    bootstrap.Feed(
        iocp::buffer::ByteView(
            request.data() + 2, request.size() - 2));

    Check(matched.empty(),
        "HTTP/1.1 input must not select matching protocol");
    Check(fallback == request,
        "fallback protocol must receive every detection byte");
}

std::vector<std::byte> MakeHeadersFrame(
    const std::uint32_t stream_id,
    const std::string& path,
    const bool end_stream,
    const std::string& method = "GET")
{
    HpackCodec codec;
    auto block = codec.Encode({
        {":method", method},
        {":path", path},
        {":authority", "localhost"},
    });

    FrameHeader header;
    header.type = FrameType::Headers;
    header.stream_id = stream_id;
    header.flags =
        static_cast<std::uint8_t>(FrameFlags::EndHeaders);
    if (end_stream)
    {
        header.flags |=
            static_cast<std::uint8_t>(FrameFlags::EndStream);
    }
    header.length = static_cast<std::uint32_t>(block.size());

    auto frame = FrameCodec::EncodeHeader(header);
    frame.insert(frame.end(), block.begin(), block.end());
    return frame;
}

std::vector<std::byte> MakeDataFrame(
    const std::uint32_t stream_id,
    const std::vector<std::byte>& body,
    const bool end_stream,
    const std::size_t padding = 0)
{
    std::vector<std::byte> payload;
    std::uint8_t flags = 0;
    if (padding != 0)
    {
        if (padding > 255)
        {
            throw std::invalid_argument("test padding too large");
        }
        flags |= static_cast<std::uint8_t>(FrameFlags::Padded);
        payload.push_back(static_cast<std::byte>(padding));
    }
    payload.insert(payload.end(), body.begin(), body.end());
    payload.insert(payload.end(), padding, std::byte{0});

    if (end_stream)
    {
        flags |= static_cast<std::uint8_t>(FrameFlags::EndStream);
    }

    FrameHeader header;
    header.type = FrameType::Data;
    header.stream_id = stream_id;
    header.flags = flags;
    header.length = static_cast<std::uint32_t>(payload.size());
    auto frame = FrameCodec::EncodeHeader(header);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

void FeedClientPreamble(H2Session& session)
{
    const auto preface = Bytes(FrameCodec::kPreface);
    const auto result = session.Feed(
        iocp::buffer::ByteView(
            preface.data(), preface.size()));
    Check(
        result.status ==
            iocp::protocol::ProtocolFeedStatus::Ready,
        "client preface must be accepted");

    const auto settings = FrameCodec::EncodeSettings({});
    const auto settings_result = session.Feed(
        iocp::buffer::ByteView(
            settings.data(), settings.size()));
    Check(
        settings_result.status ==
            iocp::protocol::ProtocolFeedStatus::Ready,
        "initial client SETTINGS must be accepted");
}

void TestH2SessionEverySplitBoundary()
{
    auto input = Bytes(FrameCodec::kPreface);
    const auto settings = FrameCodec::EncodeSettings({});
    const auto headers = MakeHeadersFrame(1, "/split", true);
    input.insert(input.end(), settings.begin(), settings.end());
    input.insert(input.end(), headers.begin(), headers.end());

    for (std::size_t split = 1; split < input.size(); ++split)
    {
        auto router =
            std::make_shared<iocp::protocol::http::HttpRouter>();
        Check(
            router->Register(
                iocp::protocol::http::HttpMethod::Get,
                "/split",
                [](const iocp::protocol::http::HttpRequest&) {
                    return iocp::protocol::http::MakeTextResponse(
                        200, "ok");
                }),
            "test route registration failed");

        auto executor =
            std::make_shared<iocp::execution::ManualExecutor>(8);
        std::vector<std::uint32_t> responses;
        std::vector<std::vector<std::byte>> frames;
        auto session = std::make_shared<H2Session>(
            router,
            executor,
            [&responses](
                const std::uint32_t stream_id,
                iocp::protocol::http::HttpResponse) {
                responses.push_back(stream_id);
            },
            [&frames](std::vector<std::byte> frame) {
                frames.push_back(std::move(frame));
            },
            1);

        const auto first = session->Feed(
            iocp::buffer::ByteView(input.data(), split));
        Check(
            first.status ==
                iocp::protocol::ProtocolFeedStatus::Ready,
            "first split feed must remain ready");
        const auto second = session->Feed(
            iocp::buffer::ByteView(
                input.data() + split,
                input.size() - split));
        Check(
            second.status ==
                iocp::protocol::ProtocolFeedStatus::Ready,
            "second split feed must complete request");

        executor->RunReady();
        Check(
            responses.size() == 1 && responses.front() == 1,
            "split input must dispatch stream 1 exactly once");
        Check(
            frames.size() >= 2,
            "session must send server SETTINGS and client SETTINGS ACK");
    }
}

void TestH2PostWaitsForEndStreamAndPreservesBody()
{
    auto router =
        std::make_shared<iocp::protocol::http::HttpRouter>();
    std::vector<std::byte> received_body;
    Check(
        router->Register(
            iocp::protocol::http::HttpMethod::Post,
            "/body",
            [&received_body](
                const iocp::protocol::http::HttpRequest& request) {
                received_body = request.body;
                return iocp::protocol::http::MakeTextResponse(
                    200, "ok");
            }),
        "POST route registration failed");

    auto executor =
        std::make_shared<iocp::execution::ManualExecutor>(8);
    std::vector<std::uint32_t> responses;
    std::vector<std::vector<std::byte>> frames;
    H2Session session(
        router,
        executor,
        [&responses](
            const std::uint32_t stream_id,
            iocp::protocol::http::HttpResponse) {
            responses.push_back(stream_id);
        },
        [&frames](std::vector<std::byte> frame) {
            frames.push_back(std::move(frame));
        },
        1);
    FeedClientPreamble(session);

    const auto headers =
        MakeHeadersFrame(1, "/body", false, "POST");
    const auto header_result = session.Feed(
        iocp::buffer::ByteView(
            headers.data(), headers.size()));
    Check(
        header_result.status ==
            iocp::protocol::ProtocolFeedStatus::Ready,
        "POST headers must be accepted");
    Check(
        executor->Snapshot().pending_tasks == 0,
        "handler must wait until END_STREAM");

    const auto body = Bytes("x");
    const auto data = MakeDataFrame(1, body, true);
    const auto data_result = session.Feed(
        iocp::buffer::ByteView(data.data(), data.size()));
    Check(
        data_result.status ==
            iocp::protocol::ProtocolFeedStatus::Ready,
        "POST DATA must be accepted on existing stream");
    Check(
        data_result.messages_dispatched == 1,
        "END_STREAM DATA must dispatch exactly one request");

    executor->RunReady();
    Check(
        received_body == body,
        "one-byte DATA body must not lose its first byte");
    Check(
        responses.size() == 1 && responses.front() == 1,
        "POST response must preserve stream id");
}

void TestH2PaddedDataPreservesBody()
{
    auto router =
        std::make_shared<iocp::protocol::http::HttpRouter>();
    std::vector<std::byte> received_body;
    router->Register(
        iocp::protocol::http::HttpMethod::Post,
        "/padded",
        [&received_body](
            const iocp::protocol::http::HttpRequest& request) {
            received_body = request.body;
            return iocp::protocol::http::MakeTextResponse(
                200, "ok");
        });

    auto executor =
        std::make_shared<iocp::execution::ManualExecutor>(8);
    H2Session session(
        router,
        executor,
        [](std::uint32_t,
           iocp::protocol::http::HttpResponse) {},
        [](std::vector<std::byte>) {},
        1);
    FeedClientPreamble(session);

    const auto headers =
        MakeHeadersFrame(1, "/padded", false, "POST");
    session.Feed(
        iocp::buffer::ByteView(
            headers.data(), headers.size()));

    const auto body = Bytes("padded-body");
    const auto data = MakeDataFrame(1, body, true, 5);
    const auto result = session.Feed(
        iocp::buffer::ByteView(data.data(), data.size()));
    Check(
        result.status ==
            iocp::protocol::ProtocolFeedStatus::Ready,
        "valid padded DATA must be accepted");
    executor->RunReady();
    Check(
        received_body == body,
        "padding bytes must not enter request body");
}

void TestH2SessionStreamInterleaving()
{
    auto router =
        std::make_shared<iocp::protocol::http::HttpRouter>();
    std::vector<std::string> paths;
    router->Register(
        iocp::protocol::http::HttpMethod::Post,
        "/one",
        [&paths](const iocp::protocol::http::HttpRequest& request) {
            paths.push_back(
                request.path + ":" +
                iocp::protocol::http::StringFromBytes(request.body));
            return iocp::protocol::http::MakeTextResponse(200, "one");
        });
    router->Register(
        iocp::protocol::http::HttpMethod::Get,
        "/two",
        [&paths](const iocp::protocol::http::HttpRequest& request) {
            paths.push_back(request.path);
            return iocp::protocol::http::MakeTextResponse(200, "two");
        });

    auto executor =
        std::make_shared<iocp::execution::ManualExecutor>(8);
    std::vector<std::uint32_t> responses;
    H2Session session(
        router,
        executor,
        [&responses](
            const std::uint32_t stream_id,
            iocp::protocol::http::HttpResponse) {
            responses.push_back(stream_id);
        },
        [](std::vector<std::byte>) {},
        1);
    FeedClientPreamble(session);

    const auto stream1 =
        MakeHeadersFrame(1, "/one", false, "POST");
    const auto stream3 =
        MakeHeadersFrame(3, "/two", true);
    const auto stream1_data =
        MakeDataFrame(1, Bytes("body"), true);

    Check(
        session.Feed(iocp::buffer::ByteView(
            stream1.data(), stream1.size())).status ==
            iocp::protocol::ProtocolFeedStatus::Ready,
        "stream 1 HEADERS must be accepted");
    Check(
        session.Feed(iocp::buffer::ByteView(
            stream3.data(), stream3.size())).status ==
            iocp::protocol::ProtocolFeedStatus::Ready,
        "stream 3 HEADERS must be accepted while stream 1 is open");
    Check(
        session.Feed(iocp::buffer::ByteView(
            stream1_data.data(), stream1_data.size())).status ==
            iocp::protocol::ProtocolFeedStatus::Ready,
        "stream 1 DATA must remain valid after stream 3 opens");

    executor->RunReady();
    Check(
        responses.size() == 2 &&
            responses[0] == 3 &&
            responses[1] == 1,
        "interleaved streams must preserve response stream ids");
    Check(
        paths.size() == 2 &&
            paths[0] == "/two" &&
            paths[1] == "/one:body",
        "each stream must assemble its own request");
}

void TestH2RejectsMalformedPaddedData()
{
    auto router =
        std::make_shared<iocp::protocol::http::HttpRouter>();
    router->Register(
        iocp::protocol::http::HttpMethod::Post,
        "/bad",
        [](const iocp::protocol::http::HttpRequest&) {
            return iocp::protocol::http::MakeTextResponse(200, "bad");
        });
    auto executor =
        std::make_shared<iocp::execution::ManualExecutor>(8);
    H2Session session(
        router,
        executor,
        [](std::uint32_t,
           iocp::protocol::http::HttpResponse) {},
        [](std::vector<std::byte>) {},
        1);
    FeedClientPreamble(session);

    const auto headers =
        MakeHeadersFrame(1, "/bad", false, "POST");
    session.Feed(
        iocp::buffer::ByteView(headers.data(), headers.size()));

    FrameHeader data_header;
    data_header.type = FrameType::Data;
    data_header.stream_id = 1;
    data_header.flags =
        static_cast<std::uint8_t>(FrameFlags::Padded) |
        static_cast<std::uint8_t>(FrameFlags::EndStream);
    data_header.length = 1;
    auto malformed = FrameCodec::EncodeHeader(data_header);
    malformed.push_back(static_cast<std::byte>(5));

    const auto result = session.Feed(
        iocp::buffer::ByteView(
            malformed.data(), malformed.size()));
    Check(
        result.status ==
            iocp::protocol::ProtocolFeedStatus::ProtocolError,
        "padding beyond payload must be rejected");
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
        "protocol bootstrap every split",
        TestProtocolBootstrapEverySplit);
    failures += !RunTest(
        "protocol bootstrap fallback preserves bytes",
        TestProtocolBootstrapFallbackPreservesBytes);
    failures += !RunTest(
        "H2 session every split boundary",
        TestH2SessionEverySplitBoundary);
    failures += !RunTest(
        "H2 POST waits for END_STREAM and preserves body",
        TestH2PostWaitsForEndStreamAndPreservesBody);
    failures += !RunTest(
        "H2 padded DATA preserves body",
        TestH2PaddedDataPreservesBody);
    failures += !RunTest(
        "H2 session stream interleaving",
        TestH2SessionStreamInterleaving);
    failures += !RunTest(
        "H2 rejects malformed padded DATA",
        TestH2RejectsMalformedPaddedData);
    return failures == 0 ? 0 : 1;
}
