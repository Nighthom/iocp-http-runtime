#pragma once

#include "buffer/ring_receive_buffer.h"
#include "protocol/sample/length_prefixed_codec.h"
#include "protocol/protocol_session.h"
#include "protocol/sample/sample_dispatcher.h"

#include <cstddef>
#include <memory>

namespace iocp::protocol
{

struct LengthPrefixedSessionOptions final
{
    std::size_t initial_buffer_bytes{256};
    std::size_t maximum_buffer_bytes{64 * 1024};
    std::size_t maximum_payload_bytes{60 * 1024};
};

/// @brief length-prefixed sample protocol의 buffer/decoder/dispatch state다.
class LengthPrefixedSession final : public IProtocolSession
{
public:
    LengthPrefixedSession(
        std::shared_ptr<SampleDispatcher> dispatcher,
        LengthPrefixedSessionOptions options = {});

    ProtocolFeedResult Feed(buffer::ByteView bytes) override;

    bool IsStopped() const noexcept;
    std::size_t BufferedBytes() const noexcept;
    FrameDecodeError LastDecodeError() const noexcept;

private:
    ProtocolFeedResult Fail(
        ProtocolFeedStatus status,
        std::size_t messages_dispatched) noexcept;

    std::shared_ptr<SampleDispatcher> dispatcher_;
    buffer::RingReceiveBuffer receive_buffer_;
    LengthPrefixedFrameDecoder decoder_;
    bool stopped_{};
    FrameDecodeError last_decode_error_{FrameDecodeError::None};
};

} // namespace iocp::protocol
