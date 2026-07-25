/// @file webapp.h
/// @brief 간단한 게시판 + 로그인 웹 애플리케이션 서버

#pragma once

#include "core/logging.h"
#include "core/timer_service.h"
#include "execution/thread_pool_executor.h"
#include "platform/windows/winsock_runtime.h"
#include "protocol/http/http_response_encoder.h"
#include "protocol/http/http_router.h"
#include "protocol/http/http_session.h"
#include "runtime/io_context.h"
#include "transport/connection_registry.h"
#include "transport/tcp_connection.h"
#include "transport/tcp_listener.h"
#include "webapp/board_handlers.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace iocp::server
{

struct AuthState final
{
    std::mutex mutex;
    std::unordered_map<std::string, std::string> sessions;
    std::unordered_map<std::string, std::string> users{
        {"admin", "admin123"},
        {"user", "pass123"},
    };
};

struct WebAppOptions final
{
    std::string home_directory{"apps/webapp/templates"};
    transport::ListenerOptions listener;
    std::chrono::milliseconds shutdown_timeout{
        std::chrono::seconds{10}};
    // 나머지(io_workers, http parser, connection 등)는 httpserver 기본값 사용
};

/// @brief template 기반 게시판 + 로그인 웹 애플리케이션 서버.
///
/// session/auth/board state는 WebAppServer가, HTML 렌더링은
/// board_handlers가 담당한다. handler 함수는 socket/parser 없이
/// 단위 테스트할 수 있다.
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

    // test에서 접근 가능하도록 공개
    std::string GenerateSessionToken() const;
    bool ValidateSession(const std::string& token) const;
    std::string GetUsername(const std::string& token) const;
    void AddSession(const std::string& token,
        const std::string& username);
    std::vector<webapp::Post> GetPosts() const;
    void AddPost(webapp::Post post);

private:
    WebAppServer(
        std::shared_ptr<core::Logger> logger,
        WebAppOptions options);

    void RegisterRoutes();
    void Start();
    void OnAccepted(
        platform::windows::SocketHandle socket) noexcept;

    static std::string ExtractSessionId(
        const protocol::http::HttpRequest& request);

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
    std::shared_ptr<core::TimerService> timer_service_;

    mutable std::mutex state_mutex_;
    std::mutex stop_mutex_;
    enum class State { Created, Running, Stopping, Stopped };
    State state_{State::Created};

    std::shared_ptr<AuthState> auth_{
        std::make_shared<AuthState>()};
    mutable std::mutex board_mutex_;
    std::vector<webapp::Post> posts_;
    std::atomic<std::uint64_t> next_post_id_{1};
};

} // namespace iocp::server
