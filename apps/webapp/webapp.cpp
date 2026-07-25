// 게시판 + 로그인 웹 애플리케이션 서버 구현
// state 관리는 WebAppServer, HTML 렌더링은 board_handlers에 위임

#include "webapp/webapp.h"
#include "webapp/board_handlers.h"

#include "core/json_utils.h"

#include "execution/serial_executor.h"

#include <algorithm>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace iocp::server
{

namespace
{

protocol::http::HttpResponse Redirect(const std::string& path)
{
    protocol::http::HttpResponse response;
    response.status_code = 302;
    response.headers.push_back({"Location", path});
    return response;
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
    , io_context_(2, logger_)
    , application_context_(
          std::make_shared<execution::ThreadPoolContext>(2, 1024))
    , application_executor_(
          std::make_shared<execution::ThreadPoolExecutor>(
              application_context_))
    , registry_(
          std::make_shared<transport::ConnectionRegistry>())
    , router_(
          std::make_shared<protocol::http::HttpRouter>())
    , timer_service_(
          std::make_shared<core::TimerService>())
{
    webapp::SetHomeDirectory(options_.home_directory);
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
        listener_->WaitStopped(options_.shutdown_timeout);
    }

    if (registry_)
    {
        for (const auto& conn : registry_->Connections())
        {
            conn->BeginClose(
                transport::CloseReason::LocalShutdown);
        }
        registry_->WaitEmpty(options_.shutdown_timeout);
    }

    application_context_->Stop(execution::StopMode::Drain);
    application_context_->WaitStopped(options_.shutdown_timeout);
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
    using namespace protocol::http;
    auto& router = *router_;

    // GET /login
    router.Register(
        HttpMethod::Get, "/login",
        [this](const HttpRequest& request) {
            const auto session = ExtractSessionId(request);
            if (!session.empty() && ValidateSession(session))
            {
                return Redirect("/board");
            }
            return webapp::HandleLoginPage();
        });

    // POST /login
    router.Register(
        HttpMethod::Post, "/login",
        [this](const HttpRequest& request) {
            const auto body = StringFromBytes(request.body);
            std::string username;
            std::string password;

            auto amp = body.find('&');
            if (amp != std::string::npos)
            {
                auto eq = body.find('=');
                if (eq < amp) username = body.substr(eq + 1, amp - eq - 1);
                eq = body.find('=', amp);
                if (eq != std::string::npos) password = body.substr(eq + 1);
            }

            for (auto& s : {&username, &password})
                for (auto& c : *s) if (c == '+') c = ' ';

            const auto user = webapp::TryLogin(username, password);
            if (user.empty())
            {
                return webapp::HandleLoginPage(
                    "잘못된 아이디 또는 비밀번호입니다.");
            }

            const std::string token = GenerateSessionToken();
            AddSession(token, user);
            auto resp = Redirect("/board");
            resp.headers.push_back(
                {"Set-Cookie",
                 "session_id=" + token +
                 "; Path=/; HttpOnly; Max-Age=3600"});
            return resp;
        });

    // GET /logout
    router.Register(
        HttpMethod::Get, "/logout",
        [this](const HttpRequest& request) {
            const auto session = ExtractSessionId(request);
            if (!session.empty())
            {
                std::lock_guard lock(auth_->mutex);
                auth_->sessions.erase(session);
            }
            return Redirect("/login");
        });

    // GET /board
    router.Register(
        HttpMethod::Get, "/board",
        [this](const HttpRequest& request) {
            const auto session = ExtractSessionId(request);
            if (session.empty() || !ValidateSession(session))
                return Redirect("/login");

            const auto posts = GetPosts();
            return webapp::HandleBoardPage(
                GetUsername(session), posts);
        });

    // GET /write
    router.Register(
        HttpMethod::Get, "/write",
        [this](const HttpRequest& request) {
            const auto session = ExtractSessionId(request);
            if (session.empty() || !ValidateSession(session))
                return Redirect("/login");
            return webapp::HandleWriteForm();
        });

    // POST /write
    router.Register(
        HttpMethod::Post, "/write",
        [this](const HttpRequest& request) {
            const auto session = ExtractSessionId(request);
            if (session.empty() || !ValidateSession(session))
                return Redirect("/login");

            const auto body = StringFromBytes(request.body);
            std::string title, content;

            auto t_eq = body.find("title=");
            if (t_eq != std::string::npos)
            {
                auto start = t_eq + 6;
                auto end = body.find('&', start);
                title = end == std::string::npos
                    ? body.substr(start)
                    : body.substr(start, end - start);
            }

            auto c_eq = body.find("content=");
            if (c_eq != std::string::npos)
            {
                content = body.substr(c_eq + 8);
            }

            for (auto& s : {&title, &content})
                for (auto& c : *s) if (c == '+') c = ' ';

            // Basic percent decode
            for (auto* s : {&title, &content})
            {
                for (std::size_t i = 0; i + 2 < s->size(); ++i)
                {
                    if ((*s)[i] == '%')
                    {
                        char decoded = 0;
                        for (int j = 0; j < 2; ++j)
                        {
                            auto h = (*s)[i + 1 + j];
                            decoded <<= 4;
                            if (h >= '0' && h <= '9') decoded |= h - '0';
                            else if (h >= 'a' && h <= 'f') decoded |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') decoded |= h - 'A' + 10;
                        }
                        (*s)[i] = decoded;
                        s->erase(i + 1, 2);
                    }
                }
            }

            if (title.empty() || content.empty())
            {
                return webapp::HandleWriteForm();
            }

            auto post = webapp::CreatePost(
                title, content, GetUsername(session));
            AddPost(std::move(post));
            return Redirect("/board");
        });

    // GET /post
    router.Register(
        HttpMethod::Get, "/post",
        [this](const HttpRequest& request) {
            const auto session = ExtractSessionId(request);
            if (session.empty() || !ValidateSession(session))
                return Redirect("/login");

            const auto params = request.QueryParams();
            const auto it = params.find("id");
            if (it == params.end()) return Redirect("/board");

            const auto id = std::stoull(it->second);
            const auto posts = GetPosts();
            for (const auto& post : posts)
            {
                if (post.id == id)
                    return webapp::HandlePostDetail(post);
            }
            return Redirect("/board");
        });

    // GET /style.css
    router.Register(
        HttpMethod::Get, "/style.css",
        [](const HttpRequest&) {
            return webapp::HandleStyles();
        });

    // --- API routes ---

    // GET /api/posts — 게시글 목록 JSON
    router.Register(
        HttpMethod::Get, "/api/posts",
        [this](const HttpRequest& /*request*/) {
            const auto posts = GetPosts();
            std::string json = "[";
            for (std::size_t i = 0; i < posts.size(); ++i)
            {
                if (i > 0) json += ",";
                json += core::JsonValue::Format({
                    {"id", std::to_string(posts[i].id)},
                    {"title", posts[i].title},
                    {"author", posts[i].author},
                });
            }
            json += "]";
            return MakeTextResponse(200, json,
                "application/json; charset=utf-8");
        });

    // POST /api/posts — 게시글 작성 (JSON body)
    router.Register(
        HttpMethod::Post, "/api/posts",
        [this](const HttpRequest& request) {
            const auto body = StringFromBytes(request.body);
            auto j = core::JsonValue::Parse(body);
            auto title = j.GetString("title");
            auto content = j.GetString("content");
            auto author = j.GetString("author", "anonymous");

            if (title.empty() || content.empty())
            {
                return MakeTextResponse(400,
                    core::JsonValue::Format({
                        {"error", "title and content required"}}),
                    "application/json; charset=utf-8");
            }

            auto post = webapp::CreatePost(title, content, author);
            AddPost(std::move(post));
            return MakeTextResponse(201,
                core::JsonValue::Format({{"status", "created"}}),
                "application/json; charset=utf-8");
        });

    // GET /api/stream — chunked streaming demo
    router.Register(
        HttpMethod::Get, "/api/stream",
        [](const HttpRequest&) {
            // pre-encoded chunked response
            std::string body;
            static const char* words[] = {"Hello ", "IOCP ", "streaming ", "demo!\n"};
            for (const auto& w : words)
            {
                body += (std::ostringstream{} << std::hex << std::strlen(w) << "\r\n" << w << "\r\n").str();
            }
            body += "0\r\n\r\n";

            HttpResponse resp;
            resp.headers.push_back({"Transfer-Encoding", "chunked"});
            resp.headers.push_back({"Content-Type", "text/plain; charset=utf-8"});
            resp.body = BytesFromString(body);
            return resp;
        });

    // GET /
    router.Register(
        HttpMethod::Get, "/",
        [](const HttpRequest&) {
            return Redirect("/login");
        });
}

void WebAppServer::Start()
{
    state_ = State::Running;

    listener_ = transport::TcpListener::Create(
        io_context_, logger_, options_.listener,
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
            application_executor_, 128);
        const auto encoder =
            std::make_shared<protocol::http::HttpResponseEncoder>(
                protocol::http::HttpResponseEncoderOptions{});

        auto session =
            std::make_shared<protocol::http::HttpSession>(
                router_, serial_executor,
                [connection_slot, encoder](
                    protocol::http::HttpResponse response) {
                    const auto connection = connection_slot->lock();
                    if (!connection) return;
                    try
                    {
                        auto encoded = encoder->Encode(std::move(response));
                        TcpConnection::OutboundBatch batch;
                        batch.reserve(2);
                        batch.push_back(std::move(encoded.head));
                        if (!encoded.body.empty())
                            batch.push_back(std::move(encoded.body));
                        const auto status =
                            encoded.close_connection
                            ? connection->SendBatchAndClose(std::move(batch))
                            : connection->SendBatch(std::move(batch));
                        if (status == transport::SendStatus::StartFailed ||
                            status == transport::SendStatus::QueueOverflow)
                            connection->BeginClose(
                                transport::CloseReason::SendError);
                    }
                    catch (...)
                    {
                        connection->BeginClose(
                            transport::CloseReason::HandlerError);
                    }
                });

        auto connection = TcpConnection::Create(
            registry_->NextId(), std::move(socket), registry_,
            logger_,
            [session](const std::shared_ptr<TcpConnection>& connection,
                       const buffer::ByteView bytes) {
                const auto result = session->Feed(bytes);
                if (result.status !=
                    protocol::ProtocolFeedStatus::Ready)
                    connection->BeginClose(
                        transport::CloseReason::HandlerError);
            },
            transport::ConnectionOptions{});
        *connection_slot = connection;

        registry_->Add(connection);
        connection->Start();
    }
    catch (const std::exception& exception)
    {
        logger_->Log(core::LogLevel::Error,
            "webapp.connection_create_failed",
            "webapp connection 생성 실패",
            {{"exception", exception.what()}});
    }
    catch (...) {}
}

std::string WebAppServer::ExtractSessionId(
    const protocol::http::HttpRequest& request)
{
    const auto cookie = request.Header("cookie");
    if (!cookie) return {};

    constexpr auto kCookie = "session_id=";
    const auto pos = cookie->find(kCookie);
    if (pos == std::string::npos) return {};

    const auto start = pos + std::char_traits<char>::length(kCookie);
    const auto end = cookie->find(';', start);
    return end == std::string::npos
        ? std::string(cookie->substr(start))
        : std::string(cookie->substr(start, end - start));
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
    std::lock_guard lock(auth_->mutex);
    return auth_->sessions.find(token) != auth_->sessions.end();
}

std::string WebAppServer::GetUsername(
    const std::string& token) const
{
    std::lock_guard lock(auth_->mutex);
    const auto found = auth_->sessions.find(token);
    return found != auth_->sessions.end() ? found->second : "";
}

void WebAppServer::AddSession(
    const std::string& token,
    const std::string& username)
{
    std::lock_guard lock(auth_->mutex);
    auth_->sessions[token] = username;

    // 1시간 후 session 자동 만료
    auto auth_copy = auth_;
    (void)timer_service_->Schedule(
        std::chrono::hours{1},
        [auth_copy, token_copy = token] {
            std::lock_guard lock(auth_copy->mutex);
            auth_copy->sessions.erase(token_copy);
        });
}

std::vector<webapp::Post> WebAppServer::GetPosts() const
{
    std::lock_guard lock(board_mutex_);
    auto posts = posts_;
    std::reverse(posts.begin(), posts.end());
    if (posts.size() > 50) posts.resize(50);
    return posts;
}

void WebAppServer::AddPost(webapp::Post post)
{
    std::lock_guard lock(board_mutex_);
    posts_.push_back(std::move(post));
}

} // namespace iocp::server
