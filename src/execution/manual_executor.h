#pragma once

/// @file
/// @brief 테스트가 task 실행 시점을 직접 제어하는 bounded executor.
///
/// 외부 worker가 없고 RunOne()/RunReady()를 호출해야 task가 실행되므로,
/// 단위 테스트에서 실행 순서와 중간 상태를 결정적으로 검증할 수 있다.

#include "execution/executor.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace iocp::execution
{

/// @brief 테스트가 task 실행 시점을 직접 진행하는 bounded executor다.
class ManualExecutor final : public IExecutor
{
public:
    explicit ManualExecutor(
        std::size_t maximum_pending_tasks,
        TaskExceptionHandler exception_handler = {});

    SubmitStatus Post(Task task) override;

    /// @brief 대기 task 하나를 현재 thread에서 실행한다.
    bool RunOne();

    /// @brief 현재 실행 가능한 task를 queue가 빌 때까지 실행한다.
    std::size_t RunReady();

    /// @brief 신규 제출을 막고 선택한 정책으로 pending task를 처리한다.
    void Stop(StopMode mode);

    bool WaitStopped(std::chrono::milliseconds timeout);
    ExecutorSnapshot Snapshot() const;

private:
    void Execute(Task task) noexcept;
    bool MoveToStoppedIfDrainedLocked() noexcept;

    const std::size_t maximum_pending_tasks_;
    const TaskExceptionHandler exception_handler_;

    mutable std::mutex mutex_;
    std::condition_variable stopped_condition_;
    std::deque<Task> tasks_;
    ExecutionState state_{ExecutionState::Running};
    StopMode stop_mode_{StopMode::Drain};
    std::size_t running_tasks_{};
    std::size_t completed_tasks_{};
    std::size_t cancelled_tasks_{};
};

} // namespace iocp::execution
