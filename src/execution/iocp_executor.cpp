#include "execution/iocp_executor.h"

// IOCP executor 구현. IoContext::PostTask로 packet을 제출하고,
// ExecutePacket에서 stop mode에 따라 task 실행/취소를 결정한다.
// Post 실패 시 RollBackSubmission으로 pending count를 복구하며,
// 모든 packet 소진/취소 시 MoveToStoppedIfDrainedLocked로 Stopped 전이 후
// stopped_condition을 깨워 WaitStopped를 해제한다.

#include "runtime/io_context.h"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace iocp::execution
{

struct IocpExecutor::State final
{
    State(
        const std::size_t maximum_pending_tasks_value,
        TaskExceptionHandler exception_handler_value)
        : maximum_pending_tasks(maximum_pending_tasks_value),
          exception_handler(std::move(exception_handler_value))
    {
    }

    const std::size_t maximum_pending_tasks;
    const TaskExceptionHandler exception_handler;

    mutable std::mutex mutex;
    std::condition_variable stopped_condition;
    ExecutionState execution_state{ExecutionState::Running};
    StopMode stop_mode{StopMode::Drain};
    std::size_t pending_tasks{};
    std::size_t running_tasks{};
    std::size_t completed_tasks{};
    std::size_t cancelled_tasks{};
};

IocpExecutor::IocpExecutor(
    runtime::IoContext& context,
    const std::size_t maximum_pending_tasks,
    TaskExceptionHandler exception_handler)
    : io_context_state_(context.state_),
      state_(std::make_shared<State>(
          maximum_pending_tasks,
          std::move(exception_handler)))
{
    if (maximum_pending_tasks == 0)
    {
        throw std::invalid_argument(
            "IOCP executor queue 상한은 1 이상이어야 합니다");
    }
}

IocpExecutor::~IocpExecutor()
{
    Stop(StopMode::CancelPending);
}

SubmitStatus IocpExecutor::Post(Task task)
{
    if (!task)
    {
        throw std::invalid_argument("executor task는 비어 있을 수 없습니다");
    }

    {
        std::lock_guard lock(state_->mutex);
        if (state_->execution_state != ExecutionState::Running)
        {
            return SubmitStatus::Stopped;
        }
        if (state_->pending_tasks >= state_->maximum_pending_tasks)
        {
            return SubmitStatus::Saturated;
        }
        ++state_->pending_tasks;
    }

    try
    {
        const auto state = state_;
        const bool accepted = runtime::IoContext::PostTask(
            io_context_state_,
            [state, task = std::move(task)]() mutable noexcept {
                ExecutePacket(state, std::move(task));
            });
        if (!accepted)
        {
            RollBackSubmission(true);
            return SubmitStatus::Stopped;
        }
    }
    catch (...)
    {
        RollBackSubmission(false);
        throw;
    }

    return SubmitStatus::Accepted;
}

void IocpExecutor::Stop(const StopMode mode)
{
    bool stopped = false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->execution_state == ExecutionState::Stopped)
        {
            return;
        }

        if (state_->execution_state == ExecutionState::Running)
        {
            state_->execution_state = ExecutionState::Stopping;
            state_->stop_mode = mode;
        }
        else if (mode == StopMode::CancelPending)
        {
            state_->stop_mode = StopMode::CancelPending;
        }

        stopped = MoveToStoppedIfDrainedLocked(*state_);
    }

    if (stopped)
    {
        state_->stopped_condition.notify_all();
    }
}

bool IocpExecutor::WaitStopped(
    const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(state_->mutex);
    return state_->stopped_condition.wait_for(lock, timeout, [this] {
        return state_->execution_state == ExecutionState::Stopped;
    });
}

ExecutorSnapshot IocpExecutor::Snapshot() const
{
    std::lock_guard lock(state_->mutex);
    return ExecutorSnapshot{
        state_->execution_state,
        state_->pending_tasks,
        state_->running_tasks,
        state_->completed_tasks,
        state_->cancelled_tasks,
    };
}

void IocpExecutor::ExecutePacket(
    const std::shared_ptr<State>& state,
    Task task) noexcept
{
    bool cancelled = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->pending_tasks > 0)
        {
            --state->pending_tasks;
        }

        cancelled =
            state->execution_state != ExecutionState::Running &&
            state->stop_mode == StopMode::CancelPending;
        if (cancelled)
        {
            ++state->cancelled_tasks;
        }
        else
        {
            ++state->running_tasks;
        }
    }

    if (!cancelled)
    {
        try
        {
            task();
        }
        catch (...)
        {
            if (state->exception_handler)
            {
                try
                {
                    state->exception_handler(std::current_exception());
                }
                catch (...)
                {
                    // exception handler failure가 IOCP worker를 종료하지 않게 한다.
                }
            }
        }
    }

    bool stopped = false;
    {
        std::lock_guard lock(state->mutex);
        if (!cancelled)
        {
            --state->running_tasks;
            ++state->completed_tasks;
        }
        stopped = MoveToStoppedIfDrainedLocked(*state);
    }

    if (stopped)
    {
        state->stopped_condition.notify_all();
    }
}

bool IocpExecutor::MoveToStoppedIfDrainedLocked(State& state) noexcept
{
    if (state.execution_state == ExecutionState::Stopping &&
        state.pending_tasks == 0 &&
        state.running_tasks == 0)
    {
        state.execution_state = ExecutionState::Stopped;
        return true;
    }
    return false;
}

void IocpExecutor::RollBackSubmission(const bool context_stopped) noexcept
{
    bool stopped = false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->pending_tasks > 0)
        {
            --state_->pending_tasks;
        }

        if (context_stopped &&
            state_->execution_state == ExecutionState::Running)
        {
            state_->execution_state = ExecutionState::Stopping;
        }
        stopped = MoveToStoppedIfDrainedLocked(*state_);
    }

    if (stopped)
    {
        state_->stopped_condition.notify_all();
    }
}

} // namespace iocp::execution
