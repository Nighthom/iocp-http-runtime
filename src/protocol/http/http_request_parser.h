/// @file http_request_parser.h
/// @brief incremental HTTP/1.1 request parser

#pragma once

#include "buffer/buffer_sequence.h"
#include "protocol/http/http_message.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace iocp::protocol::http
{

struct HttpParserOptions final
{
    std::size_t maximum_request_line_bytes{8 * 1024};
    std::size_t maximum_header_bytes{32 * 1024};
    std::size_t maximum_header_count{64};
    std::size_t maximum_body_bytes{1024 * 1024};
};

enum class HttpParseStatus : std::uint8_t
{
    Incomplete,
    Complete,
    HeadersComplete,
    Error,
};

enum class HttpParseError : std::uint8_t
{
    None,
    InvalidLineEnding,
    RequestLineTooLarge,
    InvalidRequestLine,
    UnsupportedVersion,
    InvalidTarget,
    HeaderTooLarge,
    TooManyHeaders,
    InvalidHeader,
    MissingHost,
    DuplicateHost,
    InvalidContentLength,
    DuplicateContentLength,
    UnsupportedTransferEncoding,
    BodyTooLarge,
};

struct HttpParseResult final
{
    HttpParseStatus status{HttpParseStatus::Incomplete};
    HttpParseError error{HttpParseError::None};
    std::size_t consumed_bytes{};
    HttpRequest request;
    bool expect_continue{};
};

/// @brief HTTP/1.1 request line, headers, Content-Length body를 증분 해석한다.
///
/// parser는 같은 unconsumed logical stream이 append로만 늘어나는 동안
/// scan offset을 보존한다. complete 결과를 반환하면 다음 request를 위해
/// 자동으로 reset한다.
class HttpRequestParser final
{
public:
    explicit HttpRequestParser(HttpParserOptions options = {});

    HttpParseResult Parse(buffer::BufferSequence input);

    void Reset() noexcept;
    HttpParseError LastError() const noexcept;

private:
    enum class State : std::uint8_t
    {
        RequestLine,
        Headers,
        Body,
        ChunkHead,
        ChunkBody,
        ChunkTrailer,
        ChunkEnd,
        Error,
    };

    enum class LineStatus : std::uint8_t
    {
        Incomplete,
        Complete,
        Error,
        LimitExceeded,
    };

    struct LineResult final
    {
        LineStatus status{LineStatus::Incomplete};
        std::string value;
    };

    LineResult ReadLine(
        buffer::BufferSequence input,
        std::size_t maximum_bytes);
    bool ParseRequestLine(const std::string& line);
    bool ParseHeaderLine(const std::string& line);
    bool FinalizeHeaders();
    HttpParseResult Complete(buffer::BufferSequence input);
    HttpParseResult Fail(HttpParseError error) noexcept;

    HttpParserOptions options_;
    State state_{State::RequestLine};
    HttpRequest request_;
    std::size_t line_start_{};
    std::size_t scan_offset_{};
    std::size_t header_bytes_{};
    std::size_t content_length_{};
    std::size_t body_offset_{};
    std::size_t chunk_size_{};
    std::size_t chunk_body_offset_{};
    bool has_chunked_{};
    bool expect_continue_{};
    bool expect_continue_sent_{};
    HttpParseError last_error_{HttpParseError::None};
};

} // namespace iocp::protocol::http
