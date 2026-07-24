#include "buffer/byte_view.h"
#include "buffer/ring_receive_buffer.h"
#include "execution/manual_executor.h"
#include "protocol/http/http_message.h"
#include "protocol/http/http_request_parser.h"
#include "protocol/http/http_response_encoder.h"
#include "protocol/http/http_router.h"
#include "protocol/http/http_session.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

using iocp::buffer::ByteView;
using iocp::buffer::RingReceiveBuffer;
using iocp::execution::ManualExecutor;
using iocp::execution::StopMode;
using iocp::execution::SubmitStatus;
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

void Append(RingReceiveBuffer& buffer, const std::string_view text)
{
    Check(
        buffer.Append(ViewOf(text)) ==
            iocp::buffer::BufferStatus::Ready,
        "test receive buffer append failed");
}

HttpParseResult ParseWhole(
    const std::string_view wire,
    HttpParserOptions options = {})
{
    RingReceiveBuffer buffer(8, wire.size() + 16);
    Append(buffer, wire);
    HttpRequestParser parser(options);
    return parser.Parse(buffer.ReadableSequence());
}

void TestGetAtEverySplitBoundary()
{
    constexpr std::string_view wire =
        "GET /health?verbose=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Trace: abc\r\n"
        "\r\n";

    for (std::size_t split = 0; split <= wire.size(); ++split)
    {
        RingReceiveBuffer buffer(8, wire.size() + 16);
        HttpRequestParser parser;

        Append(buffer, wire.substr(0, split));
        const auto first = parser.Parse(buffer.ReadableSequence());
        if (split < wire.size())
        {
            Check(
                first.status == HttpParseStatus::Incomplete,
                "partial GET request did not remain incomplete");
            Append(buffer, wire.substr(split));
        }

        const auto result =
            split == wire.size()
            ? first
            : parser.Parse(buffer.ReadableSequence());
        Check(
            result.status == HttpParseStatus::Complete &&
                result.consumed_bytes == wire.size(),
            "split GET request did not complete");
        Check(
            result.request.method == HttpMethod::Get &&
                result.request.path == "/health" &&
                result.request.query == "verbose=1" &&
                result.request.keep_alive,
            "GET request metadata was decoded incorrectly");
        Check(
            result.request.Header("X-TRACE") == "abc",
            "case-insensitive HTTP header lookup failed");
    }
}

void TestPostOneByteAtATime()
{
    constexpr std::string_view wire =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello";

    RingReceiveBuffer buffer(4, wire.size() + 16);
    HttpRequestParser parser;
    HttpParseResult result;
    for (std::size_t index = 0; index < wire.size(); ++index)
    {
        Append(buffer, wire.substr(index, 1));
        result = parser.Parse(buffer.ReadableSequence());
        Check(
            index + 1 == wire.size()
                ? result.status == HttpParseStatus::Complete
                : result.status == HttpParseStatus::Incomplete,
            "one-byte POST parsing changed state unexpectedly");
    }

    Check(
        result.request.method == HttpMethod::Post &&
            result.request.path == "/echo" &&
            !result.request.keep_alive &&
            StringFromBytes(result.request.body) == "hello",
        "POST body or connection policy was decoded incorrectly");
}

void TestPipelinedRequests()
{
    constexpr std::string_view first_wire =
        "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n";
    constexpr std::string_view second_wire =
        "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const std::string wire =
        std::string(first_wire) + std::string(second_wire);

    RingReceiveBuffer buffer(16, wire.size() + 16);
    Append(buffer, wire);
    HttpRequestParser parser;

    const auto first = parser.Parse(buffer.ReadableSequence());
    Check(
        first.status == HttpParseStatus::Complete &&
            first.request.path == "/one" &&
            first.consumed_bytes == first_wire.size(),
        "first pipelined request was decoded incorrectly");
    buffer.Consume(first.consumed_bytes);

    const auto second = parser.Parse(buffer.ReadableSequence());
    Check(
        second.status == HttpParseStatus::Complete &&
            second.request.path == "/two" &&
            second.consumed_bytes == second_wire.size(),
        "second pipelined request was decoded incorrectly");
}

void TestParserErrorsAndLimits()
{
    const auto ExpectError = [](
                                 const std::string_view wire,
                                 const HttpParseError expected,
                                 HttpParserOptions options = {}) {
        const auto result = ParseWhole(wire, options);
        Check(
            result.status == HttpParseStatus::Error &&
                result.error == expected,
            "HTTP parser returned an unexpected error");
    };

    ExpectError(
        "GET / HTTP/1.1\nHost: x\r\n\r\n",
        HttpParseError::InvalidLineEnding);
    ExpectError(
        "GET / HTTP/1.1\r\n\r\n",
        HttpParseError::MissingHost);
    ExpectError(
        "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        HttpParseError::DuplicateHost);
    ExpectError(
        "POST / HTTP/1.1\r\nHost: a\r\n"
        "Content-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        HttpParseError::DuplicateContentLength);
    ExpectError(
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: x\r\n\r\n",
        HttpParseError::InvalidContentLength);
    ExpectError(
        "POST / HTTP/1.1\r\nHost: a\r\n"
        "Transfer-Encoding: something-unsupported\r\n\r\n",
        HttpParseError::UnsupportedTransferEncoding);
    ExpectError(
        "GET / HTTP/1.0\r\nHost: a\r\n\r\n",
        HttpParseError::UnsupportedVersion);
    ExpectError(
        "GET absolute HTTP/1.1\r\nHost: a\r\n\r\n",
        HttpParseError::InvalidTarget);

    HttpParserOptions line_options;
    line_options.maximum_request_line_bytes = 8;
    ExpectError(
        "GET /long HTTP/1.1\r\nHost: a\r\n\r\n",
        HttpParseError::RequestLineTooLarge,
        line_options);

    HttpParserOptions count_options;
    count_options.maximum_header_count = 1;
    ExpectError(
        "GET / HTTP/1.1\r\nHost: a\r\nX: b\r\n\r\n",
        HttpParseError::TooManyHeaders,
        count_options);

    HttpParserOptions body_options;
    body_options.maximum_body_bytes = 3;
    ExpectError(
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n\r\ntest",
        HttpParseError::BodyTooLarge,
        body_options);

    const auto unsupported = ParseWhole(
        "BOGUS / HTTP/1.1\r\nHost: a\r\n\r\n");
    Check(
        unsupported.status == HttpParseStatus::Complete &&
            unsupported.request.method == HttpMethod::Unsupported &&
            unsupported.request.method_text == "BOGUS",
        "syntactically valid unsupported method did not reach routing");
}

void TestResponseEncoder()
{
    HttpResponse response =
        MakeTextResponse(200, "hello", "text/plain");
    response.headers.push_back({"X-Trace", "abc"});

    HttpResponseEncoder encoder;
    const auto encoded = encoder.Encode(std::move(response));
    const std::string head = StringFromBytes(encoded.head);
    Check(
        head.find("HTTP/1.1 200 OK\r\n") == 0 &&
            head.find("Content-Type: text/plain\r\n") !=
                std::string::npos &&
            head.find("X-Trace: abc\r\n") != std::string::npos &&
            head.find("Content-Length: 5\r\n") !=
                std::string::npos &&
            head.find("Connection: keep-alive\r\n") !=
                std::string::npos &&
            StringFromBytes(encoded.body) == "hello" &&
            !encoded.close_connection,
        "HTTP response encoding was incorrect");

    HttpResponse close = MakeTextResponse(404, "missing");
    close.close_connection = true;
    const auto close_encoded = encoder.Encode(std::move(close));
    Check(
        StringFromBytes(close_encoded.head).find(
            "Connection: close\r\n") != std::string::npos &&
            close_encoded.close_connection,
        "HTTP close response did not encode its connection policy");
}

void TestRouterFallbacks()
{
    auto router = std::make_shared<HttpRouter>();
    Check(
        router->Register(
            HttpMethod::Get,
            "/health",
            [](const HttpRequest&) {
                return MakeTextResponse(200, "healthy\n");
            }),
        "GET /health route registration failed");

    auto executor = std::make_shared<ManualExecutor>(8);
    std::vector<HttpResponse> responses;
    const auto sender = [&](HttpResponse response) {
        responses.push_back(std::move(response));
    };

    HttpRequest health;
    health.method = HttpMethod::Get;
    health.path = "/health";
    Check(
        router->Dispatch(
            std::move(health),
            executor,
            sender,
            false) == HttpDispatchStatus::Accepted,
        "registered route was not admitted");

    HttpRequest wrong_method;
    wrong_method.method = HttpMethod::Post;
    wrong_method.path = "/health";
    Check(
        router->Dispatch(
            std::move(wrong_method),
            executor,
            sender,
            false) == HttpDispatchStatus::Accepted,
        "405 fallback was not admitted");

    HttpRequest missing;
    missing.method = HttpMethod::Get;
    missing.path = "/missing";
    Check(
        router->Dispatch(
            std::move(missing),
            executor,
            sender,
            true) == HttpDispatchStatus::Accepted,
        "404 fallback was not admitted");

    Check(
        executor->RunReady() == 3 &&
            responses.size() == 3 &&
            responses[0].status_code == 200 &&
            responses[1].status_code == 405 &&
            responses[1].headers.size() == 2 &&
            responses[1].headers[1].name == "Allow" &&
            responses[1].headers[1].value == "GET" &&
            responses[2].status_code == 404 &&
            responses[2].close_connection,
        "router responses or ordering were incorrect");
    executor->Stop(StopMode::Drain);
    executor->RunReady();
}

void TestSessionPipeliningAndClosePolicy()
{
    auto router = std::make_shared<HttpRouter>();
    Check(
        router->Register(
            HttpMethod::Get,
            "/value",
            [](const HttpRequest& request) {
                return MakeTextResponse(200, request.query);
            }),
        "session test route registration failed");

    auto executor = std::make_shared<ManualExecutor>(8);
    std::vector<HttpResponse> responses;
    HttpSessionOptions options;
    options.initial_buffer_bytes = 8;
    options.maximum_buffer_bytes = 1024;
    options.maximum_requests_per_connection = 2;
    HttpSession session(
        router,
        executor,
        [&](HttpResponse response) {
            responses.push_back(std::move(response));
        },
        options);

    const std::string wire =
        "GET /value?one HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /value?two HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto result = session.Feed(ViewOf(wire));
    Check(
        result.status == iocp::protocol::ProtocolFeedStatus::Ready &&
            result.messages_dispatched == 2 &&
            session.IsStopped() &&
            session.RequestsDispatched() == 2,
        "HTTP session did not dispatch the pipeline");
    Check(
        executor->RunReady() == 2 &&
            responses.size() == 2 &&
            StringFromBytes(responses[0].body) == "one" &&
            !responses[0].close_connection &&
            StringFromBytes(responses[1].body) == "two" &&
            responses[1].close_connection,
        "HTTP pipeline order or request limit policy was incorrect");
    executor->Stop(StopMode::Drain);
    executor->RunReady();
}

void TestSessionErrorResponseAndSaturation()
{
    auto router = std::make_shared<HttpRouter>();
    auto executor = std::make_shared<ManualExecutor>(2);
    std::vector<HttpResponse> responses;
    HttpSession session(
        router,
        executor,
        [&](HttpResponse response) {
            responses.push_back(std::move(response));
        });

    const auto error = session.Feed(
        ViewOf("GET / HTTP/1.1\r\n\r\n"));
    Check(
        error.status ==
                iocp::protocol::ProtocolFeedStatus::ProtocolError &&
            session.LastParseError() == HttpParseError::MissingHost &&
            executor->RunReady() == 1 &&
            responses.size() == 1 &&
            responses[0].status_code == 400 &&
            responses[0].close_connection,
        "HTTP parser error did not become a close response");
    executor->Stop(StopMode::Drain);
    executor->RunReady();

    auto saturated = std::make_shared<ManualExecutor>(1);
    Check(
        saturated->Post([] {}) == SubmitStatus::Accepted,
        "failed to seed saturated executor");
    HttpSession saturated_session(
        router,
        saturated,
        [](HttpResponse) {});
    const auto rejected = saturated_session.Feed(
        ViewOf("GET /missing HTTP/1.1\r\nHost: x\r\n\r\n"));
    Check(
        rejected.status ==
            iocp::protocol::ProtocolFeedStatus::ExecutorSaturated,
        "HTTP session did not report executor saturation");
    saturated->RunReady();
    saturated->Stop(StopMode::Drain);
    saturated->RunReady();
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
        "GET at every split boundary",
        TestGetAtEverySplitBoundary);
    failures += !RunTest(
        "POST one byte at a time",
        TestPostOneByteAtATime);
    failures += !RunTest(
        "pipelined requests",
        TestPipelinedRequests);
    failures += !RunTest(
        "parser errors and limits",
        TestParserErrorsAndLimits);
    failures += !RunTest(
        "response encoder",
        TestResponseEncoder);
    failures += !RunTest(
        "router fallbacks",
        TestRouterFallbacks);
    failures += !RunTest(
        "session pipeline and close policy",
        TestSessionPipeliningAndClosePolicy);
    failures += !RunTest(
        "session error response and saturation",
        TestSessionErrorResponseAndSaturation);
    return failures == 0 ? 0 : 1;
}
