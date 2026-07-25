/// @file http_server.h
/// @brief HTTP 서버 composition root

#pragma once

#include "core/logging.h"
#include "execution/thread_pool_executor.h"
#include "platform/windows/winsock_runtime.h"
#include "protocol/http/http_response_encoder.h"
#include "protocol/http/http_router.h"
#include "protocol/http/http_session.h"
#include "runtime/io_context.h"
#include "transport/connection_registry.h"
#include "transport/tcp_connection.h"
#include "transport/tcp_listener.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>

namespace iocp::server
{

struct HttpServerOptions final
{
    HttpServerOptions();

    std::size_t io_worker_count{2};
    std::size_t application_worker_count{2};
    std::size_t maximum_application_tasks{1024};
    std::size_t maximum_connection_tasks{128};
    bool enable_http2{true};
    transport::ListenerOptions listener;
    transport::ConnectionOptions connection;
    protocol::http::HttpSessionOptions session;
    protocol::http::HttpResponseEncoderOptions encoder;
    std::chrono::milliseconds shutdown_timeout{
        std::chrono::seconds{10}};
};

enum class HttpServerState
{
    Created,
    Running,
    Stopping,
    Stopped,
};

struct HttpServerSnapshot final
{
    HttpServerState state{HttpServerState::Created};
    transport::ListenerSnapshot listener;
    transport::RegistrySnapshot registry;
    execution::ExecutorSnapshot application_executor;
};

/// @brief IOCP transport, HTTP protocol, 기본 service route를 조립한다.
class HttpServer final
{
public:
    static std::unique_ptr<HttpServer> Create(
        std::shared_ptr<core::Logger> logger,
        HttpServerOptions options = {});

    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool Stop();
    bool Stop(std::chrono::milliseconds timeout);

    std::uint16_t LocalPort() const;
    HttpServerSnapshot Snapshot() const;

private:
    HttpServer(
        std::shared_ptr<core::Logger> logger,
        HttpServerOptions options);

    void RegisterRoutes();
    void Start();
    void OnAccepted(platform::windows::SocketHandle socket) noexcept;
    void LogShutdownTimeout(std::string_view barrier) noexcept;

    std::shared_ptr<core::Logger> logger_;
    HttpServerOptions options_;
    platform::windows::WinsockRuntime winsock_;
    runtime::IoContext io_context_;
    std::shared_ptr<execution::ThreadPoolContext> application_context_;
    std::shared_ptr<execution::ThreadPoolExecutor> application_executor_;
    std::shared_ptr<transport::ConnectionRegistry> registry_;
    std::shared_ptr<protocol::http::HttpRouter> router_;
    std::shared_ptr<transport::TcpListener> listener_;

    mutable std::mutex state_mutex_;
    std::mutex stop_mutex_;
    HttpServerState state_{HttpServerState::Created};
};

} // namespace iocp::server
