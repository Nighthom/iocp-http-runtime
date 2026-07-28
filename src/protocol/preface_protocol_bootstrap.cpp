#include "protocol/preface_protocol_bootstrap.h"

#include <stdexcept>
#include <utility>

namespace iocp::protocol
{

PrefaceProtocolBootstrap::PrefaceProtocolBootstrap(
    std::string preface,
    SessionFactory match_factory,
    SessionFactory fallback_factory)
    : preface_(std::move(preface))
    , match_factory_(std::move(match_factory))
    , fallback_factory_(std::move(fallback_factory))
{
    if (preface_.empty())
    {
        throw std::invalid_argument(
            "protocol bootstrap preface must not be empty");
    }
    if (!match_factory_ || !fallback_factory_)
    {
        throw std::invalid_argument(
            "protocol bootstrap requires both session factories");
    }
    pending_.reserve(preface_.size());
}

ProtocolFeedResult PrefaceProtocolBootstrap::Feed(
    const buffer::ByteView bytes)
{
    if (selected_)
    {
        return selected_->Feed(bytes);
    }

    std::size_t offset = 0;
    while (offset < bytes.Size())
    {
        const std::byte value = bytes[offset++];
        const std::size_t preface_offset = pending_.size();
        pending_.push_back(value);

        if (static_cast<char>(value) != preface_[preface_offset])
        {
            return SelectAndFeed(
                fallback_factory_,
                bytes.SubView(offset));
        }

        if (pending_.size() == preface_.size())
        {
            return SelectAndFeed(
                match_factory_,
                bytes.SubView(offset));
        }
    }

    return ProtocolFeedResult{
        ProtocolFeedStatus::Ready,
        0,
        pending_.size(),
    };
}

ProtocolFeedResult PrefaceProtocolBootstrap::SelectAndFeed(
    SessionFactory& factory,
    const buffer::ByteView remaining)
{
    selected_ = factory();
    if (!selected_)
    {
        throw std::runtime_error(
            "protocol bootstrap factory returned null session");
    }

    ProtocolFeedResult result = selected_->Feed(
        buffer::ByteView(pending_.data(), pending_.size()));
    pending_.clear();

    if (result.status != ProtocolFeedStatus::Ready ||
        remaining.Empty())
    {
        return result;
    }

    const ProtocolFeedResult tail_result =
        selected_->Feed(remaining);
    return ProtocolFeedResult{
        tail_result.status,
        result.messages_dispatched +
            tail_result.messages_dispatched,
        tail_result.buffered_bytes,
    };
}

} // namespace iocp::protocol
