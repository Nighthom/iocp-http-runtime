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
    if (value == "PUT")
    {
        return HttpMethod::Put;
    }
    if (value == "DELETE")
    {
        return HttpMethod::Delete_;
    }
    if (value == "PATCH")
    {
        return HttpMethod::Patch;
    }
    if (value == "HEAD")
    {
        return HttpMethod::Head;
    }
    if (value == "OPTIONS")
    {
        return HttpMethod::Options;
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
    case HttpMethod::Put:
        return "PUT";
    case HttpMethod::Delete_:
        return "DELETE";
    case HttpMethod::Patch:
        return "PATCH";
    case HttpMethod::Head:
        return "HEAD";
    case HttpMethod::Options:
        return "OPTIONS";
    case HttpMethod::Unsupported:
        break;
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

std::unordered_map<std::string, std::string>
HttpRequest::QueryParams() const
{
    std::unordered_map<std::string, std::string> params;
    if (query.empty())
    {
        return params;
    }

    std::string_view remaining = query;
    while (!remaining.empty())
    {
        const auto eq = remaining.find('=');
        const auto amp = remaining.find('&');
        const auto segment_end =
            std::min(amp, remaining.size());

        const auto key = remaining.substr(0, eq);
        const auto val = eq != std::string_view::npos &&
                eq < segment_end
            ? remaining.substr(eq + 1, segment_end - eq - 1)
            : std::string_view{};

        params.emplace(key, val);

        if (amp == std::string_view::npos)
        {
            break;
        }
        remaining = remaining.substr(amp + 1);
    }
    return params;
}

} // namespace iocp::protocol::http
