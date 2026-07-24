// per-connection HTTP session: Append → Parse → Dispatch → Stop 사이클
#include "protocol/http/http_session.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace iocp::protocol::http
{

HttpSession::HttpSession(
    std::shared_ptr<HttpRouter> router,
    std::shared_ptr<execution::IExecutor> executor,
    HttpResponseSender response_sender,
    const HttpSessionOptions options)
    : HttpSession(
          std::move(router),
          std::move(executor),
          std::move(response_sender),
          0,
          options)
{
}

HttpSession::HttpSession(
    std::shared_ptr<HttpRouter> router,
    std::shared_ptr<execution::IExecutor> executor,
    HttpResponseSender response_sender,
    const std::uint64_t connection_id,
    const HttpSessionOptions options)
    : router_(std::move(router)),
      executor_(std::move(executor)),
      response_sender_(std::move(response_sender)),
      receive_buffer_(
          options.initial_buffer_bytes,
          options.maximum_buffer_bytes),
      parser_(options.parser),
      maximum_requests_per_connection_(
          options.maximum_requests_per_connection),
      connection_id_(connection_id)
{
    if (!router_)
    {
        throw std::invalid_argument(
            "HttpSession requires a router");
    }
    if (!executor_)
    {
        throw std::invalid_argument(
            "HttpSession requires an executor");
    }
    if (!response_sender_)
    {
        throw std::invalid_argument(
            "HttpSession requires a response sender");
    }
    if (maximum_requests_per_connection_ == 0)
    {
        throw std::invalid_argument(
            "maximum HTTP requests per connection must be positive");
    }
}

ProtocolFeedResult HttpSession::Feed(
    const buffer::ByteView bytes)
{
    if (stopped_)
    {
        return ProtocolFeedResult{
            ProtocolFeedStatus::Stopped,
            0,
            receive_buffer_.ReadableBytes(),
        };
    }

    if (receive_buffer_.Append(bytes) !=
        buffer::BufferStatus::Ready)
    {
        stopped_ = true;
        const ProtocolFeedStatus status =
            PostErrorResponse(413, "payload too large\n");
        return ProtocolFeedResult{
            status == ProtocolFeedStatus::Ready
                ? ProtocolFeedStatus::BufferLimitExceeded
                : status,
            0,
            receive_buffer_.ReadableBytes(),
        };
    }

    std::size_t dispatched_now = 0;

    // --- Feed loop: Append → Parse → Dispatch → Stop 사이클 ---
    for (;;)
    {
        HttpParseResult parsed =
            parser_.Parse(receive_buffer_.ReadableSequence());

        if (parsed.status == HttpParseStatus::HeadersComplete)
        {
            if (parsed.expect_continue)
            {
                const auto continue_response = MakeTextResponse(
                    100, {}, "text/plain");
                response_sender_(continue_response);
            }
            continue;
        }

        if (parsed.status == HttpParseStatus::Incomplete)
        {
            return ProtocolFeedResult{
                ProtocolFeedStatus::Ready,
                dispatched_now,
                receive_buffer_.ReadableBytes(),
            };
        }

        if (parsed.status == HttpParseStatus::Error)
        {
            stopped_ = true;
            last_parse_error_ = parsed.error;
            const ProtocolFeedStatus status = PostErrorResponse(
                StatusCodeFor(parsed.error),
                "invalid HTTP request\n");
            return ProtocolFeedResult{
                status == ProtocolFeedStatus::Ready
                    ? ProtocolFeedStatus::ProtocolError
                    : status,
                dispatched_now,
                receive_buffer_.ReadableBytes(),
            };
        }

        receive_buffer_.Consume(parsed.consumed_bytes);
        ++requests_dispatched_;
        parsed.request.request_id = next_request_id_++;
        parsed.request.connection_id = connection_id_;
        const bool close_connection =
            !parsed.request.keep_alive ||
            requests_dispatched_ >=
                maximum_requests_per_connection_;
        const HttpDispatchStatus dispatch_status =
            router_->Dispatch(
                std::move(parsed.request),
                executor_,
                response_sender_,
                close_connection);
        if (dispatch_status != HttpDispatchStatus::Accepted)
        {
            stopped_ = true;
            return ProtocolFeedResult{
                ToFeedStatus(dispatch_status),
                dispatched_now,
                receive_buffer_.ReadableBytes(),
            };
        }

        ++dispatched_now;
        if (close_connection)
        {
            stopped_ = true;
            return ProtocolFeedResult{
                ProtocolFeedStatus::Ready,
                dispatched_now,
                receive_buffer_.ReadableBytes(),
            };
        }
    }
}

bool HttpSession::IsStopped() const noexcept
{
    return stopped_;
}

std::size_t HttpSession::BufferedBytes() const noexcept
{
    return receive_buffer_.ReadableBytes();
}

HttpParseError HttpSession::LastParseError() const noexcept
{
    return last_parse_error_;
}

std::size_t HttpSession::RequestsDispatched() const noexcept
{
    return requests_dispatched_;
}

std::uint64_t HttpSession::ConnectionId() const noexcept
{
    return connection_id_;
}

ProtocolFeedStatus HttpSession::PostErrorResponse(
    const std::uint16_t status_code,
    const std::string_view message)
{
    HttpResponse response = MakeTextResponse(status_code, message);
    response.close_connection = true;

    const execution::SubmitStatus status = executor_->Post(
        [response = std::move(response),
         response_sender = response_sender_]() mutable {
            response_sender(std::move(response));
        });
    switch (status)
    {
    case execution::SubmitStatus::Accepted:
        return ProtocolFeedStatus::Ready;
    case execution::SubmitStatus::Stopped:
        return ProtocolFeedStatus::ExecutorStopped;
    case execution::SubmitStatus::Saturated:
        return ProtocolFeedStatus::ExecutorSaturated;
    }
    return ProtocolFeedStatus::ExecutorStopped;
}

ProtocolFeedStatus HttpSession::ToFeedStatus(
    const HttpDispatchStatus status) noexcept
{
    switch (status)
    {
    case HttpDispatchStatus::Accepted:
        return ProtocolFeedStatus::Ready;
    case HttpDispatchStatus::ExecutorStopped:
        return ProtocolFeedStatus::ExecutorStopped;
    case HttpDispatchStatus::ExecutorSaturated:
        return ProtocolFeedStatus::ExecutorSaturated;
    }
    return ProtocolFeedStatus::ExecutorStopped;
}

std::uint16_t HttpSession::StatusCodeFor(
    const HttpParseError error) noexcept
{
    switch (error)
    {
    case HttpParseError::RequestLineTooLarge:
    case HttpParseError::InvalidTarget:
        return 414;
    case HttpParseError::HeaderTooLarge:
    case HttpParseError::TooManyHeaders:
        return 431;
    case HttpParseError::BodyTooLarge:
        return 413;
    case HttpParseError::None:
    case HttpParseError::InvalidLineEnding:
    case HttpParseError::InvalidRequestLine:
    case HttpParseError::UnsupportedVersion:
    case HttpParseError::InvalidHeader:
    case HttpParseError::MissingHost:
    case HttpParseError::DuplicateHost:
    case HttpParseError::InvalidContentLength:
    case HttpParseError::DuplicateContentLength:
    case HttpParseError::UnsupportedTransferEncoding:
        return 400;
    }
    return 400;
}

} // namespace iocp::protocol::http
