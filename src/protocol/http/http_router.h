/// @file http_router.h
/// @brief exact-path router + executor dispatch

#pragma once

#include "execution/executor.h"
#include "protocol/http/http_message.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iocp::protocol::http
{

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;
using HttpResponseSender = std::function<void(HttpResponse)>;

enum class HttpDispatchStatus : std::uint8_t
{
    Accepted,
    ExecutorStopped,
    ExecutorSaturated,
};

/// @brief HTTP method와 exact path를 application 작업에 연결한다.
class HttpRouter final
{
public:
    bool Register(
        HttpMethod method,
        std::string path,
        HttpHandler handler);

    HttpDispatchStatus Dispatch(
        HttpRequest request,
        std::shared_ptr<execution::IExecutor> executor,
        HttpResponseSender response_sender,
        bool close_connection) const;

private:
    struct MethodHandlers final
    {
        HttpHandler get;
        HttpHandler post;
    };

    static HttpResponse MethodNotAllowed(
        const MethodHandlers& handlers);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, MethodHandlers> routes_;
};

} // namespace iocp::protocol::http
