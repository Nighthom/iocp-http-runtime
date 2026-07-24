#pragma once

#include "execution/executor.h"
#include "protocol/sample/length_prefixed_codec.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace iocp::protocol
{

enum class DispatchStatus : std::uint8_t
{
    Accepted,
    HandlerNotFound,
    ExecutorStopped,
    ExecutorSaturated,
};

/// @brief sample message id를 application handler와 executor에 연결한다.
class SampleDispatcher final
{
public:
    using Handler = std::function<void(SampleMessage message)>;

    explicit SampleDispatcher(
        std::shared_ptr<execution::IExecutor> executor);

    /// @brief id에 handler를 한 번 등록한다.
    ///
    /// 같은 id가 이미 있으면 `false`를 반환한다.
    bool Register(std::uint16_t message_id, Handler handler);

    DispatchStatus Dispatch(SampleMessage message);

private:
    std::shared_ptr<execution::IExecutor> executor_;

    std::mutex mutex_;
    std::unordered_map<
        std::uint16_t,
        std::shared_ptr<const Handler>> handlers_;
};

} // namespace iocp::protocol
