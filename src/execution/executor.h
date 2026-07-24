#pragma once

#include <cstddef>
#include <exception>
#include <functional>

namespace iocp::execution
{

using Task = std::function<void()>;
using TaskExceptionHandler = std::function<void(std::exception_ptr)>;

enum class SubmitStatus
{
    Accepted,
    Stopped,
    Saturated,
};

enum class StopMode
{
    Drain,
    CancelPending,
};

enum class ExecutionState
{
    Running,
    Stopping,
    Stopped,
};

struct ExecutorSnapshot final
{
    ExecutionState state{ExecutionState::Running};
    std::size_t pending_tasks{};
    std::size_t running_tasks{};
    std::size_t completed_tasks{};
    std::size_t cancelled_tasks{};
    std::size_t live_workers{};
};

/// @brief application task의 제출 위치와 admission 정책을 표현한다.
class IExecutor
{
public:
    virtual ~IExecutor() = default;

    /// @brief task를 executor에 제출한다.
    ///
    /// `Accepted`는 `CancelPending`이 적용되지 않는 한 task가 정확히 한 번
    /// 실행됨을 뜻한다. 다른 결과에서는 executor가 task를 소유하지 않는다.
    ///
    /// @throws std::invalid_argument task가 비어 있는 경우.
    virtual SubmitStatus Post(Task task) = 0;
};

} // namespace iocp::execution
