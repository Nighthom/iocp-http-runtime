// HTTP/1.1 chunked transfer encoding parser 테스트
#include "buffer/byte_view.h"
#include "buffer/ring_receive_buffer.h"
#include "protocol/http/http_message.h"
#include "protocol/http/http_request_parser.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

using iocp::buffer::ByteView;
using iocp::buffer::RingReceiveBuffer;
using namespace iocp::protocol::http;

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

ByteView ViewOf(const std::string_view text)
{
    return ByteView(
        reinterpret_cast<const std::byte*>(text.data()),
        text.size());
}

HttpParseResult ParseWhole(const std::string_view wire)
{
    RingReceiveBuffer buffer(8, wire.size() + 16);
    Check(
        buffer.Append(ViewOf(wire)) ==
            iocp::buffer::BufferStatus::Ready,
        "append failed");
    HttpRequestParser parser;
    return parser.Parse(buffer.ReadableSequence());
}

void TestSimpleChunked()
{
    const std::string wire =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    const auto result = ParseWhole(wire);
    Check(
        result.status == HttpParseStatus::Complete &&
            result.request.method == HttpMethod::Post &&
            result.request.path == "/upload",
        "simple chunked request did not complete");
    Check(
        StringFromBytes(result.request.body) == "hello",
        "chunked body was not decoded correctly");
}

void TestMultipleChunks()
{
    const std::string wire =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "3\r\n"
        "foo\r\n"
        "4\r\n"
        "bar \r\n"
        "0\r\n"
        "\r\n";

    const auto result = ParseWhole(wire);
    Check(
        result.status == HttpParseStatus::Complete,
        "multi-chunk request did not complete");
    Check(
        StringFromBytes(result.request.body) == "foobar ",
        "multi-chunk body was not decoded correctly");
}

void TestChunkedAtSplits()
{
    const std::string wire =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "A\r\n"
        "hello worl\r\n"
        "1\r\n"
        "d\r\n"
        "0\r\n"
        "\r\n";

    for (std::size_t split = 1; split < wire.size(); ++split)
    {
        RingReceiveBuffer buffer(8, 4096);
        HttpRequestParser parser;

        Check(
            buffer.Append(ViewOf(wire.substr(0, split))) ==
                iocp::buffer::BufferStatus::Ready,
            "split append failed");
        const auto first = parser.Parse(buffer.ReadableSequence());
        if (first.status == HttpParseStatus::Incomplete ||
            first.status == HttpParseStatus::HeadersComplete)
        {
            // continue parsing
            Check(
                buffer.Append(ViewOf(wire.substr(split))) ==
                    iocp::buffer::BufferStatus::Ready,
                "second append failed");
            const auto second =
                parser.Parse(buffer.ReadableSequence());
            Check(
                second.status == HttpParseStatus::Complete,
                "split chunked did not complete");
            Check(
                StringFromBytes(
                    second.request.body) == "hello world",
                "split chunked body was decoded incorrectly");
        }
    }
}

void TestChunkedWithExtensions()
{
    const std::string wire =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5;myext=123\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    const auto result = ParseWhole(wire);
    Check(
        result.status == HttpParseStatus::Complete &&
            StringFromBytes(
                result.request.body) == "hello",
        "chunked with extension was not decoded");
}

void TestEmptyChunked()
{
    const std::string wire =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n"
        "\r\n";

    const auto result = ParseWhole(wire);
    Check(
        result.status == HttpParseStatus::Complete &&
            result.request.body.empty(),
        "empty chunked body was not handled correctly");
}

void TestExpectContinue()
{
    const std::string wire =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Expect: 100-continue\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    RingReceiveBuffer buffer(4, 4096);
    HttpRequestParser parser;

    // Feed headers only first
    const auto headers_wire = wire.substr(0, wire.find("\r\n\r\n") + 4);
    Check(
        buffer.Append(ViewOf(headers_wire)) ==
            iocp::buffer::BufferStatus::Ready,
        "headers append failed");

    const auto headers_result =
        parser.Parse(buffer.ReadableSequence());
    Check(
        headers_result.status == HttpParseStatus::HeadersComplete &&
            headers_result.expect_continue,
        "100-continue was not detected in headers");

    // Feed the rest (body)
    Check(
        buffer.Append(ViewOf(wire.substr(headers_wire.size()))) ==
            iocp::buffer::BufferStatus::Ready,
        "body append failed");

    const auto body_result =
        parser.Parse(buffer.ReadableSequence());
    Check(
        body_result.status == HttpParseStatus::Complete &&
            StringFromBytes(
                body_result.request.body) == "hello",
        "100-continue body was not parsed correctly");
}

void TestContentLengthAndChunkedConflict()
{
    const auto error = ParseWhole(
        "POST / HTTP/1.1\r\n"
        "Host: a\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n");
    Check(
        error.status == HttpParseStatus::Error,
        "conflicting Transfer-Encoding and Content-Length "
        "was not rejected");
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
        "simple chunked encoding",
        TestSimpleChunked);
    failures += !RunTest(
        "multiple chunks",
        TestMultipleChunks);
    failures += !RunTest(
        "chunked at every split boundary",
        TestChunkedAtSplits);
    failures += !RunTest(
        "chunked with extensions",
        TestChunkedWithExtensions);
    failures += !RunTest(
        "empty chunked body",
        TestEmptyChunked);
    failures += !RunTest(
        "expect 100-continue",
        TestExpectContinue);
    failures += !RunTest(
        "content-length and chunked conflict",
        TestContentLengthAndChunkedConflict);
    return failures == 0 ? 0 : 1;
}
