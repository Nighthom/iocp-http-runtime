// HTTP/1.1 response 인코딩 구현: status line, headers, body를 wire format으로 변환
#include "protocol/http/http_response_encoder.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace iocp::protocol::http
{

namespace
{

bool IsTokenCharacter(const unsigned char value) noexcept
{
    if (std::isalnum(value) != 0)
    {
        return true;
    }
    constexpr std::string_view kPunctuation =
        "!#$%&'*+-.^_`|~";
    return kPunctuation.find(static_cast<char>(value)) !=
        std::string_view::npos;
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

void ValidateHeader(const HttpHeader& header)
{
    if (header.name.empty())
    {
        throw std::invalid_argument(
            "HTTP response header 이름은 비어 있을 수 없습니다");
    }
    for (const char character : header.name)
    {
        if (!IsTokenCharacter(
                static_cast<unsigned char>(character)))
        {
            throw std::invalid_argument(
                "HTTP response header 이름이 token 규칙을 위반합니다");
        }
    }
    for (const char character : header.value)
    {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte < 0x20 && byte != '\t') || byte == 0x7f)
        {
            throw std::invalid_argument(
                "HTTP response header 값에 금지된 문자가 있습니다");
        }
    }
}

} // namespace

HttpResponseEncoder::HttpResponseEncoder(
    HttpResponseEncoderOptions options)
    : options_(std::move(options))
{
    if (options_.server_name.empty() ||
        options_.maximum_header_bytes == 0)
    {
        throw std::invalid_argument(
            "HTTP response encoder 설정이 유효하지 않습니다");
    }
}

EncodedHttpResponse HttpResponseEncoder::Encode(
    HttpResponse response) const
{
    if (response.status_code < 100 ||
        response.status_code > 599)
    {
        throw std::invalid_argument(
            "HTTP response status code는 100..599 범위여야 합니다");
    }

    const std::string_view reason =
        response.reason_phrase.empty()
        ? DefaultReasonPhrase(response.status_code)
        : std::string_view(response.reason_phrase);
    if (reason.find('\r') != std::string_view::npos ||
        reason.find('\n') != std::string_view::npos)
    {
        throw std::invalid_argument(
            "HTTP reason phrase에 줄바꿈을 사용할 수 없습니다");
    }

    std::string head =
        "HTTP/1.1 " + std::to_string(response.status_code) +
        " " + std::string(reason) + "\r\n";
    bool has_server = false;
    for (const HttpHeader& header : response.headers)
    {
        ValidateHeader(header);
        const std::string name = Lowercase(header.name);
        if (name == "content-length" || name == "connection")
        {
            continue;
        }
        has_server = has_server || name == "server";
        head += header.name + ": " + header.value + "\r\n";
    }

    if (!has_server)
    {
        head += "Server: " + options_.server_name + "\r\n";
    }
    head += "Content-Length: " +
        std::to_string(response.body.size()) + "\r\n";
    head += response.close_connection
        ? "Connection: close\r\n"
        : "Connection: keep-alive\r\n";
    head += "\r\n";

    if (head.size() > options_.maximum_header_bytes)
    {
        throw std::length_error(
            "HTTP response header가 설정된 상한을 넘습니다");
    }

    return EncodedHttpResponse{
        BytesFromString(head),
        std::move(response.body),
        response.close_connection,
    };
}

} // namespace iocp::protocol::http
