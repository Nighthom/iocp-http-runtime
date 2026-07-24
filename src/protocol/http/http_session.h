/// @file http_session.h
/// @brief per-connection HTTP session orchestration

#pragma once

#include "buffer/ring_receive_buffer.h"
#include "execution/executor.h"
#include "protocol/http/http_request_parser.h"
#include "protocol/http/http_router.h"
#include "protocol/protocol_session.h"

#include <cstddef>
#include <memory>

namespace iocp::protocol::http
{

struct HttpSessionOptions final
{
    std::size_t initial_buffer_bytes{4096};
    std::size_t maximum_buffer_bytes{2 * 1024 * 1024};
    std::size_t maximum_requests_per_connection{100};
    HttpParserOptions parser;
};

/// @brief connection 하나의 HTTP parser, receive buffer, ordering 정책을 소유한다.
class HttpSession final : public IProtocolSession
{
public:
    HttpSession(
        std::shared_ptr<HttpRouter> router,
        std::shared_ptr<execution::IExecutor> executor,
        HttpResponseSender response_sender,
        HttpSessionOptions options = {});
    HttpSession(
        std::shared_ptr<HttpRouter> router,
        std::shared_ptr<execution::IExecutor> executor,
        HttpResponseSender response_sender,
        std::uint64_t connection_id,
        HttpSessionOptions options = {});

    ProtocolFeedResult Feed(buffer::ByteView bytes) override;

    bool IsStopped() const noexcept;
    std::size_t BufferedBytes() const noexcept;
    HttpParseError LastParseError() const noexcept;
    std::size_t RequestsDispatched() const noexcept;
    std::uint64_t ConnectionId() const noexcept;

private:
    ProtocolFeedStatus PostErrorResponse(
        std::uint16_t status_code,
        std::string_view message);
    static ProtocolFeedStatus ToFeedStatus(
        HttpDispatchStatus status) noexcept;
    static std::uint16_t StatusCodeFor(
        HttpParseError error) noexcept;

    std::shared_ptr<HttpRouter> router_;
    std::shared_ptr<execution::IExecutor> executor_;
    HttpResponseSender response_sender_;
    buffer::RingReceiveBuffer receive_buffer_;
    HttpRequestParser parser_;
    const std::size_t maximum_requests_per_connection_;
    std::uint64_t connection_id_{};
    std::uint64_t next_request_id_{1};
    bool stopped_{};
    HttpParseError last_parse_error_{HttpParseError::None};
    std::size_t requests_dispatched_{};
};

} // namespace iocp::protocol::http
