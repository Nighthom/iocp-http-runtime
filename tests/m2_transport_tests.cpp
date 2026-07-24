// M2 transport의 foundation부터 coordinated shutdown까지 검증한다.
#include "buffer/receive_buffer.h"
#include "core/logging.h"
#include "execution/iocp_executor.h"
#include "platform/windows/socket_handle.h"
#include "platform/windows/winsock_runtime.h"
#include "runtime/completion_operation.h"
#include "runtime/io_context.h"
#include "echo_server/echo_server.h"
#include "transport/connection_registry.h"
#include "transport/send_queue.h"
#include "transport/tcp_connection.h"
#include "transport/tcp_connector.h"
#include "transport/tcp_listener.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using iocp::core::FileLogSink;
using iocp::core::LogLevel;
using iocp::core::Logger;
using iocp::core::StreamLogSink;
using iocp::buffer::BufferStatus;
using iocp::buffer::ByteView;
using iocp::buffer::ReceiveBuffer;
using iocp::execution::IocpExecutor;
using iocp::execution::StopMode;
using iocp::execution::SubmitStatus;
using iocp::platform::windows::SocketHandle;
using iocp::platform::windows::WinsockRuntime;
using iocp::runtime::CompletionOperation;
using iocp::runtime::IoContext;
using iocp::server::EchoServer;
using iocp::server::EchoServerOptions;
using iocp::server::EchoServerState;
using iocp::transport::ListenerOptions;
using iocp::transport::ListenerState;
using iocp::transport::ConnectionRegistry;
using iocp::transport::ConnectionOptions;
using iocp::transport::ConnectStartStatus;
using iocp::transport::SendConsumeResult;
using iocp::transport::SendQueue;
using iocp::transport::SendStatus;
using iocp::transport::TcpConnection;
using iocp::transport::TcpConnectOptions;
using iocp::transport::TcpConnector;
using iocp::transport::TcpConnectorState;
using iocp::transport::TcpListener;

void Check(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::size_t CountOccurrences(
    const std::string& text,
    const std::string& token)
{
    if (token.empty())
    {
        return 0;
    }

    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(token, position)) != std::string::npos)
    {
        ++count;
        position += token.size();
    }
    return count;
}

TcpConnection::OutboundBytes ToBytes(const std::string& text)
{
    const auto* first =
        reinterpret_cast<const std::byte*>(text.data());
    return TcpConnection::OutboundBytes(first, first + text.size());
}

void SendAll(const SOCKET socket, const std::vector<std::byte>& bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const int sent = ::send(
            socket,
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<int>(bytes.size() - offset),
            0);
        if (sent == SOCKET_ERROR)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "send");
        }
        Check(sent > 0, "send가 progress 없이 0을 반환했습니다");
        offset += static_cast<std::size_t>(sent);
    }
}

std::vector<std::byte> ReceiveExact(
    const SOCKET socket,
    const std::size_t size)
{
    std::vector<std::byte> bytes(size);
    std::size_t offset = 0;
    while (offset < size)
    {
        const int received = ::recv(
            socket,
            reinterpret_cast<char*>(bytes.data() + offset),
            static_cast<int>(size - offset),
            0);
        if (received == SOCKET_ERROR)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "recv");
        }
        Check(received > 0, "echo payload를 모두 받기 전에 peer가 종료됐습니다");
        offset += static_cast<std::size_t>(received);
    }
    return bytes;
}

SocketHandle ConnectClient(const std::uint16_t port)
{
    SocketHandle client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!client)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "socket(client)");
    }

    const DWORD timeout_ms = 3000;
    ::setsockopt(
        client.Get(),
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout_ms),
        sizeof(timeout_ms));

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(port);
    endpoint.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(
            client.Get(),
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)) == SOCKET_ERROR)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "connect(client)");
    }
    return client;
}

template <typename Predicate>
bool WaitUntil(
    const std::chrono::milliseconds timeout,
    Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return predicate();
}

void TestStreamSinkRouting()
{
    std::ostringstream standard_output;
    std::ostringstream standard_error;

    Logger logger;
    logger.AddSink(std::make_shared<StreamLogSink>(
        standard_output,
        LogLevel::Trace,
        LogLevel::Info));
    logger.AddSink(std::make_shared<StreamLogSink>(
        standard_error,
        LogLevel::Warning,
        LogLevel::Critical));

    logger.Log(
        LogLevel::Info,
        "logger.info",
        "표준 출력으로 보낼 정보입니다.",
        {{"phase", "foundation"}});
    logger.Log(
        LogLevel::Error,
        "logger.error",
        "표준 오류로 보낼 오류입니다.",
        {{"win32_error", "123"}});

    const std::string output = standard_output.str();
    const std::string error = standard_error.str();

    Check(
        output.find("logger.info") != std::string::npos,
        "info record가 stdout sink에 없습니다");
    Check(
        output.find("logger.error") == std::string::npos,
        "error record가 stdout sink의 level filter를 통과했습니다");
    Check(
        error.find("logger.error") != std::string::npos,
        "error record가 stderr sink에 없습니다");
    Check(
        error.find("logger.info") == std::string::npos,
        "info record가 stderr sink의 level filter를 통과했습니다");
    Check(
        output.find("표준 출력으로 보낼 정보입니다.") != std::string::npos,
        "한글 message가 stream에 UTF-8로 기록되지 않았습니다");
}

void TestFileAndStreamFanOut()
{
    const std::filesystem::path path =
        std::filesystem::current_path() / "m2_foundation_test.log";
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);

    std::ostringstream stream_output;
    {
        auto logger = std::make_shared<Logger>();
        logger->AddSink(std::make_shared<StreamLogSink>(stream_output));
        logger->AddSink(std::make_shared<FileLogSink>(path, false));

        logger->Log(
            LogLevel::Warning,
            "logger.fanout",
            "같은 record를 stream과 file에 기록합니다.",
            {{"encoding", "utf-8"}});
    }

    std::ifstream file(path, std::ios::binary);
    Check(file.is_open(), "file sink가 출력 file을 만들지 못했습니다");
    const std::string file_output{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};

    Check(
        stream_output.str().find("logger.fanout") != std::string::npos,
        "fan-out record가 stream sink에 없습니다");
    Check(
        file_output.find("logger.fanout") != std::string::npos,
        "fan-out record가 file sink에 없습니다");
    Check(
        file_output.find("같은 record를 stream과 file에 기록합니다.") !=
            std::string::npos,
        "file sink의 한글 UTF-8 message가 손상됐습니다");

    file.close();
    std::filesystem::remove(path, remove_error);
}

void TestConcurrentRecordsRemainWholeLines()
{
    constexpr std::size_t kThreadCount = 4;
    constexpr std::size_t kRecordsPerThread = 50;

    std::ostringstream output;
    Logger logger;
    logger.AddSink(std::make_shared<StreamLogSink>(output));

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (std::size_t thread_index = 0;
         thread_index < kThreadCount;
         ++thread_index)
    {
        producers.emplace_back([&logger, thread_index] {
            const std::string producer = std::to_string(thread_index);
            for (std::size_t record_index = 0;
                 record_index < kRecordsPerThread;
                 ++record_index)
            {
                const std::string sequence = std::to_string(record_index);
                logger.Log(
                    LogLevel::Debug,
                    "logger.concurrent",
                    "동시에 기록한 로그입니다.",
                    {
                        {"producer", producer},
                        {"sequence", sequence},
                    });
            }
        });
    }

    for (auto& producer : producers)
    {
        producer.join();
    }

    const std::string text = output.str();
    const std::size_t expected = kThreadCount * kRecordsPerThread;
    Check(
        static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) ==
            expected,
        "동시 logger 출력의 line 수가 record 수와 다릅니다");
    Check(
        CountOccurrences(text, "logger.concurrent") == expected,
        "동시 logger record 일부가 손실되거나 섞였습니다");
}

class RecoveryOperation final : public CompletionOperation
{
public:
    void Complete(
        std::uint32_t,
        std::error_code,
        std::uintptr_t) noexcept override
    {
    }
};

void TestCompletionOperationRecovery()
{
    RecoveryOperation operation;
    Check(
        CompletionOperation::FromNative(operation.NativeHandle()) == &operation,
        "OVERLAPPED pointer에서 operation owner를 복구하지 못했습니다");
}

void TestWinsockSocketAndIoContextFoundation()
{
    std::ostringstream output;
    auto logger = std::make_shared<Logger>();
    logger->AddSink(std::make_shared<StreamLogSink>(output));

    WinsockRuntime winsock(logger);
    {
        IoContext context(2, logger);

        SocketHandle socket(::WSASocketW(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP,
            nullptr,
            0,
            WSA_FLAG_OVERLAPPED));
        if (!socket)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "WSASocketW");
        }

        const SOCKET native_socket = socket.Get();
        SocketHandle moved(std::move(socket));
        Check(!socket, "move 이후 원본 SocketHandle이 비워지지 않았습니다");
        Check(
            moved.Get() == native_socket,
            "SocketHandle move가 native socket ownership을 잃었습니다");

        bool reserved_key_rejected = false;
        try
        {
            context.Associate(
                reinterpret_cast<HANDLE>(moved.Get()),
                std::numeric_limits<std::uintptr_t>::max());
        }
        catch (const std::invalid_argument&)
        {
            reserved_key_rejected = true;
        }
        Check(
            reserved_key_rejected,
            "IOCP internal completion key가 socket association에 허용됐습니다");

        context.Associate(
            reinterpret_cast<HANDLE>(moved.Get()),
            static_cast<std::uintptr_t>(17));
        context.Stop();

        bool rejected_after_stop = false;
        try
        {
            context.Associate(
                reinterpret_cast<HANDLE>(moved.Get()),
                static_cast<std::uintptr_t>(18));
        }
        catch (const std::logic_error&)
        {
            rejected_after_stop = true;
        }

        Check(
            rejected_after_stop,
            "stop 이후 handle association이 거부되지 않았습니다");
        context.Join();
        Check(context.IsStopping(), "IoContext stop 상태가 보존되지 않았습니다");
        Check(context.WorkerCount() == 2, "IoContext worker 수가 다릅니다");
    }

    const std::string log = output.str();
    Check(
        log.find("winsock.started") != std::string::npos,
        "Winsock 시작 로그가 없습니다");
    Check(
        log.find("iocp.runtime_started") != std::string::npos,
        "IOCP runtime 시작 로그가 없습니다");
    Check(
        log.find("iocp.handle_associated") != std::string::npos,
        "IOCP handle association 로그가 없습니다");
    Check(
        log.find("iocp.workers_joined") != std::string::npos,
        "IOCP worker Join 로그가 없습니다");
}

void TestListenerAcceptAndCancellation()
{
    using namespace std::chrono_literals;

    std::ostringstream output;
    auto logger = std::make_shared<Logger>();
    logger->AddSink(std::make_shared<StreamLogSink>(output));

    WinsockRuntime winsock(logger);
    IoContext context(2, logger);

    std::mutex accepted_mutex;
    std::condition_variable accepted_condition;
    std::size_t accepted_count = 0;

    auto listener = TcpListener::Create(
        context,
        logger,
        ListenerOptions{},
        [&](SocketHandle) {
            {
                std::lock_guard lock(accepted_mutex);
                ++accepted_count;
            }
            accepted_condition.notify_all();
        });

    const auto running = listener->Snapshot();
    Check(
        running.state == ListenerState::Running,
        "생성된 listener가 Running 상태가 아닙니다");
    Check(
        running.outstanding_accepts == 1,
        "listener가 하나의 AcceptEx를 유지하지 않습니다");
    Check(running.local_port != 0, "listener가 ephemeral port를 얻지 못했습니다");

    SocketHandle client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!client)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "socket(client)");
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(running.local_port);
    endpoint.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(
            client.Get(),
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)) == SOCKET_ERROR)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "connect");
    }

    {
        std::unique_lock lock(accepted_mutex);
        Check(
            accepted_condition.wait_for(lock, 2s, [&] {
                return accepted_count == 1;
            }),
            "AcceptEx completion이 handler까지 전달되지 않았습니다");
    }

    listener->Stop();
    Check(
        listener->WaitStopped(2s),
        "pending AcceptEx cancellation completion을 회수하지 못했습니다");

    const auto stopped = listener->Snapshot();
    Check(
        stopped.state == ListenerState::Stopped,
        "listener가 Stopped 상태로 전이하지 않았습니다");
    Check(
        stopped.outstanding_accepts == 0,
        "listener 종료 후 outstanding accept가 남았습니다");
    Check(
        stopped.accepted_connections == 1,
        "listener accepted connection 통계가 다릅니다");

    context.Stop();
    context.Join();

    const std::string log = output.str();
    Check(
        log.find("listener.started") != std::string::npos,
        "listener 시작 로그가 없습니다");
    Check(
        log.find("listener.stop_requested") != std::string::npos,
        "listener stop 로그가 없습니다");
}

void TestConnectionReceiveAndRegistryCleanup()
{
    using namespace std::chrono_literals;

    std::ostringstream output;
    auto logger = std::make_shared<Logger>();
    logger->AddSink(std::make_shared<StreamLogSink>(output));

    WinsockRuntime winsock(logger);
    IoContext context(2, logger);
    IocpExecutor task_executor(context, 128);
    auto registry = std::make_shared<ConnectionRegistry>();
    std::atomic<int> task_count{0};

    std::mutex received_mutex;
    std::condition_variable received_condition;
    ReceiveBuffer received(4, 64);

    auto listener = TcpListener::Create(
        context,
        logger,
        ListenerOptions{},
        [registry, logger, &received_mutex, &received_condition, &received](
            SocketHandle socket) {
            const auto id = registry->NextId();
            auto connection = TcpConnection::Create(
                id,
                std::move(socket),
                registry,
                logger,
                [&received_mutex, &received_condition, &received](
                    const std::shared_ptr<TcpConnection>&,
                    const ByteView bytes) {
                    {
                        std::lock_guard lock(received_mutex);
                        if (received.Append(bytes) != BufferStatus::Ready)
                        {
                            throw std::runtime_error(
                                "receive buffer 상한을 초과했습니다");
                        }
                    }
                    received_condition.notify_all();
                });

            registry->Add(connection);
            connection->Start();
        });

    const std::uint16_t port = listener->Snapshot().local_port;
    SocketHandle client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Check(static_cast<bool>(client), "receive test client socket 생성에 실패했습니다");

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(port);
    endpoint.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(
            client.Get(),
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)) == SOCKET_ERROR)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "connect(receive test)");
    }

    for (int index = 0; index < 64; ++index)
    {
        Check(
            task_executor.Post([&] { ++task_count; }) ==
                SubmitStatus::Accepted,
            "socket completion과 혼합할 IOCP task 제출에 실패했습니다");
    }

    const std::string payload = "receive-path";
    const int sent = ::send(
        client.Get(),
        payload.data(),
        static_cast<int>(payload.size()),
        0);
    Check(
        sent == static_cast<int>(payload.size()),
        "receive test payload 전체를 보내지 못했습니다");

    {
        std::unique_lock lock(received_mutex);
        Check(
            received_condition.wait_for(lock, 2s, [&] {
                return received.ReadableBytes() == payload.size();
            }),
            "WSARecv completion이 receive handler까지 전달되지 않았습니다");
    }

    const std::string received_text(
        reinterpret_cast<const char*>(received.ReadableView().Data()),
        received.ReadableBytes());
    Check(received_text == payload, "receive handler payload가 다릅니다");

    ::shutdown(client.Get(), SD_SEND);
    client.Reset();

    Check(
        registry->WaitEmpty(2s),
        "peer close 이후 connection registry가 비워지지 않았습니다");
    const auto registry_snapshot = registry->Snapshot();
    Check(
        registry_snapshot.active_connections == 0,
        "registry에 closed connection이 남았습니다");
    Check(
        registry_snapshot.total_added == 1 &&
            registry_snapshot.total_removed == 1,
        "registry add/remove 통계가 일치하지 않습니다");

    task_executor.Stop(StopMode::Drain);
    Check(
        task_executor.WaitStopped(2s),
        "socket completion과 혼합한 IOCP task가 drain되지 않았습니다");
    Check(task_count == 64, "혼합 IOCP task 일부가 실행되지 않았습니다");

    listener->Stop();
    Check(
        listener->WaitStopped(2s),
        "receive test listener가 종료되지 않았습니다");
    context.Stop();
    context.Join();

    const std::string log = output.str();
    Check(
        log.find("connection.close") != std::string::npos,
        "connection close 로그가 없습니다");
}

void TestSendQueuePartialAndCapacity()
{
    SendQueue queue(2, 8);
    auto first = std::make_shared<const std::vector<std::byte>>(
        ToBytes("abcdef"));
    Check(queue.TryPush(first), "첫 send buffer enqueue에 실패했습니다");
    Check(queue.ItemCount() == 1, "send queue item 수가 다릅니다");
    Check(queue.QueuedBytes() == 6, "send queue byte 수가 다릅니다");

    const auto initial = queue.Front();
    Check(initial.offset == 0 && initial.size == 6, "초기 send slice가 다릅니다");
    Check(
        queue.Consume(2) == SendConsumeResult::Progress,
        "partial send consume 결과가 Progress가 아닙니다");

    const auto partial = queue.Front();
    Check(
        partial.offset == 2 && partial.size == 4,
        "partial send 이후 offset/remaining이 다릅니다");
    Check(queue.QueuedBytes() == 4, "partial send 이후 queued byte가 다릅니다");

    auto overflow = std::make_shared<const std::vector<std::byte>>(
        ToBytes("12345"));
    Check(
        !queue.TryPush(overflow),
        "send queue byte 상한을 넘은 item이 enqueue됐습니다");
    Check(
        queue.Consume(4) == SendConsumeResult::ItemCompleted,
        "front 전체 consume이 item을 제거하지 않았습니다");
    Check(queue.Empty(), "send queue가 비지 않았습니다");
    Check(
        queue.Consume(1) == SendConsumeResult::Invalid,
        "빈 send queue consume이 Invalid가 아닙니다");
}

void TestSendQueueAtomicBatchGatherAndLifetime()
{
    SendQueue queue(5, 12);
    auto first = std::make_shared<const std::vector<std::byte>>(
        ToBytes("ab"));
    auto second = std::make_shared<const std::vector<std::byte>>(
        ToBytes("cdef"));
    auto third = std::make_shared<const std::vector<std::byte>>(
        ToBytes("gh"));

    Check(
        queue.TryPushBatch({first, second}) &&
            queue.TryPushBatch({third}),
        "send batch enqueue에 실패했습니다");
    Check(
        queue.BatchCount() == 2 &&
            queue.ItemCount() == 3 &&
            queue.QueuedBytes() == 8,
        "send batch queue count가 다릅니다");

    const auto first_gather = queue.Gather(3, 5);
    Check(
        first_gather.slices.size() == 2 &&
            first_gather.total_bytes == 5 &&
            first_gather.slices[0].buffer == first &&
            first_gather.slices[0].offset == 0 &&
            first_gather.slices[0].size == 2 &&
            first_gather.slices[1].buffer == second &&
            first_gather.slices[1].offset == 0 &&
            first_gather.slices[1].size == 3,
        "gather count/byte 상한 결과가 다릅니다");
    Check(
        queue.Consume(5) == SendConsumeResult::Progress,
        "segment 경계를 넘은 partial consume 결과가 다릅니다");

    const auto remaining = queue.Front();
    Check(
        remaining.buffer == second &&
            remaining.offset == 3 &&
            remaining.size == 1,
        "cross-segment consume 이후 front offset이 다릅니다");
    const auto second_gather = queue.Gather(2, 3);
    Check(
        second_gather.slices.size() == 2 &&
            second_gather.total_bytes == 3 &&
            second_gather.slices[0].size == 1 &&
            second_gather.slices[1].buffer == third &&
            second_gather.slices[1].size == 2,
        "partial segment 이후 다음 gather 순서가 다릅니다");
    Check(
        queue.Consume(3) == SendConsumeResult::ItemCompleted &&
            queue.Empty(),
        "gathered queue 전체 consume에 실패했습니다");

    SendQueue atomic_queue(2, 4);
    Check(
        atomic_queue.TryPush(first),
        "atomic rejection 기준 item enqueue에 실패했습니다");
    const auto overflow_one =
        std::make_shared<const std::vector<std::byte>>(ToBytes("x"));
    const auto overflow_two =
        std::make_shared<const std::vector<std::byte>>(ToBytes("y"));
    Check(
        !atomic_queue.TryPushBatch({overflow_one, overflow_two}) &&
            atomic_queue.BatchCount() == 1 &&
            atomic_queue.ItemCount() == 1 &&
            atomic_queue.QueuedBytes() == 2 &&
            atomic_queue.Front().buffer == first,
        "batch admission 거부가 queue를 부분 변경했습니다");

    SendQueue lifetime_queue(2, 8);
    auto lifetime_buffer =
        std::make_shared<const std::vector<std::byte>>(ToBytes("alive"));
    std::weak_ptr<const std::vector<std::byte>> weak = lifetime_buffer;
    Check(
        lifetime_queue.TryPush(lifetime_buffer),
        "lifetime test enqueue에 실패했습니다");
    lifetime_buffer.reset();
    auto owned_gather = lifetime_queue.Gather(1, 8);
    lifetime_queue.Clear();
    Check(
        !weak.expired(),
        "queue clear가 pending gather storage를 파괴했습니다");
    owned_gather.slices.clear();
    Check(
        weak.expired(),
        "gather release 이후 send storage가 남았습니다");
}

void TestConnectionGatheredBatchSend()
{
    using namespace std::chrono_literals;

    auto logger = std::make_shared<Logger>();
    WinsockRuntime winsock(logger);
    IoContext context(2, logger);
    auto registry = std::make_shared<ConnectionRegistry>();

    std::mutex accepted_mutex;
    std::condition_variable accepted_condition;
    std::shared_ptr<TcpConnection> accepted_connection;
    std::exception_ptr callback_failure;

    auto listener = TcpListener::Create(
        context,
        logger,
        ListenerOptions{},
        [registry,
         logger,
         &accepted_mutex,
         &accepted_condition,
         &accepted_connection,
         &callback_failure](SocketHandle socket) {
            try
            {
                ConnectionOptions options;
                options.maximum_send_queue_items = 8;
                options.maximum_send_queue_bytes = 64;
                options.maximum_gather_segments_per_operation = 3;
                options.maximum_gather_bytes_per_operation = 5;
                options.maximum_outbound_batch_segments = 4;

                auto connection = TcpConnection::Create(
                    registry->NextId(),
                    std::move(socket),
                    registry,
                    logger,
                    [](const std::shared_ptr<TcpConnection>&,
                       const ByteView) {},
                    options);
                registry->Add(connection);
                if (!connection->Start())
                {
                    throw std::runtime_error(
                        "gathered send connection receive 등록에 실패했습니다");
                }

                TcpConnection::OutboundBatch batch;
                batch.push_back(ToBytes("ab"));
                batch.push_back(ToBytes("cdef"));
                batch.push_back(ToBytes("ghij"));
                if (connection->SendBatch(std::move(batch)) !=
                    SendStatus::Accepted)
                {
                    throw std::runtime_error(
                        "gathered send batch가 거부됐습니다");
                }

                {
                    std::lock_guard lock(accepted_mutex);
                    accepted_connection = std::move(connection);
                }
            }
            catch (...)
            {
                std::lock_guard lock(accepted_mutex);
                callback_failure = std::current_exception();
            }
            accepted_condition.notify_all();
        });

    SocketHandle client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Check(
        static_cast<bool>(client),
        "gathered send client socket 생성에 실패했습니다");
    const DWORD timeout_ms = 2000;
    ::setsockopt(
        client.Get(),
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout_ms),
        sizeof(timeout_ms));

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(listener->Snapshot().local_port);
    endpoint.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(
            client.Get(),
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)) == SOCKET_ERROR)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "connect(gathered send test)");
    }

    {
        std::unique_lock lock(accepted_mutex);
        Check(
            accepted_condition.wait_for(lock, 2s, [&] {
                return callback_failure ||
                    static_cast<bool>(accepted_connection);
            }),
            "gathered send connection 생성이 timeout됐습니다");
        if (callback_failure)
        {
            std::rethrow_exception(callback_failure);
        }
    }

    const auto received = ReceiveExact(client.Get(), 10);
    Check(
        received == ToBytes("abcdefghij"),
        "gathered WSASend의 wire byte 순서가 다릅니다");

    client.Reset();
    Check(
        registry->WaitEmpty(2s),
        "gathered send client 종료 후 registry가 비워지지 않았습니다");
    accepted_connection.reset();
    listener->Stop();
    Check(
        listener->WaitStopped(2s),
        "gathered send listener가 종료되지 않았습니다");
    context.Stop();
    context.Join();
}

void TestConnectionEchoAndQueueOverflow()
{
    using namespace std::chrono_literals;

    std::ostringstream output;
    auto logger = std::make_shared<Logger>();
    logger->AddSink(std::make_shared<StreamLogSink>(output));

    WinsockRuntime winsock(logger);
    IoContext context(3, logger);
    auto registry = std::make_shared<ConnectionRegistry>();
    constexpr std::size_t kReceiveChunkBytes = 257;
    std::atomic<std::size_t> receive_completions{0};
    std::atomic<std::size_t> largest_receive{0};

    auto listener = TcpListener::Create(
        context,
        logger,
        ListenerOptions{},
        [registry,
         logger,
         &receive_completions,
         &largest_receive](SocketHandle socket) {
            const auto id = registry->NextId();
            ConnectionOptions options;
            options.receive_chunk_bytes = kReceiveChunkBytes;
            auto connection = TcpConnection::Create(
                id,
                std::move(socket),
                registry,
                logger,
                [&receive_completions,
                 &largest_receive](
                    const std::shared_ptr<TcpConnection>& connection,
                    const ByteView bytes) {
                    ++receive_completions;
                    std::size_t previous = largest_receive.load();
                    while (previous < bytes.Size() &&
                           !largest_receive.compare_exchange_weak(
                               previous,
                               bytes.Size()))
                    {
                    }
                    TcpConnection::OutboundBytes owned(
                        bytes.begin(),
                        bytes.end());
                    connection->Send(std::move(owned));
                },
                options);
            registry->Add(connection);
            connection->Start();
        });

    SocketHandle client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Check(static_cast<bool>(client), "echo client socket 생성에 실패했습니다");
    const DWORD timeout_ms = 2000;
    ::setsockopt(
        client.Get(),
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout_ms),
        sizeof(timeout_ms));

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(listener->Snapshot().local_port);
    endpoint.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(
            client.Get(),
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)) == SOCKET_ERROR)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "connect(echo test)");
    }

    TcpConnection::OutboundBytes payload(32 * 1024);
    for (std::size_t index = 0; index < payload.size(); ++index)
    {
        payload[index] = static_cast<std::byte>(index % 251);
    }

    SendAll(client.Get(), payload);
    const auto echoed = ReceiveExact(client.Get(), payload.size());
    Check(echoed == payload, "echo response가 원본 payload와 다릅니다");
    Check(
        receive_completions.load() > 1,
        "작은 receive chunk가 여러 completion을 만들지 않았습니다");
    Check(
        largest_receive.load() <= kReceiveChunkBytes,
        "receive completion이 설정한 chunk 크기를 넘었습니다");

    ::shutdown(client.Get(), SD_SEND);
    client.Reset();
    Check(
        registry->WaitEmpty(2s),
        "echo client 종료 후 registry가 비워지지 않았습니다");

    listener->Stop();
    Check(listener->WaitStopped(2s), "echo listener가 종료되지 않았습니다");
    context.Stop();
    context.Join();

    // 실제 socket timing과 별개로 connection의 overflow-close 정책도 확인한다.
    IoContext overflow_context(2, logger);
    auto overflow_registry = std::make_shared<ConnectionRegistry>();
    std::mutex accepted_mutex;
    std::condition_variable accepted_condition;
    std::shared_ptr<TcpConnection> accepted_connection;

    auto overflow_listener = TcpListener::Create(
        overflow_context,
        logger,
        ListenerOptions{},
        [overflow_registry,
         logger,
         &accepted_mutex,
         &accepted_condition,
         &accepted_connection](SocketHandle socket) {
            auto connection = TcpConnection::Create(
                overflow_registry->NextId(),
                std::move(socket),
                overflow_registry,
                logger,
                [](const std::shared_ptr<TcpConnection>&,
                   const ByteView) {},
                ConnectionOptions{2, 1, 4096, 1, 1, 1});
            overflow_registry->Add(connection);
            connection->Start();
            {
                std::lock_guard lock(accepted_mutex);
                accepted_connection = std::move(connection);
            }
            accepted_condition.notify_all();
        });

    SocketHandle overflow_client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Check(
        static_cast<bool>(overflow_client),
        "overflow client socket 생성에 실패했습니다");
    endpoint.sin_port = ::htons(overflow_listener->Snapshot().local_port);
    if (::connect(
            overflow_client.Get(),
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint)) == SOCKET_ERROR)
    {
        throw std::system_error(
            ::WSAGetLastError(),
            std::system_category(),
            "connect(overflow test)");
    }

    {
        std::unique_lock lock(accepted_mutex);
        Check(
            accepted_condition.wait_for(lock, 2s, [&] {
                return static_cast<bool>(accepted_connection);
            }),
            "overflow test connection이 생성되지 않았습니다");
    }

    Check(
        accepted_connection->Send(ToBytes("xx")) ==
            SendStatus::QueueOverflow,
        "send queue overflow가 명시적인 결과를 반환하지 않았습니다");
    Check(
        overflow_registry->WaitEmpty(2s),
        "send queue overflow 이후 registry가 비워지지 않았습니다");
    accepted_connection.reset();
    overflow_client.Reset();

    overflow_listener->Stop();
    Check(
        overflow_listener->WaitStopped(2s),
        "overflow listener가 종료되지 않았습니다");
    overflow_context.Stop();
    overflow_context.Join();

    Check(
        output.str().find("connection.send_queue_overflow") !=
            std::string::npos,
        "send queue overflow 로그가 없습니다");
}

void TestConnectorHandoffFailureAndShutdown()
{
    using namespace std::chrono_literals;

    std::ostringstream output;
    auto logger = std::make_shared<Logger>();
    logger->AddSink(std::make_shared<StreamLogSink>(output));

    WinsockRuntime winsock(logger);
    IoContext context(3, logger);
    auto server_registry = std::make_shared<ConnectionRegistry>();
    auto client_registry = std::make_shared<ConnectionRegistry>();

    auto listener = TcpListener::Create(
        context,
        logger,
        ListenerOptions{},
        [server_registry, logger](SocketHandle socket) {
            auto connection = TcpConnection::Create(
                server_registry->NextId(),
                std::move(socket),
                server_registry,
                logger,
                [](const std::shared_ptr<TcpConnection>& connection,
                   const ByteView bytes) {
                    connection->Send(TcpConnection::OutboundBytes(
                        bytes.begin(),
                        bytes.end()));
                });
            server_registry->Add(connection);
            connection->Start();
        });

    const std::uint16_t listener_port =
        listener->Snapshot().local_port;
    auto connector = TcpConnector::Create(context, logger);

    const std::string payload = "ConnectEx-to-TcpConnection";
    std::mutex result_mutex;
    std::condition_variable result_condition;
    std::string received;
    std::exception_ptr callback_failure;
    std::shared_ptr<TcpConnection> client_connection;
    std::atomic<std::size_t> success_callbacks{0};

    const auto start = connector->Connect(
        TcpConnectOptions{"127.0.0.1", listener_port},
        [&](SocketHandle socket, const std::error_code error) {
            ++success_callbacks;
            try
            {
                if (error)
                {
                    throw std::system_error(error, "ConnectEx completion");
                }
                Check(
                    static_cast<bool>(socket),
                    "성공한 ConnectEx가 유효한 socket을 넘기지 않았습니다");

                auto connection = TcpConnection::Create(
                    client_registry->NextId(),
                    std::move(socket),
                    client_registry,
                    logger,
                    [&](const std::shared_ptr<TcpConnection>&,
                        const ByteView bytes) {
                        {
                            std::lock_guard lock(result_mutex);
                            received.append(
                                reinterpret_cast<const char*>(bytes.Data()),
                                bytes.Size());
                        }
                        result_condition.notify_all();
                    });
                client_registry->Add(connection);
                {
                    std::lock_guard lock(result_mutex);
                    client_connection = connection;
                }
                if (!connection->Start())
                {
                    throw std::runtime_error(
                        "outbound TcpConnection receive 등록에 실패했습니다");
                }
                if (connection->Send(ToBytes(payload)) !=
                    SendStatus::Accepted)
                {
                    throw std::runtime_error(
                        "outbound TcpConnection send가 거부됐습니다");
                }
            }
            catch (...)
            {
                {
                    std::lock_guard lock(result_mutex);
                    callback_failure = std::current_exception();
                }
                result_condition.notify_all();
            }
        });
    Check(
        start.status == ConnectStartStatus::Accepted && !start.error,
        "ConnectEx 요청이 accepted되지 않았습니다");

    {
        std::unique_lock lock(result_mutex);
        Check(
            result_condition.wait_for(lock, 3s, [&] {
                return callback_failure ||
                    received.size() >= payload.size();
            }),
            "ConnectEx handoff echo가 timeout됐습니다");
        if (callback_failure)
        {
            std::rethrow_exception(callback_failure);
        }
        Check(
            received == payload,
            "ConnectEx handoff echo payload가 다릅니다");
    }
    Check(
        success_callbacks.load() == 1,
        "successful connect handler가 정확히 한 번 호출되지 않았습니다");

    {
        std::shared_ptr<TcpConnection> connection;
        {
            std::lock_guard lock(result_mutex);
            connection = client_connection;
        }
        connection->BeginClose(iocp::transport::CloseReason::LocalShutdown);
    }
    Check(
        client_registry->WaitEmpty(2s),
        "outbound TcpConnection registry가 비워지지 않았습니다");
    client_connection.reset();
    Check(
        server_registry->WaitEmpty(2s),
        "outbound peer close 뒤 server registry가 비워지지 않았습니다");

    // 모든 IOCP worker를 잠시 점유해 ConnectEx completion dequeue를 막은 뒤
    // Stop이 pending socket을 닫고 cancellation completion을 drain하는지 본다.
    IocpExecutor blocker(context, 8);
    std::mutex blocker_mutex;
    std::condition_variable blocker_condition;
    std::size_t blocked_workers = 0;
    bool release_workers = false;
    for (std::size_t index = 0; index < context.WorkerCount(); ++index)
    {
        Check(
            blocker.Post([&] {
                std::unique_lock lock(blocker_mutex);
                ++blocked_workers;
                blocker_condition.notify_all();
                blocker_condition.wait(lock, [&] {
                    return release_workers;
                });
            }) == SubmitStatus::Accepted,
            "IOCP worker blocker task가 거부됐습니다");
    }
    {
        std::unique_lock lock(blocker_mutex);
        Check(
            blocker_condition.wait_for(lock, 2s, [&] {
                return blocked_workers == context.WorkerCount();
            }),
            "ConnectEx cancellation test가 IOCP worker를 점유하지 못했습니다");
    }

    std::mutex cancellation_mutex;
    std::condition_variable cancellation_condition;
    std::error_code cancellation_error;
    bool cancellation_socket_was_valid = false;
    bool allow_cancellation_handler_return = false;
    std::atomic<std::size_t> cancellation_callbacks{0};
    const auto cancellation_start = connector->Connect(
        TcpConnectOptions{"127.0.0.1", listener_port},
        [&](SocketHandle socket, const std::error_code error) {
            std::unique_lock lock(cancellation_mutex);
            cancellation_socket_was_valid =
                static_cast<bool>(socket);
            cancellation_error = error;
            ++cancellation_callbacks;
            cancellation_condition.notify_all();
            cancellation_condition.wait(lock, [&] {
                return allow_cancellation_handler_return;
            });
        });
    Check(
        cancellation_start.status == ConnectStartStatus::Accepted,
        "cancellation 대상 ConnectEx가 accepted되지 않았습니다");

    connector->Stop();
    const auto stopping = connector->Snapshot();
    Check(
        stopping.state == TcpConnectorState::Stopping &&
            stopping.outstanding_connects == 1,
        "TcpConnector Stop이 pending connect drain을 기다리지 않았습니다");

    {
        std::lock_guard lock(blocker_mutex);
        release_workers = true;
    }
    blocker_condition.notify_all();
    blocker.Stop(StopMode::Drain);
    Check(
        blocker.WaitStopped(2s),
        "ConnectEx cancellation blocker가 종료되지 않았습니다");
    {
        std::unique_lock lock(cancellation_mutex);
        Check(
            cancellation_condition.wait_for(lock, 2s, [&] {
                return cancellation_callbacks.load() == 1;
            }),
            "cancelled ConnectEx handler가 호출되지 않았습니다");
        Check(
            !cancellation_socket_was_valid &&
                cancellation_error ==
                    std::make_error_code(std::errc::operation_canceled),
            "cancelled ConnectEx 결과가 올바르지 않습니다");
        Check(
            !connector->WaitStopped(50ms),
            "TcpConnector barrier가 connect handler 반환을 기다리지 않았습니다");
        allow_cancellation_handler_return = true;
    }
    cancellation_condition.notify_all();
    Check(
        connector->WaitStopped(2s),
        "TcpConnector가 cancellation completion을 drain하지 못했습니다");

    const auto stopped = connector->Snapshot();
    Check(
        stopped.state == TcpConnectorState::Stopped &&
            stopped.outstanding_connects == 0 &&
            stopped.successful_connects == 1 &&
            stopped.cancelled_connects == 1,
        "TcpConnector cancellation snapshot이 올바르지 않습니다");

    Check(
        server_registry->WaitEmpty(2s),
        "cancelled ConnectEx peer가 server registry에 남았습니다");
    listener->Stop();
    Check(
        listener->WaitStopped(2s),
        "connector failure test 전에 listener가 종료되지 않았습니다");

    auto failure_connector = TcpConnector::Create(context, logger);
    std::mutex failure_mutex;
    std::condition_variable failure_condition;
    std::error_code completion_error;
    bool failure_socket_was_valid = false;
    std::atomic<std::size_t> failure_callbacks{0};
    const auto failed_start = failure_connector->Connect(
        TcpConnectOptions{"127.0.0.1", listener_port},
        [&](SocketHandle socket, const std::error_code error) {
            {
                std::lock_guard lock(failure_mutex);
                failure_socket_was_valid =
                    static_cast<bool>(socket);
                completion_error = error;
                ++failure_callbacks;
            }
            failure_condition.notify_all();
        });

    if (failed_start.status == ConnectStartStatus::Accepted)
    {
        std::unique_lock lock(failure_mutex);
        Check(
            failure_condition.wait_for(lock, 3s, [&] {
                return failure_callbacks.load() == 1;
            }),
            "거부된 ConnectEx completion이 timeout됐습니다");
        Check(
            !failure_socket_was_valid &&
                static_cast<bool>(completion_error),
            "거부된 ConnectEx completion 결과가 올바르지 않습니다");
    }
    else
    {
        Check(
            failed_start.status == ConnectStartStatus::StartFailed &&
                static_cast<bool>(failed_start.error),
            "거부된 ConnectEx의 start 결과가 올바르지 않습니다");
        Check(
            failure_callbacks.load() == 0,
            "start failure인데 connect handler가 호출됐습니다");
    }

    failure_connector->Stop();
    Check(
        failure_connector->WaitStopped(2s),
        "failure TcpConnector가 종료되지 않았습니다");
    const auto failure_stopped = failure_connector->Snapshot();
    Check(
        failure_stopped.state == TcpConnectorState::Stopped &&
            failure_stopped.outstanding_connects == 0 &&
            failure_stopped.failed_connects >= 1,
        "failure TcpConnector snapshot이 올바르지 않습니다");

    const auto rejected = failure_connector->Connect(
        TcpConnectOptions{"127.0.0.1", listener_port},
        [](SocketHandle, std::error_code) {});
    Check(
        rejected.status == ConnectStartStatus::Stopped,
        "stopped TcpConnector가 신규 connect를 받았습니다");

    context.Stop();
    context.Join();

    const std::string log = output.str();
    Check(
        log.find("connector.connect_failed") != std::string::npos ||
            log.find("connector.connect_start_failed") != std::string::npos,
        "ConnectEx failure 로그가 없습니다");
}

void TestEchoServerMultiClientAndActiveShutdown()
{
    using namespace std::chrono_literals;

    std::ostringstream output;
    auto logger = std::make_shared<Logger>();
    logger->AddSink(std::make_shared<StreamLogSink>(output));

    EchoServerOptions options;
    options.io_worker_count = 4;
    auto server = EchoServer::Create(logger, options);

    constexpr std::size_t kClientCount = 8;
    constexpr std::size_t kRounds = 20;
    std::mutex failure_mutex;
    std::exception_ptr failure;
    std::vector<std::thread> clients;
    clients.reserve(kClientCount);

    for (std::size_t client_index = 0;
         client_index < kClientCount;
         ++client_index)
    {
        clients.emplace_back(
            [&, client_index] {
                try
                {
                    auto client = ConnectClient(server->LocalPort());
                    for (std::size_t round = 0; round < kRounds; ++round)
                    {
                        const std::string text =
                            "client=" + std::to_string(client_index) +
                            ",round=" + std::to_string(round) +
                            std::string(128, static_cast<char>('a' + round % 26));
                        const auto payload = ToBytes(text);
                        SendAll(client.Get(), payload);
                        Check(
                            ReceiveExact(client.Get(), payload.size()) == payload,
                            "multi-client echo payload가 다릅니다");
                    }
                    ::shutdown(client.Get(), SD_SEND);
                }
                catch (...)
                {
                    std::lock_guard lock(failure_mutex);
                    if (!failure)
                    {
                        failure = std::current_exception();
                    }
                }
            });
    }

    for (auto& client : clients)
    {
        client.join();
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }

    Check(
        WaitUntil(3s, [&] {
            return server->Snapshot().registry.active_connections == 0;
        }),
        "multi-client 종료 후 registry가 비워지지 않았습니다");

    auto aborted_client = ConnectClient(server->LocalPort());
    Check(
        WaitUntil(3s, [&] {
            return server->Snapshot().registry.active_connections == 1;
        }),
        "abort test connection이 registry에 들어오지 않았습니다");
    linger abortive_linger{};
    abortive_linger.l_onoff = 1;
    abortive_linger.l_linger = 0;
    Check(
        ::setsockopt(
            aborted_client.Get(),
            SOL_SOCKET,
            SO_LINGER,
            reinterpret_cast<const char*>(&abortive_linger),
            sizeof(abortive_linger)) == 0,
        "abortive SO_LINGER 설정에 실패했습니다");
    aborted_client.Reset();
    Check(
        WaitUntil(3s, [&] {
            return server->Snapshot().registry.active_connections == 0;
        }),
        "client abort 이후 registry가 비워지지 않았습니다");

    Check(
        server->Stop(3s),
        "multi-client echo server shutdown이 timeout됐습니다");

    auto stopped = server->Snapshot();
    Check(
        stopped.state == EchoServerState::Stopped,
        "EchoServer가 Stopped 상태가 아닙니다");
    Check(
        stopped.listener.outstanding_accepts == 0,
        "EchoServer 종료 후 outstanding accept가 남았습니다");
    Check(
        stopped.registry.active_connections == 0,
        "EchoServer 종료 후 active connection이 남았습니다");

    // idle receive가 pending인 client를 server 쪽에서 먼저 닫아 cancellation
    // completion과 registry drain을 검증한다.
    auto active_server = EchoServer::Create(logger, options);
    std::vector<SocketHandle> idle_clients;
    for (std::size_t index = 0; index < 6; ++index)
    {
        idle_clients.push_back(ConnectClient(active_server->LocalPort()));
    }

    Check(
        WaitUntil(3s, [&] {
            return active_server->Snapshot().registry.active_connections == 6;
        }),
        "active shutdown test connection이 모두 registry에 들어오지 않았습니다");
    Check(
        active_server->Stop(3s),
        "pending receive가 있는 EchoServer shutdown이 timeout됐습니다");

    const auto active_stopped = active_server->Snapshot();
    Check(
        active_stopped.state == EchoServerState::Stopped &&
            active_stopped.listener.outstanding_accepts == 0 &&
            active_stopped.registry.active_connections == 0,
        "active shutdown 이후 barrier count가 0이 아닙니다");
    idle_clients.clear();

    const std::string log = output.str();
    Check(
        CountOccurrences(log, "server.shutdown_completed") >= 2,
        "server shutdown 완료 로그가 누락됐습니다");
}

template <typename Test>
bool RunTest(const char* name, Test test)
{
    try
    {
        test();
        std::cout << "[통과] " << name << '\n';
        return true;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[실패] " << name << ": " << exception.what() << '\n';
        return false;
    }
    catch (...)
    {
        std::cerr << "[실패] " << name << ": 알 수 없는 예외\n";
        return false;
    }
}

} // namespace

int main()
{
    ::SetConsoleOutputCP(CP_UTF8);

    int failures = 0;
    failures += !RunTest("stream sink level 분리", TestStreamSinkRouting);
    failures += !RunTest("file/stream fan-out", TestFileAndStreamFanOut);
    failures += !RunTest(
        "동시 logger line 보존",
        TestConcurrentRecordsRemainWholeLines);
    failures += !RunTest(
        "CompletionOperation owner 복구",
        TestCompletionOperationRecovery);
    failures += !RunTest(
        "Winsock/socket/IOCP foundation",
        TestWinsockSocketAndIoContextFoundation);
    failures += !RunTest(
        "TcpListener AcceptEx/cancellation",
        TestListenerAcceptAndCancellation);
    failures += !RunTest(
        "TcpConnection receive/registry cleanup",
        TestConnectionReceiveAndRegistryCleanup);
    failures += !RunTest(
        "SendQueue partial/capacity",
        TestSendQueuePartialAndCapacity);
    failures += !RunTest(
        "SendQueue atomic batch/gather/lifetime",
        TestSendQueueAtomicBatchGatherAndLifetime);
    failures += !RunTest(
        "TcpConnection gathered batch send",
        TestConnectionGatheredBatchSend);
    failures += !RunTest(
        "TcpConnection echo/queue overflow",
        TestConnectionEchoAndQueueOverflow);
    failures += !RunTest(
        "TcpConnector handoff/failure/shutdown",
        TestConnectorHandoffFailureAndShutdown);
    failures += !RunTest(
        "EchoServer multi-client/active shutdown",
        TestEchoServerMultiClientAndActiveShutdown);

    if (failures == 0)
    {
        std::cout << "M2 transport 테스트를 모두 통과했습니다.\n";
    }

    return failures == 0 ? 0 : 1;
}
