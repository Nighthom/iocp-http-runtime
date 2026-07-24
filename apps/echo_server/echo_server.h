#pragma once

#include "core/logging.h"
#include "platform/windows/winsock_runtime.h"
#include "runtime/io_context.h"
#include "transport/connection_registry.h"
#include "transport/tcp_connection.h"
#include "transport/tcp_listener.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace iocp::server
{

struct EchoServerOptions final
{
    std::size_t io_worker_count{2};
    transport::ListenerOptions listener;
    transport::ConnectionOptions connection;
    std::chrono::milliseconds shutdown_timeout{std::chrono::seconds{10}};
};

enum class EchoServerState
{
    Created,
    Running,
    Stopping,
    Stopped,
};

struct EchoServerSnapshot final
{
    EchoServerState state{EchoServerState::Created};
    transport::ListenerSnapshot listener;
    transport::RegistrySnapshot registry;
};

/// @brief echo application의 composition root와 shutdown barrier다.
class EchoServer final
{
public:
    static std::unique_ptr<EchoServer> Create(
        std::shared_ptr<core::Logger> logger,
        EchoServerOptions options = {});

    ~EchoServer();

    EchoServer(const EchoServer&) = delete;
    EchoServer& operator=(const EchoServer&) = delete;

    /// @brief listener, connection, IOCP worker 순서로 server를 종료한다.
    ///
    /// timeout은 object를 강제로 파괴하지 않는다. barrier가 완료되지 않으면
    /// `false`를 반환하며 이후 다시 호출할 수 있다.
    ///
    /// @pre IOCP worker 자신이 호출하지 않는다.
    bool Stop(std::chrono::milliseconds timeout);

    /// @brief configured shutdown timeout으로 server를 종료한다.
    bool Stop();

    std::uint16_t LocalPort() const;
    EchoServerSnapshot Snapshot() const;

private:
    EchoServer(
        std::shared_ptr<core::Logger> logger,
        EchoServerOptions options);

    void Start();
    void OnAccepted(platform::windows::SocketHandle socket) noexcept;
    void LogShutdownTimeout(std::string_view barrier) noexcept;

    std::shared_ptr<core::Logger> logger_;
    EchoServerOptions options_;
    platform::windows::WinsockRuntime winsock_;
    runtime::IoContext io_context_;
    std::shared_ptr<transport::ConnectionRegistry> registry_;
    std::shared_ptr<transport::TcpListener> listener_;

    mutable std::mutex state_mutex_;
    std::mutex stop_mutex_;
    EchoServerState state_{EchoServerState::Created};
};

} // namespace iocp::server
