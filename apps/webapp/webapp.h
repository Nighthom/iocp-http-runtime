/// @file webapp.h
/// @brief 간단한 게시판 + 로그인 기능을 가진 테스트용 웹 애플리케이션

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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace iocp::server
{

struct WebAppOptions final
{
    std::size_t io_worker_count{2};
    std::size_t application_worker_count{2};
    std::size_t maximum_application_tasks{1024};
    std::size_t maximum_connection_tasks{128};
    transport::ListenerOptions listener;
    transport::ConnectionOptions connection;
    protocol::http::HttpSessionOptions session;
    std::chrono::milliseconds shutdown_timeout{
        std::chrono::seconds{10}};
};

/// @brief 게시판 게시글 데이터다.
struct Post final
{
    std::uint64_t id{};
    std::string title;
    std::string author;
    std::string content;
    std::chrono::system_clock::time_point created_at;
};

/// @brief 간단한 게시판 + 로그인 웹 애플리케이션 서버다.
class WebAppServer final
{
public:
    static std::unique_ptr<WebAppServer> Create(
        std::shared_ptr<core::Logger> logger,
        WebAppOptions options = {});

    ~WebAppServer();

    WebAppServer(const WebAppServer&) = delete;
    WebAppServer& operator=(const WebAppServer&) = delete;

    bool Stop();
    std::uint16_t LocalPort() const;

private:
    WebAppServer(
        std::shared_ptr<core::Logger> logger,
        WebAppOptions options);

    void RegisterRoutes();
    void Start();
    void OnAccepted(
        platform::windows::SocketHandle socket) noexcept;

    // Auth helpers
    std::string GenerateSessionToken() const;
    bool ValidateSession(const std::string& token) const;
    std::string GetUsername(const std::string& token) const;

    static std::string HtmlEscape(const std::string& value);
    static std::string RenderLoginPage(
        const std::string& error = {});
    static std::string RenderBoardPage(
        const std::vector<Post>& posts,
        const std::string& username);
    static std::string RenderPostDetail(const Post& post);
    static std::string RenderWriteForm(
        const std::string& username);
    static std::string RenderStyles();

    static protocol::http::HttpResponse HtmlResponse(
        std::uint16_t status_code,
        const std::string& html);

    std::shared_ptr<core::Logger> logger_;
    WebAppOptions options_;
    platform::windows::WinsockRuntime winsock_;
    runtime::IoContext io_context_;
    std::shared_ptr<execution::ThreadPoolContext>
        application_context_;
    std::shared_ptr<execution::ThreadPoolExecutor>
        application_executor_;
    std::shared_ptr<transport::ConnectionRegistry> registry_;
    std::shared_ptr<protocol::http::HttpRouter> router_;
    std::shared_ptr<transport::TcpListener> listener_;

    mutable std::mutex state_mutex_;
    std::mutex stop_mutex_;
    enum class State { Created, Running, Stopping, Stopped };
    State state_{State::Created};

    // Auth state
    mutable std::mutex auth_mutex_;
    std::unordered_map<std::string, std::string> sessions_;
    std::unordered_map<std::string, std::string> users_{
        {"admin", "admin123"},
        {"user", "pass123"},
    };

    // Board state
    mutable std::mutex board_mutex_;
    std::vector<Post> posts_;
    std::atomic<std::uint64_t> next_post_id_{1};
};

} // namespace iocp::server
