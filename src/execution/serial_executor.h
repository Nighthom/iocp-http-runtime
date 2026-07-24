#pragma once

/// @file
/// @brief underlying executor 위에 admitted task의 FIFO 직렬화를 보장하는 executor.
///
/// SerialExecutor는 내부 deque에 task를 쌓고 drain task 하나를 underlying에
/// 제출하여 비중첩 순차 실행을 구현한다. drain이 활성화된 동안 추가 task는
///Post만으로 enqueue되므로, 추가적인 underlying 제출 없이 직렬 실행을 유지한다.

#include "execution/executor.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

namespace iocp::execution
{

/// @brief 다른 executor 위에서 admitted task의 비중첩 FIFO 실행을 보장한다.
class SerialExecutor final :
    public IExecutor,
    public std::enable_shared_from_this<SerialExecutor>
{
public:
    static std::shared_ptr<SerialExecutor> Create(
        std::shared_ptr<IExecutor> underlying,
        std::size_t maximum_pending_tasks,
        TaskExceptionHandler exception_handler = {});

    SubmitStatus Post(Task task) override;

    /// @brief 신규 제출을 막고 serial queue의 종료 정책을 선택한다.
    ///
    /// `Drain`을 사용할 때는 이 executor가 멈춘 뒤 underlying context를
    /// 중단해야 accepted drain task가 유실되지 않는다.
    void Stop(StopMode mode);

    bool WaitStopped(std::chrono::milliseconds timeout);
    ExecutorSnapshot Snapshot() const;

private:
    SerialExecutor(
        std::shared_ptr<IExecutor> underlying,
        std::size_t maximum_pending_tasks,
        TaskExceptionHandler exception_handler);

    void Drain() noexcept;
    void Execute(Task task) noexcept;
    bool MoveToStoppedIfDrainedLocked() noexcept;

    const std::shared_ptr<IExecutor> underlying_;
    const std::size_t maximum_pending_tasks_;
    const TaskExceptionHandler exception_handler_;

    mutable std::recursive_mutex mutex_;
    std::condition_variable_any stopped_condition_;
    std::deque<Task> tasks_;
    ExecutionState state_{ExecutionState::Running};
    StopMode stop_mode_{StopMode::Drain};
    bool drain_scheduled_{};
    std::size_t running_tasks_{};
    std::size_t completed_tasks_{};
    std::size_t cancelled_tasks_{};
};

} // namespace iocp::execution
