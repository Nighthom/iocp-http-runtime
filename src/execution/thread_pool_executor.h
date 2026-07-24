#pragma once

/// @file
/// @brief bounded queue와 고정 worker thread pool을 소유하는 executor.
///
/// ThreadPoolContext가 worker vector와 shared state의 lifecycle을 관리하며,
/// ThreadPoolExecutor는 IExecutor interface로 context의 bounded queue에
/// task를 제출한다. Drain join 시 worker가 자기 자신을 detach하여
/// shared State lifetime을 안전하게 유지한다.

#include "execution/executor.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace iocp::execution
{

class ThreadPoolExecutor;

/// @brief bounded application queue와 worker lifecycle을 소유한다.
class ThreadPoolContext final
{
public:
    ThreadPoolContext(
        std::size_t worker_count,
        std::size_t maximum_pending_tasks,
        TaskExceptionHandler exception_handler = {});
    ~ThreadPoolContext();

    ThreadPoolContext(const ThreadPoolContext&) = delete;
    ThreadPoolContext& operator=(const ThreadPoolContext&) = delete;

    /// @brief 신규 제출을 막고 pending task 처리 정책을 선택한다.
    void Stop(StopMode mode);

    /// @brief `Drain` stop을 요청하고 모든 worker를 회수한다.
    void Join();

    bool WaitStopped(std::chrono::milliseconds timeout);
    ExecutorSnapshot Snapshot() const;
    std::size_t ConfiguredWorkerCount() const noexcept;

private:
    struct State;

    SubmitStatus Submit(Task task);
    static void WorkerMain(const std::shared_ptr<State>& state) noexcept;

    friend class ThreadPoolExecutor;

    const std::size_t worker_count_;
    std::shared_ptr<State> state_;
    std::vector<std::thread> workers_;
    std::mutex join_mutex_;
};

/// @brief `ThreadPoolContext`의 bounded queue에 task를 제출한다.
class ThreadPoolExecutor final : public IExecutor
{
public:
    explicit ThreadPoolExecutor(
        std::shared_ptr<ThreadPoolContext> context);

    SubmitStatus Post(Task task) override;

private:
    std::shared_ptr<ThreadPoolContext> context_;
};

} // namespace iocp::execution
