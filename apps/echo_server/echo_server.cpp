// echo 서버 composition root: TcpListener→TcpConnection→echo receive handler
// 조립, shutdown barrier 순서를 관리한다.

#include "echo_server/echo_server.h"

#include <algorithm>
#include <exception>
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

std::unique_ptr<EchoServer> EchoServer::Create(
    std::shared_ptr<core::Logger> logger,
    EchoServerOptions options)
{
    auto server = std::unique_ptr<EchoServer>(
        new EchoServer(std::move(logger), std::move(options)));
    server->Start();
    return server;
}

EchoServer::EchoServer(
    std::shared_ptr<core::Logger> logger,
    EchoServerOptions options)
    : logger_(std::move(logger)),
      options_(std::move(options)),
      winsock_(logger_),
      io_context_(options_.io_worker_count, logger_),
      registry_(std::make_shared<transport::ConnectionRegistry>())
{
    if (!logger_)
    {
        throw std::invalid_argument("EchoServer에는 Logger가 필요합니다");
    }
    if (options_.shutdown_timeout <= std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument(
            "EchoServer shutdown timeout은 0보다 커야 합니다");
    }
}

EchoServer::~EchoServer()
{
    if (!Stop())
    {
        logger_->Log(
            core::LogLevel::Critical,
            "server.destructor_shutdown_timeout",
            "destructor에서 shutdown barrier를 완료하지 못했습니다.");
        std::terminate();
    }
}

void EchoServer::Start()
{
    {
        std::lock_guard lock(state_mutex_);
        if (state_ != EchoServerState::Created)
        {
            throw std::logic_error("EchoServer는 한 번만 시작할 수 있습니다");
        }
        state_ = EchoServerState::Running;
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
            state_ = EchoServerState::Stopping;
        }
        io_context_.Stop();
        io_context_.Join();
        {
            std::lock_guard lock(state_mutex_);
            state_ = EchoServerState::Stopped;
        }
        throw;
    }

    const std::string port_text = std::to_string(LocalPort());
    logger_->Log(
        core::LogLevel::Info,
        "server.started",
        "M2 echo server를 시작했습니다.",
        {{"port", port_text}});
}

void EchoServer::OnAccepted(
    platform::windows::SocketHandle socket) noexcept
{
    try
    {
        // state 확인부터 registry add와 첫 receive 등록까지 같은 server
        // critical section에서 끝낸다. Stop snapshot 뒤에 connection이
        // 늦게 추가되는 race를 막는다.
        std::lock_guard lock(state_mutex_);
        if (state_ != EchoServerState::Running)
        {
            return;
        }

        const auto id = registry_->NextId();
        auto connection = transport::TcpConnection::Create(
            id,
            std::move(socket),
            registry_,
            logger_,
            [](const std::shared_ptr<transport::TcpConnection>& connection,
               const buffer::ByteView bytes) {
                transport::TcpConnection::OutboundBytes owned(
                    bytes.begin(),
                    bytes.end());
                connection->Send(std::move(owned));
            },
            options_.connection);

        registry_->Add(connection);
        connection->Start();
    }
    catch (const std::exception& exception)
    {
        logger_->Log(
            core::LogLevel::Error,
            "server.connection_create_failed",
            "accepted socket으로 connection을 만들지 못했습니다.",
            {{"exception", exception.what()}});
    }
    catch (...)
    {
        logger_->Log(
            core::LogLevel::Error,
            "server.connection_create_failed",
            "connection 생성 중 알 수 없는 예외가 발생했습니다.");
    }
}

bool EchoServer::Stop()
{
    return Stop(options_.shutdown_timeout);
}

bool EchoServer::Stop(const std::chrono::milliseconds timeout)
{
    std::lock_guard stop_lock(stop_mutex_);

    {
        std::lock_guard lock(state_mutex_);
        if (state_ == EchoServerState::Stopped)
        {
            return true;
        }
        if (state_ == EchoServerState::Created)
        {
            state_ = EchoServerState::Stopped;
            return true;
        }
        state_ = EchoServerState::Stopping;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    logger_->Log(
        core::LogLevel::Info,
        "server.shutdown_started",
        "M2 echo server shutdown barrier를 시작합니다.");

    listener_->Stop();
    if (!listener_->WaitStopped(Remaining(deadline)))
    {
        LogShutdownTimeout("listener");
        return false;
    }

    const auto connections = registry_->Connections();
    for (const auto& connection : connections)
    {
        connection->BeginClose(transport::CloseReason::LocalShutdown);
    }

    if (!registry_->WaitEmpty(Remaining(deadline)))
    {
        LogShutdownTimeout("connection_registry");
        return false;
    }

    // listener와 registry가 모두 drain된 뒤에만 worker stop packet을 넣는다.
    io_context_.Stop();
    io_context_.Join();

    {
        std::lock_guard lock(state_mutex_);
        state_ = EchoServerState::Stopped;
    }

    logger_->Log(
        core::LogLevel::Info,
        "server.shutdown_completed",
        "M2 echo server shutdown barrier를 완료했습니다.");
    return true;
}

std::uint16_t EchoServer::LocalPort() const
{
    if (!listener_)
    {
        return 0;
    }
    return listener_->Snapshot().local_port;
}

EchoServerSnapshot EchoServer::Snapshot() const
{
    EchoServerState state;
    {
        std::lock_guard lock(state_mutex_);
        state = state_;
    }

    return EchoServerSnapshot{
        state,
        listener_ ? listener_->Snapshot() : transport::ListenerSnapshot{},
        registry_->Snapshot(),
    };
}

void EchoServer::LogShutdownTimeout(
    const std::string_view barrier) noexcept
{
    try
    {
        const auto listener = listener_->Snapshot();
        const auto registry = registry_->Snapshot();
        const std::string outstanding_accepts =
            std::to_string(listener.outstanding_accepts);
        const std::string active_connections =
            std::to_string(registry.active_connections);

        logger_->Log(
            core::LogLevel::Error,
            "server.shutdown_timeout",
            "shutdown barrier가 timeout 안에 완료되지 않았습니다.",
            {
                {"barrier", barrier},
                {"outstanding_accepts", outstanding_accepts},
                {"active_connections", active_connections},
            });
    }
    catch (...)
    {
        logger_->Log(
            core::LogLevel::Error,
            "server.shutdown_timeout",
            "shutdown barrier timeout 진단 생성에 실패했습니다.");
    }
}

} // namespace iocp::server
