#include "buffer/buffer_sequence.h"
#include "core/logging.h"
#include "execution/inline_executor.h"
#include "execution/manual_executor.h"
#include "platform/windows/socket_handle.h"
#include "platform/windows/winsock_runtime.h"
#include "protocol/sample/length_prefixed_codec.h"
#include "protocol/sample/length_prefixed_session.h"
#include "protocol/sample/sample_dispatcher.h"
#include "runtime/io_context.h"
#include "transport/connection_registry.h"
#include "transport/tcp_connection.h"
#include "transport/tcp_listener.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

using iocp::buffer::ByteView;
using iocp::buffer::BufferSequence;
using iocp::core::Logger;
using iocp::execution::InlineExecutor;
using iocp::execution::ManualExecutor;
using iocp::execution::StopMode;
using iocp::execution::SubmitStatus;
using iocp::platform::windows::SocketHandle;
using iocp::platform::windows::WinsockRuntime;
using iocp::protocol::FrameDecodeError;
using iocp::protocol::FrameDecodeStatus;
using iocp::protocol::LengthPrefixedFrameDecoder;
using iocp::protocol::LengthPrefixedFrameEncoder;
using iocp::protocol::LengthPrefixedSession;
using iocp::protocol::LengthPrefixedSessionOptions;
using iocp::protocol::ProtocolFeedStatus;
using iocp::protocol::SampleDispatcher;
using iocp::protocol::SampleMessage;
using iocp::runtime::IoContext;
using iocp::transport::ConnectionRegistry;
using iocp::transport::ListenerOptions;
using iocp::transport::TcpConnection;
using iocp::transport::TcpListener;

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::vector<std::byte> ToBytes(const std::string& text)
{
    const auto* first =
        reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(first, first + text.size());
}

ByteView ViewOf(const std::vector<std::byte>& bytes)
{
    return ByteView(bytes.data(), bytes.size());
}

LengthPrefixedSessionOptions TestSessionOptions()
{
    return LengthPrefixedSessionOptions{
        2,
        64,
        32,
    };
}

void AppendBytes(
    std::vector<std::byte>& destination,
    const std::vector<std::byte>& source)
{
    destination.insert(
        destination.end(),
        source.begin(),
        source.end());
}

void SendAll(
    const SOCKET socket,
    const ByteView bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.Size())
    {
        const int sent = ::send(
            socket,
            reinterpret_cast<const char*>(bytes.Data() + offset),
            static_cast<int>(bytes.Size() - offset),
            0);
        if (sent == SOCKET_ERROR)
        {
            throw std::system_error(
                ::WSAGetLastError(),
                std::system_category(),
                "send(sample protocol)");
        }
        Check(sent > 0, "sample protocol send가 progress 없이 끝났습니다");
        offset += static_cast<std::size_t>(sent);
    }
}

void TestCodecAndEverySplitBoundary()
{
    LengthPrefixedFrameEncoder encoder(32);
    LengthPrefixedFrameDecoder decoder(32);
    const auto payload = ToBytes("split-payload");
    const auto frame = encoder.Encode(42, ViewOf(payload));

    const auto direct = decoder.Decode(ViewOf(frame));
    Check(
        direct.status == FrameDecodeStatus::Complete &&
            direct.error == FrameDecodeError::None &&
            direct.consumed_bytes == frame.size() &&
            direct.message.id == 42 &&
            direct.message.payload == payload,
        "length-prefixed codec round trip 결과가 다릅니다");

    for (std::size_t split = 0; split <= frame.size(); ++split)
    {
        const std::vector<std::byte> first_segment(
            frame.begin(),
            frame.begin() + split);
        const std::vector<std::byte> second_segment(
            frame.begin() + split,
            frame.end());
        const auto segmented = decoder.Decode(BufferSequence(
            ViewOf(first_segment),
            ViewOf(second_segment)));
        Check(
            segmented.status == FrameDecodeStatus::Complete &&
                segmented.error == FrameDecodeError::None &&
                segmented.consumed_bytes == frame.size() &&
                segmented.message.id == 42 &&
                segmented.message.payload == payload,
            "BufferSequence split boundary에서 decoder 결과가 달라졌습니다");

        auto executor = std::make_shared<ManualExecutor>(4);
        auto dispatcher =
            std::make_shared<SampleDispatcher>(executor);
        std::vector<SampleMessage> messages;
        Check(
            dispatcher->Register(
                42,
                [&](SampleMessage message) {
                    messages.push_back(std::move(message));
                }),
            "split test handler 등록에 실패했습니다");
        LengthPrefixedSession session(
            dispatcher,
            TestSessionOptions());

        const auto first = session.Feed(
            ViewOf(frame).SubView(0, split));
        Check(
            first.status == ProtocolFeedStatus::Ready,
            "첫 split feed가 실패했습니다");

        if (split < frame.size())
        {
            Check(
                first.messages_dispatched == 0,
                "incomplete split에서 message가 dispatch됐습니다");
            const auto second = session.Feed(
                ViewOf(frame).SubView(split));
            Check(
                second.status == ProtocolFeedStatus::Ready &&
                    second.messages_dispatched == 1 &&
                    second.buffered_bytes == 0,
                "두 번째 split feed 결과가 다릅니다");
        }
        else
        {
            Check(
                first.messages_dispatched == 1 &&
                    first.buffered_bytes == 0,
                "완전한 첫 feed 결과가 다릅니다");
        }

        Check(
            executor->RunReady() == 1,
            "split frame handler task 수가 다릅니다");
        Check(
            messages.size() == 1 &&
                messages[0].id == 42 &&
                messages[0].payload == payload,
            "split boundary에 따라 decoded message가 달라졌습니다");
        executor->Stop(StopMode::Drain);
        executor->RunReady();
    }
}

void TestRingWrappedSessionInput()
{
    LengthPrefixedFrameEncoder encoder(16);
    const auto first = encoder.Encode(
        1,
        ViewOf(ToBytes("a")));
    const auto second = encoder.Encode(
        2,
        ViewOf(ToBytes("bcdef")));

    std::vector<std::byte> first_feed;
    AppendBytes(first_feed, first);
    first_feed.insert(
        first_feed.end(),
        second.begin(),
        second.begin() + 3);

    auto executor = std::make_shared<ManualExecutor>(4);
    auto dispatcher = std::make_shared<SampleDispatcher>(executor);
    std::vector<SampleMessage> messages;
    for (std::uint16_t id = 1; id <= 2; ++id)
    {
        Check(
            dispatcher->Register(
                id,
                [&](SampleMessage message) {
                    messages.push_back(std::move(message));
                }),
            "ring wrapped session handler 등록에 실패했습니다");
    }

    LengthPrefixedSession session(
        dispatcher,
        LengthPrefixedSessionOptions{12, 32, 16});
    const auto first_result = session.Feed(ViewOf(first_feed));
    Check(
        first_result.status == ProtocolFeedStatus::Ready &&
            first_result.messages_dispatched == 1 &&
            first_result.buffered_bytes == 3,
        "ring wrap 준비 feed 결과가 다릅니다");

    const auto second_result = session.Feed(
        ViewOf(second).SubView(3));
    Check(
        second_result.status == ProtocolFeedStatus::Ready &&
            second_result.messages_dispatched == 1 &&
            second_result.buffered_bytes == 0,
        "ring span 경계를 넘은 frame decode 결과가 다릅니다");
    Check(
        executor->RunReady() == 2 &&
            messages.size() == 2 &&
            messages[0].id == 1 &&
            messages[0].payload == ToBytes("a") &&
            messages[1].id == 2 &&
            messages[1].payload == ToBytes("bcdef"),
        "ring wrapped session dispatch 결과가 다릅니다");
    executor->Stop(StopMode::Drain);
    executor->RunReady();
}

void TestMergedAndTrailingPartialFrames()
{
    LengthPrefixedFrameEncoder encoder(32);
    const auto one = encoder.Encode(1, ViewOf(ToBytes("one")));
    const auto two = encoder.Encode(2, ViewOf(ToBytes("two")));
    const auto three = encoder.Encode(3, ViewOf(ToBytes("three")));

    std::vector<std::byte> merged;
    AppendBytes(merged, one);
    AppendBytes(merged, two);
    merged.insert(
        merged.end(),
        three.begin(),
        three.begin() + 3);

    auto executor = std::make_shared<ManualExecutor>(8);
    auto dispatcher = std::make_shared<SampleDispatcher>(executor);
    std::vector<std::uint16_t> ids;
    for (std::uint16_t id = 1; id <= 3; ++id)
    {
        Check(
            dispatcher->Register(
                id,
                [&](SampleMessage message) {
                    ids.push_back(message.id);
                }),
            "merged frame handler 등록에 실패했습니다");
    }

    LengthPrefixedSession session(
        dispatcher,
        TestSessionOptions());
    const auto first = session.Feed(ViewOf(merged));
    Check(
        first.status == ProtocolFeedStatus::Ready &&
            first.messages_dispatched == 2 &&
            first.buffered_bytes == 3,
        "merged frames와 trailing partial 처리 결과가 다릅니다");

    const auto second = session.Feed(
        ViewOf(three).SubView(3));
    Check(
        second.status == ProtocolFeedStatus::Ready &&
            second.messages_dispatched == 1 &&
            second.buffered_bytes == 0,
        "trailing frame 나머지 처리 결과가 다릅니다");
    Check(
        executor->RunReady() == 3,
        "merged frame handler task 수가 다릅니다");
    Check(
        ids == std::vector<std::uint16_t>({1, 2, 3}),
        "merged frame dispatch 순서가 다릅니다");
    executor->Stop(StopMode::Drain);
    executor->RunReady();
}

void TestProtocolErrorsAndBufferLimit()
{
    auto executor = std::make_shared<ManualExecutor>(4);
    auto dispatcher = std::make_shared<SampleDispatcher>(executor);

    bool impossible_frame_limit_rejected = false;
    try
    {
        static_cast<void>(LengthPrefixedSession(
            dispatcher,
            LengthPrefixedSessionOptions{2, 5, 0}));
    }
    catch (const std::invalid_argument&)
    {
        impossible_frame_limit_rejected = true;
    }
    Check(
        impossible_frame_limit_rejected,
        "wire overhead보다 작은 session buffer 상한이 허용됐습니다");

    std::vector<std::byte> body_too_small{
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{1},
    };
    LengthPrefixedSession invalid_body_session(
        dispatcher,
        TestSessionOptions());
    const auto invalid_body =
        invalid_body_session.Feed(ViewOf(body_too_small));
    Check(
        invalid_body.status == ProtocolFeedStatus::ProtocolError &&
            invalid_body_session.LastDecodeError() ==
                FrameDecodeError::BodyTooSmall &&
            invalid_body_session.IsStopped(),
        "작은 frame body가 protocol error가 아닙니다");
    Check(
        invalid_body_session.Feed(ByteView{}).status ==
            ProtocolFeedStatus::Stopped,
        "protocol error 이후 session이 입력을 받았습니다");

    std::vector<std::byte> too_large{
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{35},
    };
    LengthPrefixedSession too_large_session(
        dispatcher,
        TestSessionOptions());
    const auto too_large_result =
        too_large_session.Feed(ViewOf(too_large));
    Check(
        too_large_result.status == ProtocolFeedStatus::ProtocolError &&
            too_large_session.LastDecodeError() ==
                FrameDecodeError::PayloadTooLarge,
        "큰 payload prefix가 protocol error가 아닙니다");

    LengthPrefixedSession limited_session(
        dispatcher,
        LengthPrefixedSessionOptions{2, 14, 8});
    std::vector<std::byte> overflow(15);
    Check(
        limited_session.Feed(ViewOf(overflow)).status ==
            ProtocolFeedStatus::BufferLimitExceeded,
        "session receive buffer 상한 초과가 보고되지 않았습니다");

    LengthPrefixedFrameEncoder encoder(8);
    bool encoder_limit_rejected = false;
    try
    {
        const std::vector<std::byte> oversized(9);
        static_cast<void>(encoder.Encode(1, ViewOf(oversized)));
    }
    catch (const std::length_error&)
    {
        encoder_limit_rejected = true;
    }
    Check(
        encoder_limit_rejected,
        "encoder payload 상한 초과가 허용됐습니다");
}

void TestDispatcherAdmissionFailures()
{
    LengthPrefixedFrameEncoder encoder(8);
    const auto frame = encoder.Encode(7, ByteView{});

    auto missing_executor = std::make_shared<ManualExecutor>(2);
    auto missing_dispatcher =
        std::make_shared<SampleDispatcher>(missing_executor);
    LengthPrefixedSession missing_session(
        missing_dispatcher,
        LengthPrefixedSessionOptions{2, 14, 8});
    Check(
        missing_session.Feed(ViewOf(frame)).status ==
            ProtocolFeedStatus::HandlerNotFound,
        "등록되지 않은 message id가 보고되지 않았습니다");

    auto saturated_executor = std::make_shared<ManualExecutor>(1);
    Check(
        saturated_executor->Post([] {}) == SubmitStatus::Accepted,
        "saturation seed task 제출에 실패했습니다");
    auto saturated_dispatcher =
        std::make_shared<SampleDispatcher>(saturated_executor);
    Check(
        saturated_dispatcher->Register(7, [](SampleMessage) {}),
        "saturation handler 등록에 실패했습니다");
    LengthPrefixedSession saturated_session(
        saturated_dispatcher,
        LengthPrefixedSessionOptions{2, 14, 8});
    Check(
        saturated_session.Feed(ViewOf(frame)).status ==
            ProtocolFeedStatus::ExecutorSaturated,
        "dispatcher executor saturation이 전달되지 않았습니다");
    saturated_executor->RunReady();
    saturated_executor->Stop(StopMode::Drain);
    saturated_executor->RunReady();

    auto stopped_executor = std::make_shared<ManualExecutor>(1);
    stopped_executor->Stop(StopMode::Drain);
    stopped_executor->RunReady();
    auto stopped_dispatcher =
        std::make_shared<SampleDispatcher>(stopped_executor);
    Check(
        stopped_dispatcher->Register(7, [](SampleMessage) {}),
        "stopped handler 등록에 실패했습니다");
    LengthPrefixedSession stopped_session(
        stopped_dispatcher,
        LengthPrefixedSessionOptions{2, 14, 8});
    Check(
        stopped_session.Feed(ViewOf(frame)).status ==
            ProtocolFeedStatus::ExecutorStopped,
        "dispatcher executor stop이 전달되지 않았습니다");

    Check(
        !stopped_dispatcher->Register(7, [](SampleMessage) {}),
        "중복 sample handler가 등록됐습니다");
}

void TestLengthPrefixedSessionOverTcpConnection()
{
    using namespace std::chrono_literals;

    auto logger = std::make_shared<Logger>();
    WinsockRuntime winsock(logger);
    IoContext context(2, logger);
    auto registry = std::make_shared<ConnectionRegistry>();
    auto executor = std::make_shared<InlineExecutor>();
    auto dispatcher = std::make_shared<SampleDispatcher>(executor);

    std::mutex message_mutex;
    std::condition_variable message_condition;
    std::vector<SampleMessage> messages;
    Check(
        dispatcher->Register(
            9,
            [&](SampleMessage message) {
                {
                    std::lock_guard lock(message_mutex);
                    messages.push_back(std::move(message));
                }
                message_condition.notify_all();
            }),
        "TCP sample handler 등록에 실패했습니다");

    auto listener = TcpListener::Create(
        context,
        logger,
        ListenerOptions{},
        [registry, logger, dispatcher](SocketHandle socket) {
            auto session = std::make_shared<LengthPrefixedSession>(
                dispatcher,
                TestSessionOptions());
            auto connection = TcpConnection::Create(
                registry->NextId(),
                std::move(socket),
                registry,
                logger,
                [session](
                    const std::shared_ptr<TcpConnection>& connection,
                    const ByteView bytes) {
                    const auto result = session->Feed(bytes);
                    if (result.status != ProtocolFeedStatus::Ready)
                    {
                        connection->BeginClose(
                            iocp::transport::CloseReason::HandlerError);
                    }
                });
            registry->Add(connection);
            connection->Start();
        });

    SocketHandle client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Check(static_cast<bool>(client), "sample protocol client 생성에 실패했습니다");
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
            "connect(sample protocol)");
    }

    LengthPrefixedFrameEncoder encoder(32);
    const auto payload = ToBytes("over-real-tcp");
    const auto frame = encoder.Encode(9, ViewOf(payload));
    SendAll(client.Get(), ViewOf(frame).SubView(0, 1));
    SendAll(client.Get(), ViewOf(frame).SubView(1, 3));
    SendAll(client.Get(), ViewOf(frame).SubView(4));

    {
        std::unique_lock lock(message_mutex);
        Check(
            message_condition.wait_for(lock, 2s, [&] {
                return messages.size() == 1;
            }),
            "TCP sample frame이 handler까지 전달되지 않았습니다");
    }
    Check(
        messages[0].id == 9 &&
            messages[0].payload == payload,
        "TCP sample message 내용이 다릅니다");

    ::shutdown(client.Get(), SD_SEND);
    client.Reset();
    Check(
        registry->WaitEmpty(2s),
        "sample protocol connection이 registry에서 제거되지 않았습니다");
    listener->Stop();
    Check(
        listener->WaitStopped(2s),
        "sample protocol listener가 멈추지 않았습니다");
    context.Stop();
    context.Join();
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
    failures += !RunTest(
        "codec every split boundary",
        TestCodecAndEverySplitBoundary);
    failures += !RunTest(
        "merged/trailing partial frames",
        TestMergedAndTrailingPartialFrames);
    failures += !RunTest(
        "ring wrapped segmented frame",
        TestRingWrappedSessionInput);
    failures += !RunTest(
        "protocol errors/buffer limit",
        TestProtocolErrorsAndBufferLimit);
    failures += !RunTest(
        "dispatcher admission failures",
        TestDispatcherAdmissionFailures);
    failures += !RunTest(
        "sample protocol over TcpConnection",
        TestLengthPrefixedSessionOverTcpConnection);

    if (failures == 0)
    {
        std::cout << "Protocol 테스트를 모두 통과했습니다.\n";
    }
    return failures == 0 ? 0 : 1;
}
