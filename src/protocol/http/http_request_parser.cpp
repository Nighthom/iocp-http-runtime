#include "protocol/http/http_request_parser.h"

#include "buffer/byte_view.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace iocp::protocol::http
{

namespace
{

char ToChar(const std::byte value) noexcept
{
    return static_cast<char>(std::to_integer<unsigned char>(value));
}

bool IsTokenCharacter(const unsigned char value) noexcept
{
    if (std::isalnum(value) != 0)
    {
        return true;
    }

    switch (value)
    {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

std::string Lowercase(const std::string_view value)
{
    std::string result(value);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return result;
}

std::string_view TrimOptionalWhitespace(
    std::string_view value) noexcept
{
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t'))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t'))
    {
        value.remove_suffix(1);
    }
    return value;
}

bool HeaderContainsToken(
    const std::string_view value,
    const std::string_view expected)
{
    std::size_t offset = 0;
    while (offset <= value.size())
    {
        const std::size_t comma = value.find(',', offset);
        const std::size_t end =
            comma == std::string_view::npos ? value.size() : comma;
        if (Lowercase(TrimOptionalWhitespace(
                value.substr(offset, end - offset))) == expected)
        {
            return true;
        }
        if (comma == std::string_view::npos)
        {
            return false;
        }
        offset = comma + 1;
    }
    return false;
}

bool ParseDecimalSize(
    const std::string_view value,
    std::size_t& output) noexcept
{
    if (value.empty())
    {
        return false;
    }

    std::size_t parsed = 0;
    for (const char character : value)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        const std::size_t digit =
            static_cast<std::size_t>(character - '0');
        if (parsed >
            (std::numeric_limits<std::size_t>::max() - digit) / 10)
        {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    output = parsed;
    return true;
}

} // namespace

HttpRequestParser::HttpRequestParser(
    const HttpParserOptions options)
    : options_(options)
{
    if (options_.maximum_request_line_bytes == 0 ||
        options_.maximum_header_bytes < 2 ||
        options_.maximum_header_count == 0)
    {
        throw std::invalid_argument(
            "HTTP request line/header limit은 유효한 양수여야 합니다");
    }
}

HttpParseResult HttpRequestParser::Parse(
    const buffer::BufferSequence input)
{
    if (state_ == State::Error)
    {
        return HttpParseResult{
            HttpParseStatus::Error,
            last_error_,
            0,
            {},
        };
    }
    if (input.Size() < scan_offset_ ||
        input.Size() < line_start_)
    {
        return Fail(HttpParseError::InvalidRequestLine);
    }

    for (;;)
    {
        if (state_ == State::RequestLine)
        {
            LineResult line =
                ReadLine(input, options_.maximum_request_line_bytes);
            if (line.status == LineStatus::Incomplete)
            {
                return {};
            }
            if (line.status == LineStatus::LimitExceeded)
            {
                return Fail(HttpParseError::RequestLineTooLarge);
            }
            if (line.status == LineStatus::Error)
            {
                return Fail(HttpParseError::InvalidLineEnding);
            }
            if (!ParseRequestLine(line.value))
            {
                return HttpParseResult{
                    HttpParseStatus::Error,
                    last_error_,
                    0,
                    {},
                };
            }
            state_ = State::Headers;
            continue;
        }

        if (state_ == State::Headers)
        {
            const std::size_t remaining_header_bytes =
                header_bytes_ >= options_.maximum_header_bytes
                ? 0
                : options_.maximum_header_bytes - header_bytes_;
            LineResult line = ReadLine(
                input,
                remaining_header_bytes);
            if (line.status == LineStatus::Incomplete)
            {
                return {};
            }
            if (line.status == LineStatus::LimitExceeded)
            {
                return Fail(HttpParseError::HeaderTooLarge);
            }
            if (line.status == LineStatus::Error)
            {
                return Fail(HttpParseError::InvalidLineEnding);
            }

            const std::size_t wire_line_bytes = line.value.size() + 2;
            if (wire_line_bytes >
                options_.maximum_header_bytes - header_bytes_)
            {
                return Fail(HttpParseError::HeaderTooLarge);
            }
            header_bytes_ += wire_line_bytes;

            if (line.value.empty())
            {
                if (!FinalizeHeaders())
                {
                    return HttpParseResult{
                        HttpParseStatus::Error,
                        last_error_,
                        0,
                        {},
                    };
                }
                body_offset_ = scan_offset_;
                if (content_length_ == 0)
                {
                    return Complete(input);
                }
                state_ = State::Body;
                continue;
            }

            if (request_.headers.size() >=
                options_.maximum_header_count)
            {
                return Fail(HttpParseError::TooManyHeaders);
            }
            if (!ParseHeaderLine(line.value))
            {
                return HttpParseResult{
                    HttpParseStatus::Error,
                    last_error_,
                    0,
                    {},
                };
            }
            continue;
        }

        if (state_ == State::Body)
        {
            if (body_offset_ >
                std::numeric_limits<std::size_t>::max() - content_length_)
            {
                return Fail(HttpParseError::BodyTooLarge);
            }
            if (input.Size() < body_offset_ + content_length_)
            {
                return {};
            }
            return Complete(input);
        }
    }
}

void HttpRequestParser::Reset() noexcept
{
    state_ = State::RequestLine;
    request_ = {};
    line_start_ = 0;
    scan_offset_ = 0;
    header_bytes_ = 0;
    content_length_ = 0;
    body_offset_ = 0;
    last_error_ = HttpParseError::None;
}

HttpParseError HttpRequestParser::LastError() const noexcept
{
    return last_error_;
}

HttpRequestParser::LineResult HttpRequestParser::ReadLine(
    const buffer::BufferSequence input,
    const std::size_t maximum_bytes)
{
    for (std::size_t index = scan_offset_;
         index < input.Size();
         ++index)
    {
        const char character = ToChar(input.At(index));
        if (character == '\n')
        {
            return LineResult{LineStatus::Error, {}};
        }
        if (character == '\r')
        {
            if (index + 1 >= input.Size())
            {
                scan_offset_ = index;
                return {};
            }
            if (ToChar(input.At(index + 1)) != '\n')
            {
                return LineResult{LineStatus::Error, {}};
            }

            const std::size_t line_bytes = index - line_start_;
            if (line_bytes > maximum_bytes)
            {
                return LineResult{LineStatus::LimitExceeded, {}};
            }

            std::string line(line_bytes, '\0');
            if (!line.empty())
            {
                input.CopyTo(
                    line_start_,
                    buffer::MutableByteView(
                        reinterpret_cast<std::byte*>(line.data()),
                        line.size()));
            }
            scan_offset_ = index + 2;
            line_start_ = scan_offset_;
            return LineResult{
                LineStatus::Complete,
                std::move(line),
            };
        }

        if (index - line_start_ + 1 > maximum_bytes)
        {
            return LineResult{LineStatus::LimitExceeded, {}};
        }
    }

    scan_offset_ = input.Size();
    return {};
}

bool HttpRequestParser::ParseRequestLine(
    const std::string& line)
{
    const std::size_t first_space = line.find(' ');
    const std::size_t second_space =
        first_space == std::string::npos
        ? std::string::npos
        : line.find(' ', first_space + 1);
    if (first_space == std::string::npos ||
        second_space == std::string::npos ||
        first_space == 0 ||
        second_space == first_space + 1 ||
        second_space + 1 >= line.size() ||
        line.find(' ', second_space + 1) != std::string::npos)
    {
        Fail(HttpParseError::InvalidRequestLine);
        return false;
    }

    const std::string_view method(line.data(), first_space);
    for (const char character : method)
    {
        if (!IsTokenCharacter(
                static_cast<unsigned char>(character)))
        {
            Fail(HttpParseError::InvalidRequestLine);
            return false;
        }
    }

    const std::string_view target(
        line.data() + first_space + 1,
        second_space - first_space - 1);
    const std::string_view version(
        line.data() + second_space + 1,
        line.size() - second_space - 1);
    if (version != "HTTP/1.1")
    {
        Fail(HttpParseError::UnsupportedVersion);
        return false;
    }
    if (target.empty() ||
        target.front() != '/' ||
        target.find('#') != std::string_view::npos)
    {
        Fail(HttpParseError::InvalidTarget);
        return false;
    }

    request_.method = ParseMethod(method);
    request_.method_text.assign(method);
    request_.target.assign(target);

    const std::size_t query = target.find('?');
    request_.path.assign(target.substr(0, query));
    if (request_.path.empty())
    {
        Fail(HttpParseError::InvalidTarget);
        return false;
    }
    if (query != std::string_view::npos)
    {
        request_.query.assign(target.substr(query + 1));
    }
    return true;
}

bool HttpRequestParser::ParseHeaderLine(
    const std::string& line)
{
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0)
    {
        Fail(HttpParseError::InvalidHeader);
        return false;
    }

    const std::string_view name(line.data(), colon);
    for (const char character : name)
    {
        if (!IsTokenCharacter(
                static_cast<unsigned char>(character)))
        {
            Fail(HttpParseError::InvalidHeader);
            return false;
        }
    }

    const std::string_view value = TrimOptionalWhitespace(
        std::string_view(line).substr(colon + 1));
    for (const char character : value)
    {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte < 0x20 && byte != '\t') || byte == 0x7f)
        {
            Fail(HttpParseError::InvalidHeader);
            return false;
        }
    }

    request_.headers.push_back(
        HttpHeader{Lowercase(name), std::string(value)});
    return true;
}

bool HttpRequestParser::FinalizeHeaders()
{
    std::size_t host_count = 0;
    std::size_t content_length_count = 0;
    bool connection_close = false;

    for (const HttpHeader& header : request_.headers)
    {
        if (header.name == "host")
        {
            ++host_count;
            if (header.value.empty())
            {
                Fail(HttpParseError::MissingHost);
                return false;
            }
        }
        else if (header.name == "content-length")
        {
            ++content_length_count;
            if (!ParseDecimalSize(
                    TrimOptionalWhitespace(header.value),
                    content_length_))
            {
                Fail(HttpParseError::InvalidContentLength);
                return false;
            }
        }
        else if (header.name == "transfer-encoding")
        {
            Fail(HttpParseError::UnsupportedTransferEncoding);
            return false;
        }
        else if (header.name == "connection")
        {
            connection_close =
                connection_close ||
                HeaderContainsToken(header.value, "close");
        }
    }

    if (host_count == 0)
    {
        Fail(HttpParseError::MissingHost);
        return false;
    }
    if (host_count > 1)
    {
        Fail(HttpParseError::DuplicateHost);
        return false;
    }
    if (content_length_count > 1)
    {
        Fail(HttpParseError::DuplicateContentLength);
        return false;
    }
    if (content_length_ > options_.maximum_body_bytes)
    {
        Fail(HttpParseError::BodyTooLarge);
        return false;
    }

    request_.keep_alive = !connection_close;
    return true;
}

HttpParseResult HttpRequestParser::Complete(
    const buffer::BufferSequence input)
{
    if (content_length_ != 0)
    {
        request_.body.resize(content_length_);
        input.CopyTo(
            body_offset_,
            buffer::MutableByteView(
                request_.body.data(),
                request_.body.size()));
    }

    const std::size_t consumed_bytes =
        body_offset_ + content_length_;
    HttpRequest request = std::move(request_);
    Reset();
    return HttpParseResult{
        HttpParseStatus::Complete,
        HttpParseError::None,
        consumed_bytes,
        std::move(request),
    };
}

HttpParseResult HttpRequestParser::Fail(
    const HttpParseError error) noexcept
{
    state_ = State::Error;
    last_error_ = error;
    return HttpParseResult{
        HttpParseStatus::Error,
        error,
        0,
        {},
    };
}

} // namespace iocp::protocol::http
