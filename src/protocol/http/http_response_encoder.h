/// @file http_response_encoder.h
/// @brief HTTP/1.1 response 직렬화

#pragma once

#include "protocol/http/http_message.h"

#include <cstddef>
#include <string>
#include <vector>

namespace iocp::protocol::http
{

struct HttpResponseEncoderOptions final
{
    std::string server_name{"iocp-http-runtime"};
    std::size_t maximum_header_bytes{32 * 1024};
};

struct EncodedHttpResponse final
{
    std::vector<std::byte> head;
    std::vector<std::byte> body;
    bool close_connection{};
};

/// @brief response head와 body를 별도 immutable send segment로 인코딩한다.
class HttpResponseEncoder final
{
public:
    explicit HttpResponseEncoder(
        HttpResponseEncoderOptions options = {});

    EncodedHttpResponse Encode(HttpResponse response) const;

private:
    HttpResponseEncoderOptions options_;
};

} // namespace iocp::protocol::http
