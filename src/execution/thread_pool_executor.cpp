#include "execution/thread_pool_executor.h"

#include <condition_variable>
#include <deque>
#include <stdexcept>
#include <utility>

namespace iocp::execution
{

struct ThreadPoolContext::State final
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
    std::condition_variable work_condition;
    std::condition_variable stopped_condition;
    std::deque<Task> tasks;
    ExecutionState execution_state{ExecutionState::Running};
    StopMode stop_mode{StopMode::Drain};
    std::size_t live_workers{};
    std::size_t running_tasks{};
    std::size_t completed_tasks{};
    std::size_t cancelled_tasks{};
};

ThreadPoolContext::ThreadPoolContext(
    const std::size_t worker_count,
    const std::size_t maximum_pending_tasks,
    TaskExceptionHandler exception_handler)
    : worker_count_(worker_count),
      state_(std::make_shared<State>(
          maximum_pending_tasks,
          std::move(exception_handler)))
{
    if (worker_count == 0)
    {
        throw std::invalid_argument(
            "thread pool worker 수는 1 이상이어야 합니다");
    }
    if (maximum_pending_tasks == 0)
    {
        throw std::invalid_argument(
            "thread pool queue 상한은 1 이상이어야 합니다");
    }

    workers_.reserve(worker_count);
    try
    {
        for (std::size_t index = 0; index < worker_count; ++index)
        {
            workers_.emplace_back([state = state_] {
                WorkerMain(state);
            });
            std::lock_guard lock(state_->mutex);
            ++state_->live_workers;
        }
    }
    catch (...)
    {
        Stop(StopMode::CancelPending);
        Join();
        throw;
    }
}

ThreadPoolContext::~ThreadPoolContext()
{
    Stop(StopMode::CancelPending);
    Join();
}

void ThreadPoolContext::Stop(const StopMode mode)
{
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

        if (state_->stop_mode == StopMode::CancelPending)
        {
            state_->cancelled_tasks += state_->tasks.size();
            state_->tasks.clear();
        }

        if (state_->live_workers == 0)
        {
            state_->execution_state = ExecutionState::Stopped;
        }
    }

    state_->work_condition.notify_all();
    state_->stopped_condition.notify_all();
}

void ThreadPoolContext::Join()
{
    Stop(StopMode::Drain);

    std::lock_guard join_lock(join_mutex_);
    const auto current_thread = std::this_thread::get_id();
    for (auto& worker : workers_)
    {
        if (!worker.joinable())
        {
            continue;
        }

        if (worker.get_id() == current_thread)
        {
            // Worker가 마지막 context owner를 해제해도 shared State는 worker
            // loop가 끝날 때까지 유지된다.
            worker.detach();
        }
        else
        {
            worker.join();
        }
    }
    workers_.clear();
}

bool ThreadPoolContext::WaitStopped(
    const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(state_->mutex);
    return state_->stopped_condition.wait_for(lock, timeout, [this] {
        return state_->execution_state == ExecutionState::Stopped;
    });
}

ExecutorSnapshot ThreadPoolContext::Snapshot() const
{
    std::lock_guard lock(state_->mutex);
    return ExecutorSnapshot{
        state_->execution_state,
        state_->tasks.size(),
        state_->running_tasks,
        state_->completed_tasks,
        state_->cancelled_tasks,
        state_->live_workers,
    };
}

std::size_t ThreadPoolContext::ConfiguredWorkerCount() const noexcept
{
    return worker_count_;
}

SubmitStatus ThreadPoolContext::Submit(Task task)
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
        if (state_->tasks.size() >= state_->maximum_pending_tasks)
        {
            return SubmitStatus::Saturated;
        }
        state_->tasks.push_back(std::move(task));
    }

    state_->work_condition.notify_one();
    return SubmitStatus::Accepted;
}

void ThreadPoolContext::WorkerMain(
    const std::shared_ptr<State>& state) noexcept
{
    for (;;)
    {
        Task task;
        {
            std::unique_lock lock(state->mutex);
            state->work_condition.wait(lock, [&] {
                return state->execution_state != ExecutionState::Running ||
                    !state->tasks.empty();
            });

            if (!state->tasks.empty() &&
                (state->execution_state == ExecutionState::Running ||
                 state->stop_mode == StopMode::Drain))
            {
                task = std::move(state->tasks.front());
                state->tasks.pop_front();
                ++state->running_tasks;
            }
            else if (state->execution_state != ExecutionState::Running)
            {
                if (state->live_workers > 0)
                {
                    --state->live_workers;
                }
                if (state->live_workers == 0)
                {
                    state->execution_state = ExecutionState::Stopped;
                }
                lock.unlock();
                state->stopped_condition.notify_all();
                return;
            }
            else
            {
                continue;
            }
        }

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
                    // exception handler failure가 worker를 종료하지 않게 한다.
                }
            }
        }

        {
            std::lock_guard lock(state->mutex);
            --state->running_tasks;
            ++state->completed_tasks;
        }
        state->stopped_condition.notify_all();
    }
}

ThreadPoolExecutor::ThreadPoolExecutor(
    std::shared_ptr<ThreadPoolContext> context)
    : context_(std::move(context))
{
    if (!context_)
    {
        throw std::invalid_argument(
            "ThreadPoolExecutor에는 context가 필요합니다");
    }
}

SubmitStatus ThreadPoolExecutor::Post(Task task)
{
    return context_->Submit(std::move(task));
}

} // namespace iocp::execution
