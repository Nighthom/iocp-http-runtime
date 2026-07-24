/// @file http_message.h
/// @brief HttpRequest/HttpResponse data types

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iocp::protocol::http
{

enum class HttpMethod : std::uint8_t
{
    Get,
    Post,
    Put,
    Delete_,
    Patch,
    Head,
    Options,
    Unsupported,
};

struct HttpHeader final
{
    std::string name;
    std::string value;
};

struct HttpRequest final
{
    HttpMethod method{HttpMethod::Unsupported};
    std::string method_text;
    std::string target;
    std::string path;
    std::string query;
    std::vector<HttpHeader> headers;
    std::vector<std::byte> body;
    bool keep_alive{true};
    std::uint64_t request_id{};
    std::uint64_t connection_id{};

    std::optional<std::string_view> Header(
        std::string_view name) const noexcept;

    /// @brief query string을 key-value map으로 분해한다.
    std::unordered_map<std::string, std::string> QueryParams() const;
};

struct HttpResponse final
{
    std::uint16_t status_code{200};
    std::string reason_phrase;
    std::vector<HttpHeader> headers;
    std::vector<std::byte> body;
    bool close_connection{};
};

HttpMethod ParseMethod(std::string_view value) noexcept;
std::string_view MethodName(HttpMethod method) noexcept;
std::string_view DefaultReasonPhrase(std::uint16_t status_code) noexcept;

std::vector<std::byte> BytesFromString(std::string_view value);
std::string StringFromBytes(const std::vector<std::byte>& value);

HttpResponse MakeTextResponse(
    std::uint16_t status_code,
    std::string_view body,
    std::string_view content_type = "text/plain; charset=utf-8");

} // namespace iocp::protocol::http
