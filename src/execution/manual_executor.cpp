#include "execution/manual_executor.h"

// ManualExecutor 구현. Post는 task를 내부 deque에만 push하며, RunOne()/
// RunReady() 호출 시에만 pop-front하여 Execute를 호출한다. Stop(CancelPending)은
// 남은 task를 clear하여 cancelled count에 반영하고, 모든 task 소진 시
// MoveToStoppedIfDrainedLocked로 Stopped 상태로 전이한다.

#include <stdexcept>
#include <utility>

namespace iocp::execution
{

ManualExecutor::ManualExecutor(
    const std::size_t maximum_pending_tasks,
    TaskExceptionHandler exception_handler)
    : maximum_pending_tasks_(maximum_pending_tasks),
      exception_handler_(std::move(exception_handler))
{
    if (maximum_pending_tasks_ == 0)
    {
        throw std::invalid_argument(
            "manual executor queue 상한은 1 이상이어야 합니다");
    }
}

SubmitStatus ManualExecutor::Post(Task task)
{
    if (!task)
    {
        throw std::invalid_argument("executor task는 비어 있을 수 없습니다");
    }

    std::lock_guard lock(mutex_);
    if (state_ != ExecutionState::Running)
    {
        return SubmitStatus::Stopped;
    }
    if (tasks_.size() >= maximum_pending_tasks_)
    {
        return SubmitStatus::Saturated;
    }

    tasks_.push_back(std::move(task));
    return SubmitStatus::Accepted;
}

bool ManualExecutor::RunOne()
{
    Task task;
    {
        std::lock_guard lock(mutex_);
        if (tasks_.empty())
        {
            const bool stopped = MoveToStoppedIfDrainedLocked();
            if (stopped)
            {
                stopped_condition_.notify_all();
            }
            return false;
        }

        task = std::move(tasks_.front());
        tasks_.pop_front();
        ++running_tasks_;
    }

    Execute(std::move(task));

    bool stopped = false;
    {
        std::lock_guard lock(mutex_);
        --running_tasks_;
        ++completed_tasks_;
        stopped = MoveToStoppedIfDrainedLocked();
    }
    if (stopped)
    {
        stopped_condition_.notify_all();
    }
    return true;
}

std::size_t ManualExecutor::RunReady()
{
    std::size_t executed = 0;
    while (RunOne())
    {
        ++executed;
    }
    return executed;
}

void ManualExecutor::Stop(const StopMode mode)
{
    bool stopped = false;
    {
        std::lock_guard lock(mutex_);
        if (state_ == ExecutionState::Stopped)
        {
            return;
        }

        if (state_ == ExecutionState::Running)
        {
            state_ = ExecutionState::Stopping;
            stop_mode_ = mode;
        }
        else if (mode == StopMode::CancelPending)
        {
            stop_mode_ = StopMode::CancelPending;
        }

        if (stop_mode_ == StopMode::CancelPending)
        {
            cancelled_tasks_ += tasks_.size();
            tasks_.clear();
        }
        stopped = MoveToStoppedIfDrainedLocked();
    }

    if (stopped)
    {
        stopped_condition_.notify_all();
    }
}

bool ManualExecutor::WaitStopped(
    const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return stopped_condition_.wait_for(lock, timeout, [this] {
        return state_ == ExecutionState::Stopped;
    });
}

ExecutorSnapshot ManualExecutor::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return ExecutorSnapshot{
        state_,
        tasks_.size(),
        running_tasks_,
        completed_tasks_,
        cancelled_tasks_,
    };
}

void ManualExecutor::Execute(Task task) noexcept
{
    try
    {
        task();
    }
    catch (...)
    {
        if (exception_handler_)
        {
            try
            {
                exception_handler_(std::current_exception());
            }
            catch (...)
            {
                // exception handler failure가 다음 task 실행을 막지 않게 한다.
            }
        }
    }
}

bool ManualExecutor::MoveToStoppedIfDrainedLocked() noexcept
{
    if (state_ == ExecutionState::Stopping &&
        tasks_.empty() &&
        running_tasks_ == 0)
    {
        state_ = ExecutionState::Stopped;
        return true;
    }
    return false;
}

} // namespace iocp::execution
