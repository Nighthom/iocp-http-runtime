/// @file preface_protocol_bootstrap.h
/// @brief bounded preface detection을 통해 protocol session을 선택한다.

#pragma once

#include "protocol/protocol_session.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iocp::protocol
{

/// @brief 정확한 preface가 완성되거나 prefix가 불일치할 때 session을 선택한다.
///
/// 선택 전 byte는 bootstrap이 소유한다. match session에는 완성된 preface를
/// 포함한 모든 byte를 전달하고, fallback session에도 판정에 사용한 byte를
/// 잃지 않고 전달한다.
class PrefaceProtocolBootstrap final : public IProtocolSession
{
public:
    using SessionFactory =
        std::function<std::shared_ptr<IProtocolSession>()>;

    PrefaceProtocolBootstrap(
        std::string preface,
        SessionFactory match_factory,
        SessionFactory fallback_factory);

    ProtocolFeedResult Feed(buffer::ByteView bytes) override;

private:
    ProtocolFeedResult SelectAndFeed(
        SessionFactory& factory,
        buffer::ByteView remaining);

    std::string preface_;
    SessionFactory match_factory_;
    SessionFactory fallback_factory_;
    std::vector<std::byte> pending_;
    std::shared_ptr<IProtocolSession> selected_;
};

} // namespace iocp::protocol
