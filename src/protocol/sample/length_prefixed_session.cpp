#include "protocol/sample/length_prefixed_session.h"

#include <stdexcept>
#include <utility>

namespace iocp::protocol
{

namespace
{

constexpr std::size_t kWireOverheadBytes = 6;

ProtocolFeedStatus ToFeedStatus(const DispatchStatus status) noexcept
{
    switch (status)
    {
    case DispatchStatus::Accepted:
        return ProtocolFeedStatus::Ready;
    case DispatchStatus::HandlerNotFound:
        return ProtocolFeedStatus::HandlerNotFound;
    case DispatchStatus::ExecutorStopped:
        return ProtocolFeedStatus::ExecutorStopped;
    case DispatchStatus::ExecutorSaturated:
        return ProtocolFeedStatus::ExecutorSaturated;
    }
    return ProtocolFeedStatus::ExecutorStopped;
}

} // namespace

LengthPrefixedSession::LengthPrefixedSession(
    std::shared_ptr<SampleDispatcher> dispatcher,
    const LengthPrefixedSessionOptions options)
    : dispatcher_(std::move(dispatcher)),
      receive_buffer_(
          options.initial_buffer_bytes,
          options.maximum_buffer_bytes),
      decoder_(options.maximum_payload_bytes)
{
    if (!dispatcher_)
    {
        throw std::invalid_argument(
            "LengthPrefixedSession에는 dispatcher가 필요합니다");
    }
    if (options.maximum_buffer_bytes < kWireOverheadBytes ||
        options.maximum_payload_bytes >
            options.maximum_buffer_bytes - kWireOverheadBytes)
    {
        throw std::invalid_argument(
            "receive buffer 상한이 최대 sample frame보다 작습니다");
    }
}

ProtocolFeedResult LengthPrefixedSession::Feed(
    const buffer::ByteView bytes)
{
    if (stopped_)
    {
        return ProtocolFeedResult{
            ProtocolFeedStatus::Stopped,
            0,
            receive_buffer_.ReadableBytes(),
        };
    }

    if (receive_buffer_.Append(bytes) != buffer::BufferStatus::Ready)
    {
        return Fail(ProtocolFeedStatus::BufferLimitExceeded, 0);
    }

    std::size_t messages_dispatched = 0;
    try
    {
        for (;;)
        {
            FrameDecodeResult decoded =
                decoder_.Decode(receive_buffer_.ReadableSequence());
            if (decoded.status == FrameDecodeStatus::Incomplete)
            {
                return ProtocolFeedResult{
                    ProtocolFeedStatus::Ready,
                    messages_dispatched,
                    receive_buffer_.ReadableBytes(),
                };
            }
            if (decoded.status == FrameDecodeStatus::Error)
            {
                last_decode_error_ = decoded.error;
                return Fail(
                    ProtocolFeedStatus::ProtocolError,
                    messages_dispatched);
            }

            const DispatchStatus dispatch_status =
                dispatcher_->Dispatch(std::move(decoded.message));
            if (dispatch_status != DispatchStatus::Accepted)
            {
                return Fail(
                    ToFeedStatus(dispatch_status),
                    messages_dispatched);
            }

            receive_buffer_.Consume(decoded.consumed_bytes);
            ++messages_dispatched;
        }
    }
    catch (...)
    {
        stopped_ = true;
        throw;
    }
}

bool LengthPrefixedSession::IsStopped() const noexcept
{
    return stopped_;
}

std::size_t LengthPrefixedSession::BufferedBytes() const noexcept
{
    return receive_buffer_.ReadableBytes();
}

FrameDecodeError LengthPrefixedSession::LastDecodeError() const noexcept
{
    return last_decode_error_;
}

ProtocolFeedResult LengthPrefixedSession::Fail(
    const ProtocolFeedStatus status,
    const std::size_t messages_dispatched) noexcept
{
    stopped_ = true;
    return ProtocolFeedResult{
        status,
        messages_dispatched,
        receive_buffer_.ReadableBytes(),
    };
}

} // namespace iocp::protocol
