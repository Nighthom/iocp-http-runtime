// sample protocol dispatcher 구현: handler 등록 및 executor 실행
#include "protocol/sample/sample_dispatcher.h"

#include <stdexcept>
#include <utility>

namespace iocp::protocol
{

SampleDispatcher::SampleDispatcher(
    std::shared_ptr<execution::IExecutor> executor)
    : executor_(std::move(executor))
{
    if (!executor_)
    {
        throw std::invalid_argument(
            "SampleDispatcher에는 executor가 필요합니다");
    }
}

bool SampleDispatcher::Register(
    const std::uint16_t message_id,
    Handler handler)
{
    if (!handler)
    {
        throw std::invalid_argument(
            "sample protocol handler는 비어 있을 수 없습니다");
    }

    auto shared_handler =
        std::make_shared<const Handler>(std::move(handler));
    std::lock_guard lock(mutex_);
    return handlers_.emplace(
        message_id,
        std::move(shared_handler)).second;
}

DispatchStatus SampleDispatcher::Dispatch(SampleMessage message)
{
    std::shared_ptr<const Handler> handler;
    {
        std::lock_guard lock(mutex_);
        const auto found = handlers_.find(message.id);
        if (found == handlers_.end())
        {
            return DispatchStatus::HandlerNotFound;
        }
        handler = found->second;
    }

    const execution::SubmitStatus submit_status = executor_->Post(
        [handler = std::move(handler),
         message = std::move(message)]() mutable {
            (*handler)(std::move(message));
        });
    switch (submit_status)
    {
    case execution::SubmitStatus::Accepted:
        return DispatchStatus::Accepted;
    case execution::SubmitStatus::Stopped:
        return DispatchStatus::ExecutorStopped;
    case execution::SubmitStatus::Saturated:
        return DispatchStatus::ExecutorSaturated;
    }
    return DispatchStatus::ExecutorStopped;
}

} // namespace iocp::protocol
