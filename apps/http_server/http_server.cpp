// HTTP 서버 composition root: SerialExecutor→HttpSession→TcpConnection 조립,
// response_sender closure, shutdown barrier 순서를 관리한다.

#include "http_server/http_server.h"

#include "execution/serial_executor.h"
#include "protocol/http2/http2_frames.h"
#include "protocol/http2/http2_hpack.h"
#include "protocol/http2/http2_stream.h"

#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace iocp::server
{

namespace
{

std::chrono::milliseconds Remaining(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
    {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
}

} // namespace

HttpServerOptions::HttpServerOptions()
{
    connection.maximum_send_queue_bytes = 2 * 1024 * 1024;
}

std::unique_ptr<HttpServer> HttpServer::Create(
    std::shared_ptr<core::Logger> logger,
    HttpServerOptions options)
{
    auto server = std::unique_ptr<HttpServer>(
        new HttpServer(std::move(logger), std::move(options)));
    server->RegisterRoutes();
    server->Start();
    return server;
}

HttpServer::HttpServer(
    std::shared_ptr<core::Logger> logger,
    HttpServerOptions options)
    : logger_(std::move(logger)),
      options_(std::move(options)),
      winsock_(logger_),
      io_context_(options_.io_worker_count, logger_),
      application_context_(
          std::make_shared<execution::ThreadPoolContext>(
              options_.application_worker_count,
              options_.maximum_application_tasks)),
      application_executor_(
          std::make_shared<execution::ThreadPoolExecutor>(
              application_context_)),
      registry_(
          std::make_shared<transport::ConnectionRegistry>()),
      router_(std::make_shared<protocol::http::HttpRouter>())
{
    if (!logger_)
    {
        throw std::invalid_argument(
            "HttpServer requires a logger");
    }
    if (options_.maximum_connection_tasks == 0)
    {
        throw std::invalid_argument(
            "maximum connection tasks must be positive");
    }
    if (options_.shutdown_timeout <=
        std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument(
            "HttpServer shutdown timeout must be positive");
    }
}

HttpServer::~HttpServer()
{
    if (!Stop())
    {
        logger_->Log(
            core::LogLevel::Critical,
            "http_server.destructor_shutdown_timeout",
            "HTTP server 종료가 모든 barrier를 통과하지 못했습니다.");
        std::terminate();
    }
}

void HttpServer::RegisterRoutes()
{
    router_->Register(
        protocol::http::HttpMethod::Get,
        "/",
        [](const protocol::http::HttpRequest&) {
            return protocol::http::MakeTextResponse(
                200,
                "{\"service\":\"iocp-http\","
                "\"endpoints\":[\"GET /health\",\"POST /echo\"]}\n",
                "application/json; charset=utf-8");
        });

    router_->Register(
        protocol::http::HttpMethod::Get,
        "/health",
        [](const protocol::http::HttpRequest&) {
            return protocol::http::MakeTextResponse(
                200,
                "{\"status\":\"ok\"}\n",
                "application/json; charset=utf-8");
        });

    router_->Register(
        protocol::http::HttpMethod::Post,
        "/echo",
        [](const protocol::http::HttpRequest& request) {
            protocol::http::HttpResponse response;
            response.status_code = 200;
            response.body = request.body;
            response.headers.push_back({
                "Content-Type",
                std::string(
                    request.Header("content-type").value_or(
                        "application/octet-stream")),
            });
            return response;
        });

    // GET /metrics — OpenMetrics-compatible
    static std::atomic<std::uint64_t> req_count{0};
    static std::atomic<std::uint64_t> err_count{0};
    router_->Register(
        protocol::http::HttpMethod::Get,
        "/metrics",
        [](const protocol::http::HttpRequest&) {
            using namespace protocol::http;
            auto metrics = std::string(
                "# HELP iocp_requests_total Total HTTP requests\n"
                "# TYPE iocp_requests_total counter\n"
                "iocp_requests_total ") + std::to_string(req_count.load()) + "\n"
                "# HELP iocp_errors_total Total HTTP errors\n"
                "# TYPE iocp_errors_total counter\n"
                "iocp_errors_total " + std::to_string(err_count.load()) + "\n";
            return MakeTextResponse(200, metrics,
                "text/plain; charset=utf-8");
        });
}

void HttpServer::Start()
{
    {
        std::lock_guard lock(state_mutex_);
        if (state_ != HttpServerState::Created)
        {
            throw std::logic_error(
                "HttpServer can only be started once");
        }
        state_ = HttpServerState::Running;
    }

    try
    {
        listener_ = transport::TcpListener::Create(
            io_context_,
            logger_,
            options_.listener,
            [this](platform::windows::SocketHandle socket) {
                OnAccepted(std::move(socket));
            });
    }
    catch (...)
    {
        {
            std::lock_guard lock(state_mutex_);
            state_ = HttpServerState::Stopping;
        }
        application_context_->Stop(
            execution::StopMode::CancelPending);
        application_context_->Join();
        io_context_.Stop();
        io_context_.Join();
        {
            std::lock_guard lock(state_mutex_);
            state_ = HttpServerState::Stopped;
        }
        throw;
    }

    const std::string port_text = std::to_string(LocalPort());
    logger_->Log(
        core::LogLevel::Info,
        "http_server.started",
        "HTTP/1.1 server를 시작했습니다.",
        {{"port", port_text}});
}

void HttpServer::OnAccepted(
    platform::windows::SocketHandle socket) noexcept
{
    try
    {
        std::lock_guard lock(state_mutex_);
        if (state_ != HttpServerState::Running)
        {
            return;
        }

        using transport::TcpConnection;
        auto connection_slot =
            std::make_shared<std::weak_ptr<TcpConnection>>();
        auto serial_executor = execution::SerialExecutor::Create(
            application_executor_,
            options_.maximum_connection_tasks);
        const auto encoder =
            std::make_shared<protocol::http::HttpResponseEncoder>(
                options_.encoder);

        // h2c 감지: 첫 바이트가 HTTP/2 preface면 HTTP/2 session으로 전환한다.
        auto protocol_state =
            std::make_shared<std::shared_ptr<protocol::IProtocolSession>>();

        const auto connection_id = registry_->NextId();

        auto connection = TcpConnection::Create(
            connection_id,
            std::move(socket),
            registry_,
            logger_,
            [protocol_state,
             router = router_,
             serial_executor,
             connection_slot,
             encoder,
             session_options = options_.session,
             enable_h2 = options_.enable_http2,
             connection_id](
                const std::shared_ptr<TcpConnection>& connection,
                const buffer::ByteView bytes) mutable {
                auto& session = *protocol_state;

                // Protocol detection: check for HTTP/2 preface
                if (!session && enable_h2 &&
                    bytes.Size() >= 8)
                {
                    constexpr std::string_view h2_prefix =
                        "PRI * HT";
                    bool is_h2 = true;
                    for (std::size_t i = 0;
                         i < (std::min)(h2_prefix.size(),
                                        bytes.Size());
                         ++i)
                    {
                        if (static_cast<char>(bytes.Data()[i]) !=
                            h2_prefix[i])
                        {
                            is_h2 = false;
                            break;
                        }
                    }

                    if (is_h2 && bytes.Size() >= 24)
                    {
                        // Verify full preface
                        const auto preface =
                            protocol::http2::FrameCodec::kPreface;
                        bool full_match = true;
                        for (std::size_t i = 0;
                             i < preface.size() && i < bytes.Size();
                             ++i)
                        {
                            if (static_cast<char>(bytes.Data()[i]) !=
                                preface[i])
                            {
                                full_match = false;
                                break;
                            }
                        }
                        if (full_match)
                        {
                            // Create HTTP/2 session
                            auto h2_session =
                                std::make_shared<
                                    protocol::http2::H2Session>(
                                    router,
                                    serial_executor,
                                    [connection_slot](
                                        std::uint32_t /*stream_id*/,
                                        protocol::http::HttpResponse
                                            response) mutable {
                                        const auto conn =
                                            connection_slot->lock();
                                        if (!conn) return;
                                        // H2 response → HEADERS + DATA frames
                                        using namespace protocol::http2;
                                        HpackCodec hpack;
                                        std::vector<protocol::http::HttpHeader> hdrs;
                                        hdrs.push_back({":status", std::to_string(response.status_code)});
                                        for (auto& h : response.headers) hdrs.push_back(std::move(h));
                                        auto hb = hpack.Encode(hdrs);

                                        FrameHeader fh;
                                        fh.type = FrameType::Headers;
                                        fh.stream_id = 1;
                                        fh.flags = static_cast<std::uint8_t>(FrameFlags::EndHeaders);
                                        if (response.body.empty()) fh.flags |= static_cast<std::uint8_t>(FrameFlags::EndStream);
                                        fh.length = static_cast<std::uint32_t>(hb.size());
                                        auto frame = FrameCodec::EncodeHeader(fh);
                                        frame.insert(frame.end(), hb.begin(), hb.end());
                                        transport::TcpConnection::OutboundBatch batch;
                                        batch.push_back(std::move(frame));
                                        if (!response.body.empty())
                                        {
                                            FrameHeader df;
                                            df.type = FrameType::Data;
                                            df.stream_id = 1;
                                            df.flags = static_cast<std::uint8_t>(FrameFlags::EndStream);
                                            df.length = static_cast<std::uint32_t>(response.body.size());
                                            auto dframe = FrameCodec::EncodeHeader(df);
                                            dframe.insert(dframe.end(), response.body.begin(), response.body.end());
                                            batch.push_back(std::move(dframe));
                                        }
                                        conn->SendBatch(std::move(batch));
                                    },
                                    [connection_slot](
                                        std::vector<std::byte>
                                            frame_data) {
                                        const auto conn =
                                            connection_slot->lock();
                                        if (!conn) return;
                                        transport::TcpConnection::
                                            OutboundBatch batch;
                                        batch.push_back(
                                            std::move(frame_data));
                                        conn->SendBatch(
                                            std::move(batch));
                                    },
                                    connection_id);
                            session = std::move(h2_session);
                            const auto preface_sz =
                                protocol::http2::FrameCodec::kPreface.size();
                            const auto result =
                                session->Feed(bytes.SubView(
                                    preface_sz,
                                    bytes.Size() - preface_sz));
                            if (result.status !=
                                protocol::ProtocolFeedStatus::Ready)
                            {
                                connection->BeginClose(
                                    transport::CloseReason::HandlerError);
                            }
                            return;
                        }
                    }
                }

                // Default: HTTP/1.1 session
                if (!session)
                {
                    auto http_session =
                        std::make_shared<
                            protocol::http::HttpSession>(
                            router,
                            serial_executor,
                            [connection_slot, encoder](
                                protocol::http::HttpResponse
                                    response) {
                                const auto connection =
                                    connection_slot->lock();
                                if (!connection) return;

                                try
                                {
                                    auto encoded =
                                        encoder->Encode(
                                            std::move(response));
                                    transport::TcpConnection::
                                        OutboundBatch batch;
                                    batch.reserve(2);
                                    batch.push_back(
                                        std::move(encoded.head));
                                    if (!encoded.body.empty())
                                    {
                                        batch.push_back(
                                            std::move(encoded.body));
                                    }

                                    const transport::SendStatus
                                        status =
                                            encoded.close_connection
                                                ? connection
                                                      ->SendBatchAndClose(
                                                          std::move(
                                                              batch))
                                                : connection
                                                      ->SendBatch(
                                                          std::move(
                                                              batch));
                                    if (status ==
                                            transport::SendStatus::
                                                StartFailed ||
                                        status ==
                                            transport::SendStatus::
                                                QueueOverflow)
                                    {
                                        connection->BeginClose(
                                            transport::CloseReason::
                                                SendError);
                                    }
                                }
                                catch (...)
                                {
                                    connection->BeginClose(
                                        transport::CloseReason::
                                            HandlerError);
                                }
                            },
                            connection_id,
                            session_options);
                    session = std::move(http_session);
                }

                const protocol::ProtocolFeedResult result =
                    session->Feed(bytes);
                if (result.status ==
                        protocol::ProtocolFeedStatus::ExecutorStopped ||
                    result.status ==
                        protocol::ProtocolFeedStatus::ExecutorSaturated ||
                    result.status ==
                        protocol::ProtocolFeedStatus::HandlerNotFound)
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
            "http_server.connection_create_failed",
            "HTTP connection을 생성하지 못했습니다.",
            {{"exception", exception.what()}});
    }
    catch (...)
    {
        logger_->Log(
            core::LogLevel::Error,
            "http_server.connection_create_failed",
            "HTTP connection 생성 중 알 수 없는 오류가 발생했습니다.");
    }
}

bool HttpServer::Stop()
{
    return Stop(options_.shutdown_timeout);
}

bool HttpServer::Stop(const std::chrono::milliseconds timeout)
{
    std::lock_guard stop_lock(stop_mutex_);

    {
        std::lock_guard lock(state_mutex_);
        if (state_ == HttpServerState::Stopped)
        {
            return true;
        }
        if (state_ == HttpServerState::Created)
        {
            state_ = HttpServerState::Stopped;
            return true;
        }
        state_ = HttpServerState::Stopping;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    listener_->Stop();
    if (!listener_->WaitStopped(Remaining(deadline)))
    {
        LogShutdownTimeout("listener");
        return false;
    }

    for (const auto& connection : registry_->Connections())
    {
        connection->BeginClose(
            transport::CloseReason::LocalShutdown);
    }
    if (!registry_->WaitEmpty(Remaining(deadline)))
    {
        LogShutdownTimeout("connection_registry");
        return false;
    }

    application_context_->Stop(execution::StopMode::Drain);
    if (!application_context_->WaitStopped(Remaining(deadline)))
    {
        LogShutdownTimeout("application_executor");
        return false;
    }
    application_context_->Join();

    io_context_.Stop();
    io_context_.Join();
    {
        std::lock_guard lock(state_mutex_);
        state_ = HttpServerState::Stopped;
    }
    logger_->Log(
        core::LogLevel::Info,
        "http_server.shutdown_completed",
        "HTTP server 종료를 완료했습니다.");
    return true;
}

std::uint16_t HttpServer::LocalPort() const
{
    return listener_ ? listener_->Snapshot().local_port : 0;
}

HttpServerSnapshot HttpServer::Snapshot() const
{
    HttpServerState state;
    {
        std::lock_guard lock(state_mutex_);
        state = state_;
    }
    return HttpServerSnapshot{
        state,
        listener_
            ? listener_->Snapshot()
            : transport::ListenerSnapshot{},
        registry_->Snapshot(),
        application_context_->Snapshot(),
    };
}

void HttpServer::LogShutdownTimeout(
    const std::string_view barrier) noexcept
{
    logger_->Log(
        core::LogLevel::Error,
        "http_server.shutdown_timeout",
        "HTTP server shutdown barrier가 시간 안에 완료되지 않았습니다.",
        {{"barrier", barrier}});
}

} // namespace iocp::server
