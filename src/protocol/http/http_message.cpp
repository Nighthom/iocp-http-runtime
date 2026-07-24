// HTTP message 유틸리티 구현: Header 검색, method 파싱, reason phrase, byte 변환, 응답 생성
#include "protocol/http/http_message.h"

#include <algorithm>
#include <cctype>

namespace iocp::protocol::http
{

namespace
{

bool EqualsIgnoreCase(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto left_character =
            static_cast<unsigned char>(left[index]);
        const auto right_character =
            static_cast<unsigned char>(right[index]);
        if (std::tolower(left_character) !=
            std::tolower(right_character))
        {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<std::string_view> HttpRequest::Header(
    const std::string_view name) const noexcept
{
    const auto found = std::find_if(
        headers.begin(),
        headers.end(),
        [name](const HttpHeader& header) {
            return EqualsIgnoreCase(header.name, name);
        });
    if (found == headers.end())
    {
        return std::nullopt;
    }
    return found->value;
}

HttpMethod ParseMethod(const std::string_view value) noexcept
{
    if (value == "GET")
    {
        return HttpMethod::Get;
    }
    if (value == "POST")
    {
        return HttpMethod::Post;
    }
    return HttpMethod::Unsupported;
}

std::string_view MethodName(const HttpMethod method) noexcept
{
    switch (method)
    {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Unsupported:
        return "UNSUPPORTED";
    }
    return "UNSUPPORTED";
}

std::string_view DefaultReasonPhrase(
    const std::uint16_t status_code) noexcept
{
    switch (status_code)
    {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 414:
        return "URI Too Long";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Unknown";
    }
}

std::vector<std::byte> BytesFromString(
    const std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    const auto* first =
        reinterpret_cast<const std::byte*>(value.data());
    return std::vector<std::byte>(first, first + value.size());
}

std::string StringFromBytes(
    const std::vector<std::byte>& value)
{
    if (value.empty())
    {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(value.data()),
        value.size());
}

HttpResponse MakeTextResponse(
    const std::uint16_t status_code,
    const std::string_view body,
    const std::string_view content_type)
{
    HttpResponse response;
    response.status_code = status_code;
    response.headers.push_back(
        HttpHeader{"Content-Type", std::string(content_type)});
    response.body = BytesFromString(body);
    return response;
}

} // namespace iocp::protocol::http
