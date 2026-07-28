/// @file protocol_session.h
/// @brief transport ↔ protocol 경계 interface

#pragma once

#include "buffer/byte_view.h"

#include <cstddef>
#include <cstdint>

namespace iocp::protocol
{

enum class ProtocolFeedStatus : std::uint8_t
{
    Ready,
    BufferLimitExceeded,
    ProtocolError,
    HandlerNotFound,
    ExecutorStopped,
    ExecutorSaturated,
    CloseRequired,
    Stopped,
};

struct ProtocolFeedResult final
{
    ProtocolFeedStatus status{ProtocolFeedStatus::Ready};
    std::size_t messages_dispatched{};
    std::size_t buffered_bytes{};
};

/// @brief transport의 borrowed byte를 protocol-owned state로 소비하는 경계다.
class IProtocolSession
{
public:
    virtual ~IProtocolSession() = default;

    /// @brief callback 동안 유효한 byte를 session storage로 복사하고 처리한다.
    ///
    /// 한 session에는 serialized execution context에서만 호출해야 한다.
    virtual ProtocolFeedResult Feed(buffer::ByteView bytes) = 0;
};

} // namespace iocp::protocol
