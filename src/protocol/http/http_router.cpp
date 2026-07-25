// HTTP route 등록 및 executor 기반 dispatch 구현
#include "protocol/http/http_router.h"

#include <stdexcept>
#include <utility>

namespace iocp::protocol::http
{

namespace
{

HttpDispatchStatus ToDispatchStatus(
    const execution::SubmitStatus status) noexcept
{
    switch (status)
    {
    case execution::SubmitStatus::Accepted:
        return HttpDispatchStatus::Accepted;
    case execution::SubmitStatus::Stopped:
        return HttpDispatchStatus::ExecutorStopped;
    case execution::SubmitStatus::Saturated:
        return HttpDispatchStatus::ExecutorSaturated;
    }
    return HttpDispatchStatus::ExecutorStopped;
}

} // namespace

bool HttpRouter::Register(
    const HttpMethod method,
    std::string path,
    HttpHandler handler)
{
    if (method == HttpMethod::Unsupported)
    {
        throw std::invalid_argument(
            "an unsupported HTTP method cannot be registered");
    }
    if (path.empty() || path.front() != '/')
    {
        throw std::invalid_argument(
            "an HTTP route path must start with '/'");
    }
    if (!handler)
    {
        throw std::invalid_argument(
            "an HTTP route handler must not be empty");
    }

    std::lock_guard lock(mutex_);
    MethodHandlers& handlers = routes_[std::move(path)];
    HttpHandler* slot = nullptr;
    if (method == HttpMethod::Get)
    {
        slot = &handlers.get;
    }
    else if (method == HttpMethod::Post)
    {
        slot = &handlers.post;
    }
    else if (method == HttpMethod::Put)
    {
        slot = &handlers.put;
    }
    else if (method == HttpMethod::Delete_)
    {
        slot = &handlers.delete_;
    }
    if (!slot || *slot)
    {
        return false;
    }
    *slot = std::move(handler);
    return true;
}

HttpDispatchStatus HttpRouter::Dispatch(
    HttpRequest request,
    std::shared_ptr<execution::IExecutor> executor,
    HttpResponseSender response_sender,
    const bool close_connection) const
{
    if (!executor)
    {
        throw std::invalid_argument(
            "HTTP dispatch requires an executor");
    }
    if (!response_sender)
    {
        throw std::invalid_argument(
            "HTTP dispatch requires a response sender");
    }

    HttpHandler handler;
    HttpResponse fallback;
    {
        // --- dispatch: route lookup 후 executor에 handler 실행 등록 ---
        std::lock_guard lock(mutex_);
        const auto route = routes_.find(request.path);
        if (route == routes_.end())
        {
            fallback = MakeTextResponse(404, "not found\n");
        }
        else
        {
            if (request.method == HttpMethod::Get ||
                request.method == HttpMethod::Head)
            {
                handler = route->second.get;
            }
            else if (request.method == HttpMethod::Post)
            {
                handler = route->second.post;
            }
            else if (request.method == HttpMethod::Put)
            {
                handler = route->second.put;
            }
            else if (request.method == HttpMethod::Delete_)
            {
                handler = route->second.delete_;
            }

            if (!handler)
            {
                fallback = MethodNotAllowed(route->second);
            }
        }
    }

    const execution::SubmitStatus status = executor->Post(
        [request = std::move(request),
         handler = std::move(handler),
         fallback = std::move(fallback),
         response_sender = std::move(response_sender),
         close_connection]() mutable {
            HttpResponse response;
            try
            {
                response = handler
                    ? handler(request)
                    : std::move(fallback);
            }
            catch (...)
            {
                response = MakeTextResponse(
                    500,
                    "internal server error\n");
                response.close_connection = true;
            }
            response.close_connection =
                response.close_connection || close_connection;
            if (request.method == HttpMethod::Head)
            {
                response.body.clear();
            }
            response_sender(std::move(response));
        });
    return ToDispatchStatus(status);
}

HttpResponse HttpRouter::MethodNotAllowed(
    const MethodHandlers& handlers)
{
    HttpResponse response =
        MakeTextResponse(405, "method not allowed\n");
    std::string allow;
    if (handlers.get)
    {
        allow = "GET";
    }
    if (handlers.post)
    {
        if (!allow.empty())
        {
            allow += ", ";
        }
        allow += "POST";
    }
    if (handlers.put)
    {
        if (!allow.empty())
        {
            allow += ", ";
        }
        allow += "PUT";
    }
    if (handlers.delete_)
    {
        if (!allow.empty())
        {
            allow += ", ";
        }
        allow += "DELETE";
    }
    response.headers.push_back({"Allow", std::move(allow)});
    return response;
}

} // namespace iocp::protocol::http
