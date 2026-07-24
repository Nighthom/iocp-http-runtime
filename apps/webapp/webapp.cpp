// 간단한 게시판 + 로그인 웹 애플리케이션 구현
#include "webapp/webapp.h"

#include "execution/serial_executor.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace iocp::server
{

namespace
{

constexpr const char kSessionCookie[] = "session_id";

protocol::http::HttpResponse Redirect(const std::string& path)
{
    protocol::http::HttpResponse response;
    response.status_code = 302;
    response.headers.push_back({"Location", path});
    return response;
}

std::string ExtractedSessionId(const protocol::http::HttpRequest& request)
{
    const auto cookie = request.Header("cookie");
    if (!cookie) return {};

    const auto pos = cookie->find(std::string(kSessionCookie) + "=");
    if (pos == std::string::npos) return {};

    const auto start = pos + std::strlen(kSessionCookie) + 1;
    const auto end = cookie->find(';', start);
    if (end == std::string::npos)
    {
        return std::string(cookie->substr(start));
    }
    return std::string(cookie->substr(start, end - start));
}

} // namespace

std::unique_ptr<WebAppServer> WebAppServer::Create(
    std::shared_ptr<core::Logger> logger,
    WebAppOptions options)
{
    auto server = std::unique_ptr<WebAppServer>(
        new WebAppServer(std::move(logger), std::move(options)));
    server->RegisterRoutes();
    server->Start();
    return server;
}

WebAppServer::WebAppServer(
    std::shared_ptr<core::Logger> logger,
    WebAppOptions options)
    : logger_(std::move(logger))
    , options_(std::move(options))
    , winsock_(logger_)
    , io_context_(options_.io_worker_count, logger_)
    , application_context_(
          std::make_shared<execution::ThreadPoolContext>(
              options_.application_worker_count,
              options_.maximum_application_tasks))
    , application_executor_(
          std::make_shared<execution::ThreadPoolExecutor>(
              application_context_))
    , registry_(
          std::make_shared<transport::ConnectionRegistry>())
    , router_(
          std::make_shared<protocol::http::HttpRouter>())
{
}

WebAppServer::~WebAppServer()
{
    Stop();
}

bool WebAppServer::Stop()
{
    std::lock_guard lock(stop_mutex_);
    if (state_ == State::Stopped) return true;

    state_ = State::Stopping;

    if (listener_)
    {
        listener_->Stop();
        listener_->WaitStopped(
            options_.shutdown_timeout);
    }

    if (registry_)
    {
        for (const auto& conn : registry_->Connections())
        {
            conn->BeginClose(
                transport::CloseReason::LocalShutdown);
        }
        registry_->WaitEmpty(
            options_.shutdown_timeout);
    }

    application_context_->Stop(
        execution::StopMode::Drain);
    application_context_->WaitStopped(
        options_.shutdown_timeout);
    application_context_->Join();

    io_context_.Stop();
    io_context_.Join();

    state_ = State::Stopped;
    return true;
}

std::uint16_t WebAppServer::LocalPort() const
{
    return listener_ ? listener_->Snapshot().local_port : 0;
}

void WebAppServer::RegisterRoutes()
{
    auto& router = *router_;

    // 로그인 페이지
    router.Register(
        protocol::http::HttpMethod::Get,
        "/login",
        [this](const protocol::http::HttpRequest& request) {
            const auto session = ExtractedSessionId(request);
            if (!session.empty() && ValidateSession(session))
            {
                return Redirect("/board");
            }
            return HtmlResponse(200, RenderLoginPage());
        });

    router.Register(
        protocol::http::HttpMethod::Post,
        "/login",
        [this](const protocol::http::HttpRequest& request) {
            const auto params = request.QueryParams();
            // Read from body for POST form
            const auto body =
                protocol::http::StringFromBytes(request.body);
            // Parse simple form body
            std::string username;
            std::string password;
            auto amp = body.find('&');
            if (amp != std::string::npos)
            {
                auto eq = body.find('=');
                if (eq < amp)
                {
                    username = body.substr(eq + 1, amp - eq - 1);
                }
                eq = body.find('=', amp);
                if (eq != std::string::npos)
                {
                    password =
                        body.substr(eq + 1);
                }
            }

            // Simple URL-decode for form values
            auto decode = [](std::string& s) {
                for (auto& c : s)
                {
                    if (c == '+') c = ' ';
                }
            };
            decode(username);
            decode(password);

            std::lock_guard lock(auth_mutex_);
            const auto found = users_.find(username);
            if (found == users_.end() || found->second != password)
            {
                auto resp = HtmlResponse(
                    200, RenderLoginPage("잘못된 아이디 또는 비밀번호입니다."));
                return resp;
            }

            const std::string token = GenerateSessionToken();
            sessions_[token] = username;

            auto resp = Redirect("/board");
            resp.headers.push_back(
                {"Set-Cookie",
                 "session_id=" + token +
                 "; Path=/; HttpOnly; Max-Age=3600"});
            return resp;
        });

    // 로그아웃
    router.Register(
        protocol::http::HttpMethod::Get,
        "/logout",
        [this](const protocol::http::HttpRequest& request) {
            const auto session = ExtractedSessionId(request);
            if (!session.empty())
            {
                std::lock_guard lock(auth_mutex_);
                sessions_.erase(session);
            }
            return Redirect("/login");
        });

    // 게시판 목록
    router.Register(
        protocol::http::HttpMethod::Get,
        "/board",
        [this](const protocol::http::HttpRequest& request) {
            const auto session = ExtractedSessionId(request);
            if (session.empty() || !ValidateSession(session))
            {
                return Redirect("/login");
            }

            std::lock_guard lock(board_mutex_);
            std::vector<Post> posts = posts_;
            std::reverse(posts.begin(), posts.end());
            // 최근 50개까지만
            if (posts.size() > 50)
            {
                posts.resize(50);
            }

            const auto username = GetUsername(session);
            return HtmlResponse(
                200, RenderBoardPage(posts, username));
        });

    // 글쓰기 폼
    router.Register(
        protocol::http::HttpMethod::Get,
        "/write",
        [this](const protocol::http::HttpRequest& request) {
            const auto session = ExtractedSessionId(request);
            if (session.empty() || !ValidateSession(session))
            {
                return Redirect("/login");
            }
            const auto username = GetUsername(session);
            return HtmlResponse(
                200, RenderWriteForm(username));
        });

    // 글쓰기
    router.Register(
        protocol::http::HttpMethod::Post,
        "/write",
        [this](const protocol::http::HttpRequest& request) {
            const auto session = ExtractedSessionId(request);
            if (session.empty() || !ValidateSession(session))
            {
                return Redirect("/login");
            }

            const auto body =
                protocol::http::StringFromBytes(request.body);

            std::string title;
            std::string content;

            // Parse form body (title=...&content=...)
            auto decode = [](std::string& s) {
                for (auto& c : s)
                {
                    if (c == '+') c = ' ';
                }
                // very basic percent-decode
                for (std::size_t i = 0; i + 2 < s.size(); ++i)
                {
                    if (s[i] == '%')
                    {
                        const auto hex =
                            std::string_view(s).substr(i + 1, 2);
                        char decoded = 0;
                        for (auto h : hex)
                        {
                            decoded <<= 4;
                            if (h >= '0' && h <= '9')
                                decoded |= h - '0';
                            else if (h >= 'a' && h <= 'f')
                                decoded |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F')
                                decoded |= h - 'A' + 10;
                        }
                        s[i] = decoded;
                        s.erase(i + 1, 2);
                    }
                }
            };

            const auto t_eq = body.find("title=");
            const auto c_eq = body.find("content=");
            if (t_eq != std::string::npos)
            {
                const auto start = t_eq + 6;
                const auto end = body.find('&', start);
                title = end == std::string::npos
                    ? body.substr(start)
                    : body.substr(start, end - start);
            }
            if (c_eq != std::string::npos)
            {
                const auto start = c_eq + 8;
                content = body.substr(start);
            }

            decode(title);
            decode(content);

            if (title.empty() || content.empty())
            {
                const auto username = GetUsername(session);
                auto resp = HtmlResponse(
                    200, RenderWriteForm(username));
                return resp;
            }

            Post post;
            post.id = next_post_id_++;
            post.title = HtmlEscape(title);
            post.author = GetUsername(session);
            post.content = HtmlEscape(content);
            post.created_at =
                std::chrono::system_clock::now();

            {
                std::lock_guard lock(board_mutex_);
                posts_.push_back(std::move(post));
            }

            return Redirect("/board");
        });

    // 게시글 보기
    router.Register(
        protocol::http::HttpMethod::Get,
        "/post",
        [this](const protocol::http::HttpRequest& request) {
            const auto session = ExtractedSessionId(request);
            if (session.empty() || !ValidateSession(session))
            {
                return Redirect("/login");
            }

            const auto params = request.QueryParams();
            const auto id_iter = params.find("id");
            if (id_iter == params.end())
            {
                return Redirect("/board");
            }

            const auto id =
                std::stoull(id_iter->second);

            std::lock_guard lock(board_mutex_);
            for (const auto& post : posts_)
            {
                if (post.id == id)
                {
                    return HtmlResponse(
                        200, RenderPostDetail(post));
                }
            }
            return Redirect("/board");
        });

    // 정적 CSS
    router.Register(
        protocol::http::HttpMethod::Get,
        "/style.css",
        [](const protocol::http::HttpRequest&) {
            return protocol::http::MakeTextResponse(
                200, RenderStyles(),
                "text/css; charset=utf-8");
        });

    // 루트 리디렉션
    router.Register(
        protocol::http::HttpMethod::Get,
        "/",
        [](const protocol::http::HttpRequest&) {
            return Redirect("/login");
        });
}

void WebAppServer::Start()
{
    state_ = State::Running;

    listener_ = transport::TcpListener::Create(
        io_context_,
        logger_,
        options_.listener,
        [this](platform::windows::SocketHandle socket) {
            OnAccepted(std::move(socket));
        });
}

void WebAppServer::OnAccepted(
    platform::windows::SocketHandle socket) noexcept
{
    try
    {
        if (state_ != State::Running) return;

        using transport::TcpConnection;
        auto connection_slot =
            std::make_shared<std::weak_ptr<TcpConnection>>();
        auto serial_executor = execution::SerialExecutor::Create(
            application_executor_,
            options_.maximum_connection_tasks);
        const auto encoder =
            std::make_shared<protocol::http::HttpResponseEncoder>(
                protocol::http::HttpResponseEncoderOptions{});

        auto session =
            std::make_shared<protocol::http::HttpSession>(
                router_,
                serial_executor,
                [connection_slot, encoder](
                    protocol::http::HttpResponse response) {
                    const auto connection = connection_slot->lock();
                    if (!connection) return;

                    try
                    {
                        auto encoded = encoder->Encode(
                            std::move(response));
                        TcpConnection::OutboundBatch batch;
                        batch.reserve(2);
                        batch.push_back(std::move(encoded.head));
                        if (!encoded.body.empty())
                        {
                            batch.push_back(
                                std::move(encoded.body));
                        }

                        const auto status =
                            encoded.close_connection
                            ? connection->SendBatchAndClose(
                                  std::move(batch))
                            : connection->SendBatch(
                                  std::move(batch));
                        if (status ==
                                transport::SendStatus::StartFailed ||
                            status ==
                                transport::SendStatus::QueueOverflow)
                        {
                            connection->BeginClose(
                                transport::CloseReason::SendError);
                        }
                    }
                    catch (...)
                    {
                        connection->BeginClose(
                            transport::CloseReason::HandlerError);
                    }
                });

        auto connection = TcpConnection::Create(
            registry_->NextId(),
            std::move(socket),
            registry_,
            logger_,
            [session](
                const std::shared_ptr<TcpConnection>& connection,
                const buffer::ByteView bytes) {
                const auto result = session->Feed(bytes);
                if (result.status !=
                    protocol::ProtocolFeedStatus::Ready)
                {
                    connection->BeginClose(
                        transport::CloseReason::HandlerError);
                }
            },
            options_.connection);
        *connection_slot = connection;

        registry_->Add(connection);
        connection->Start();
    }
    catch (const std::exception& exception)
    {
        logger_->Log(
            core::LogLevel::Error,
            "webapp.connection_create_failed",
            "webapp connection 생성 실패",
            {{"exception", exception.what()}});
    }
    catch (...)
    {
        logger_->Log(
            core::LogLevel::Error,
            "webapp.connection_create_failed",
            "webapp connection 생성 중 알 수 없는 오류");
    }
}

std::string WebAppServer::GenerateSessionToken() const
{
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<
        std::uint64_t> dist;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(16) << dist(gen);
    return oss.str();
}

bool WebAppServer::ValidateSession(
    const std::string& token) const
{
    std::lock_guard lock(auth_mutex_);
    return sessions_.find(token) != sessions_.end();
}

std::string WebAppServer::GetUsername(
    const std::string& token) const
{
    std::lock_guard lock(auth_mutex_);
    const auto found = sessions_.find(token);
    return found != sessions_.end() ? found->second : "";
}

std::string WebAppServer::HtmlEscape(
    const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto c : value)
    {
        switch (c)
        {
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '&': result += "&amp;"; break;
        case '"': result += "&quot;"; break;
        default: result += c; break;
        }
    }
    return result;
}

protocol::http::HttpResponse WebAppServer::HtmlResponse(
    const std::uint16_t status_code,
    const std::string& html)
{
    return protocol::http::MakeTextResponse(
        status_code, html,
        "text/html; charset=utf-8");
}

std::string WebAppServer::RenderLoginPage(
    const std::string& error)
{
    std::ostringstream html;
    html << "<!DOCTYPE html><html lang=\"ko\"><head>"
         << "<meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width\">"
         << "<title>로그인 - IOCP 게시판</title>"
         << "<link rel=\"stylesheet\" href=\"/style.css\">"
         << "</head><body>"
         << "<div class=\"container\">"
         << "<h1>IOCP 게시판</h1>"
         << "<div class=\"card\">"
         << "<h2>로그인</h2>";

    if (!error.empty())
    {
        html << "<p class=\"error\">" << error << "</p>";
    }

    html << "<form method=\"post\" action=\"/login\">"
         << "<div class=\"form-group\">"
         << "<label for=\"username\">아이디</label>"
         << "<input type=\"text\" id=\"username\" name=\"username\" required>"
         << "</div>"
         << "<div class=\"form-group\">"
         << "<label for=\"password\">비밀번호</label>"
         << "<input type=\"password\" id=\"password\" name=\"password\" required>"
         << "</div>"
         << "<button type=\"submit\" class=\"btn\">로그인</button>"
         << "</form>"
         << "<p class=\"hint\">테스트 계정: admin / admin123  또는  user / pass123</p>"
         << "</div></div></body></html>";

    return html.str();
}

std::string WebAppServer::RenderBoardPage(
    const std::vector<Post>& posts,
    const std::string& username)
{
    std::ostringstream html;
    html << "<!DOCTYPE html><html lang=\"ko\"><head>"
         << "<meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width\">"
         << "<title>게시판 - IOCP</title>"
         << "<link rel=\"stylesheet\" href=\"/style.css\">"
         << "</head><body>"
         << "<div class=\"container\">"
         << "<div class=\"header\">"
         << "<h1>IOCP 게시판</h1>"
         << "<div class=\"user-info\">"
         << "<span>" << HtmlEscape(username) << "님</span>"
         << "<a href=\"/logout\" class=\"btn-sm\">로그아웃</a>"
         << "</div>"
         << "</div>"
         << "<div class=\"toolbar\">"
         << "<a href=\"/write\" class=\"btn\">글쓰기</a>"
         << "</div>";

    if (posts.empty())
    {
        html << "<div class=\"card\"><p>게시글이 없습니다. 첫 게시글을 작성해보세요!</p></div>";
    }
    else
    {
        html << "<table class=\"post-list\">"
             << "<thead><tr>"
             << "<th>번호</th><th>제목</th><th>작성자</th><th>날짜</th>"
             << "</tr></thead><tbody>";

        for (const auto& post : posts)
        {
            const auto t = std::chrono::system_clock::to_time_t(
                post.created_at);
            std::tm tm;
            char date_buf[32];
            localtime_s(&tm, &t);
            std::strftime(date_buf, sizeof(date_buf),
                "%Y-%m-%d %H:%M", &tm);

            html << "<tr>"
                 << "<td>" << post.id << "</td>"
                 << "<td><a href=\"/post?id=" << post.id
                 << "\">" << post.title << "</a></td>"
                 << "<td>" << post.author << "</td>"
                 << "<td>" << date_buf << "</td>"
                 << "</tr>";
        }

        html << "</tbody></table>";
    }

    html << "</div></body></html>";
    return html.str();
}

std::string WebAppServer::RenderPostDetail(const Post& post)
{
    const auto t = std::chrono::system_clock::to_time_t(
        post.created_at);
    std::tm tm;
    char date_buf[32];
    localtime_s(&tm, &t);
    std::strftime(date_buf, sizeof(date_buf),
        "%Y-%m-%d %H:%M:%S", &tm);

    std::ostringstream html;
    html << "<!DOCTYPE html><html lang=\"ko\"><head>"
         << "<meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width\">"
         << "<title>" << post.title << " - IOCP 게시판</title>"
         << "<link rel=\"stylesheet\" href=\"/style.css\">"
         << "</head><body>"
         << "<div class=\"container\">"
         << "<div class=\"card\">"
         << "<h2>" << post.title << "</h2>"
         << "<div class=\"post-meta\">"
         << "<span>작성자: " << post.author << "</span>"
         << "<span>작성일: " << date_buf << "</span>"
         << "</div>"
         << "<div class=\"post-content\">"
         << post.content
         << "</div>"
         << "<div class=\"post-actions\">"
         << "<a href=\"/board\" class=\"btn\">목록으로</a>"
         << "</div>"
         << "</div></div></body></html>";
    return html.str();
}

std::string WebAppServer::RenderWriteForm(
    const std::string& /*username*/)
{
    std::ostringstream html;
    html << "<!DOCTYPE html><html lang=\"ko\"><head>"
         << "<meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width\">"
         << "<title>글쓰기 - IOCP 게시판</title>"
         << "<link rel=\"stylesheet\" href=\"/style.css\">"
         << "</head><body>"
         << "<div class=\"container\">"
         << "<div class=\"card\">"
         << "<h2>글쓰기</h2>"
         << "<form method=\"post\" action=\"/write\">"
         << "<div class=\"form-group\">"
         << "<label for=\"title\">제목</label>"
         << "<input type=\"text\" id=\"title\" name=\"title\" required>"
         << "</div>"
         << "<div class=\"form-group\">"
         << "<label for=\"content\">내용</label>"
         << "<textarea id=\"content\" name=\"content\" rows=\"10\" required></textarea>"
         << "</div>"
         << "<div class=\"form-actions\">"
         << "<a href=\"/board\" class=\"btn-secondary\">취소</a>"
         << "<button type=\"submit\" class=\"btn\">작성</button>"
         << "</div>"
         << "</form>"
         << "</div></div></body></html>";
    return html.str();
}

std::string WebAppServer::RenderStyles()
{
    return R"css(
* { box-sizing: border-box; margin: 0; padding: 0; }

body {
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    background: #f0f2f5;
    color: #1a1a2e;
    line-height: 1.6;
    min-height: 100vh;
}

.container {
    max-width: 900px;
    margin: 0 auto;
    padding: 2rem 1rem;
}

h1 {
    font-size: 1.8rem;
    color: #16213e;
    margin-bottom: 1rem;
}

h2 {
    font-size: 1.4rem;
    color: #0f3460;
    margin-bottom: 1rem;
}

.card {
    background: white;
    border-radius: 12px;
    padding: 2rem;
    box-shadow: 0 2px 8px rgba(0,0,0,0.08);
    margin-bottom: 1rem;
}

.header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 1.5rem;
}

.user-info {
    display: flex;
    align-items: center;
    gap: 1rem;
    color: #555;
}

.form-group {
    margin-bottom: 1rem;
}

.form-group label {
    display: block;
    margin-bottom: 0.25rem;
    font-weight: 600;
    color: #333;
    font-size: 0.9rem;
}

.form-group input,
.form-group textarea {
    width: 100%;
    padding: 0.6rem 0.8rem;
    border: 1.5px solid #ddd;
    border-radius: 8px;
    font-size: 0.95rem;
    transition: border-color 0.2s;
    font-family: inherit;
}

.form-group input:focus,
.form-group textarea:focus {
    outline: none;
    border-color: #0f3460;
}

.btn {
    display: inline-block;
    padding: 0.6rem 1.4rem;
    background: #0f3460;
    color: white;
    border: none;
    border-radius: 8px;
    cursor: pointer;
    font-size: 0.95rem;
    text-decoration: none;
    font-weight: 500;
    transition: background 0.2s;
}

.btn:hover { background: #1a4a8a; }

.btn-sm {
    padding: 0.3rem 0.8rem;
    background: #eee;
    color: #333;
    border-radius: 6px;
    text-decoration: none;
    font-size: 0.85rem;
    transition: background 0.2s;
}

.btn-sm:hover { background: #ddd; }

.btn-secondary {
    padding: 0.6rem 1.4rem;
    background: #e0e0e0;
    color: #333;
    border-radius: 8px;
    text-decoration: none;
    font-size: 0.95rem;
    transition: background 0.2s;
}

.btn-secondary:hover { background: #ccc; }

.form-actions {
    display: flex;
    gap: 0.5rem;
    justify-content: flex-end;
    margin-top: 1rem;
}

.toolbar {
    margin-bottom: 1rem;
}

.error {
    background: #fef2f2;
    color: #dc2626;
    padding: 0.8rem;
    border-radius: 8px;
    margin-bottom: 1rem;
}

.hint {
    margin-top: 1rem;
    font-size: 0.85rem;
    color: #888;
}

.post-list {
    width: 100%;
    border-collapse: collapse;
    background: white;
    border-radius: 12px;
    overflow: hidden;
    box-shadow: 0 2px 8px rgba(0,0,0,0.08);
}

.post-list th {
    background: #0f3460;
    color: white;
    padding: 0.8rem;
    text-align: left;
    font-weight: 500;
}

.post-list td {
    padding: 0.8rem;
    border-bottom: 1px solid #eee;
}

.post-list a {
    color: #0f3460;
    text-decoration: none;
    font-weight: 500;
}

.post-list a:hover { text-decoration: underline; }

.post-list tr:hover td { background: #f8f9fa; }

.post-meta {
    display: flex;
    gap: 2rem;
    color: #888;
    font-size: 0.85rem;
    margin-bottom: 1.5rem;
    padding-bottom: 1rem;
    border-bottom: 1px solid #eee;
}

.post-content {
    white-space: pre-wrap;
    line-height: 1.8;
    margin-bottom: 1.5rem;
}

.post-actions {
    text-align: right;
    padding-top: 1rem;
    border-top: 1px solid #eee;
}
)css";
}

} // namespace iocp::server
