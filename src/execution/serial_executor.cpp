#include "execution/serial_executor.h"

// SerialExecutor 구현. Post는 task를 deque에 넣고 drain이 미진행 상태이면
// underlying에 drain lambda 하나를 제출한다. Drain()은 pending task를
// 하나씩 pop-front하여 Execute로 실행하며, 실행 완료 후 MoveToStoppedIfDrainedLocked로
// Stopped 조건을 확인한다. recursive mutex로 underlying InlineExecutor의
// 재진입을 허용한다.

#include <stdexcept>
#include <utility>

namespace iocp::execution
{

std::shared_ptr<SerialExecutor> SerialExecutor::Create(
    std::shared_ptr<IExecutor> underlying,
    const std::size_t maximum_pending_tasks,
    TaskExceptionHandler exception_handler)
{
    return std::shared_ptr<SerialExecutor>(new SerialExecutor(
        std::move(underlying),
        maximum_pending_tasks,
        std::move(exception_handler)));
}

SerialExecutor::SerialExecutor(
    std::shared_ptr<IExecutor> underlying,
    const std::size_t maximum_pending_tasks,
    TaskExceptionHandler exception_handler)
    : underlying_(std::move(underlying)),
      maximum_pending_tasks_(maximum_pending_tasks),
      exception_handler_(std::move(exception_handler))
{
    if (!underlying_)
    {
        throw std::invalid_argument(
            "SerialExecutor에는 underlying executor가 필요합니다");
    }
    if (maximum_pending_tasks_ == 0)
    {
        throw std::invalid_argument(
            "serial executor queue 상한은 1 이상이어야 합니다");
    }
}

SubmitStatus SerialExecutor::Post(Task task)
{
    if (!task)
    {
        throw std::invalid_argument("executor task는 비어 있을 수 없습니다");
    }

    // underlying이 InlineExecutor면 Post 안에서 Drain이 재진입한다.
    // recursive mutex로 scheduling 결과가 정해질 때까지 admission을 직렬화한다.
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
    if (drain_scheduled_)
    {
        return SubmitStatus::Accepted;
    }

    drain_scheduled_ = true;
    SubmitStatus status = SubmitStatus::Stopped;
    try
    {
        const auto self = shared_from_this();
        status = underlying_->Post([self] {
            self->Drain();
        });
    }
    catch (...)
    {
        drain_scheduled_ = false;
        tasks_.pop_back();
        throw;
    }

    if (status != SubmitStatus::Accepted)
    {
        drain_scheduled_ = false;
        tasks_.pop_back();
        if (status == SubmitStatus::Stopped)
        {
            state_ = ExecutionState::Stopped;
            stopped_condition_.notify_all();
        }
    }
    return status;
}

void SerialExecutor::Stop(const StopMode mode)
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

bool SerialExecutor::WaitStopped(
    const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return stopped_condition_.wait_for(lock, timeout, [this] {
        return state_ == ExecutionState::Stopped;
    });
}

ExecutorSnapshot SerialExecutor::Snapshot() const
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

void SerialExecutor::Drain() noexcept
{
    for (;;)
    {
        Task task;
        {
            std::lock_guard lock(mutex_);
            if (tasks_.empty())
            {
                drain_scheduled_ = false;
                const bool stopped = MoveToStoppedIfDrainedLocked();
                if (stopped)
                {
                    stopped_condition_.notify_all();
                }
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop_front();
            ++running_tasks_;
        }

        Execute(std::move(task));

        {
            std::lock_guard lock(mutex_);
            --running_tasks_;
            ++completed_tasks_;
            if (MoveToStoppedIfDrainedLocked())
            {
                stopped_condition_.notify_all();
            }
        }
    }
}

void SerialExecutor::Execute(Task task) noexcept
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
                // exception handler failure가 다음 serial task를 막지 않게 한다.
            }
        }
    }
}

bool SerialExecutor::MoveToStoppedIfDrainedLocked() noexcept
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
