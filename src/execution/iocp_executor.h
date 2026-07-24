#pragma once

/// @file
/// @brief IOCP worker에 짧은 non-blocking task를 제출하는 bounded executor.
///
/// IoContext의 PostTask를 통해 IOCP completion queue에 task packet을 전달하며,
/// Stop(Drain/CancelPending)과 WaitStopped로 graceful shutdown을 지원한다.

#include "execution/executor.h"

#include <chrono>
#include <cstddef>
#include <memory>

namespace iocp::runtime
{

class IoContext;
class IoContextState;

} // namespace iocp::runtime

namespace iocp::execution
{

/// @brief IOCP worker에 짧은 non-blocking task를 제출하는 bounded executor다.
///
/// 이 executor는 application thread pool을 대체하지 않는다. accepted task는
/// inline으로 실행되지 않지만 `Post`가 반환되기 전에 다른 IOCP worker에서
/// 실행될 수 있다.
class IocpExecutor final : public IExecutor
{
public:
    IocpExecutor(
        runtime::IoContext& context,
        std::size_t maximum_pending_tasks,
        TaskExceptionHandler exception_handler = {});
    ~IocpExecutor();

    IocpExecutor(const IocpExecutor&) = delete;
    IocpExecutor& operator=(const IocpExecutor&) = delete;

    /// @brief task를 IOCP completion queue에 제출한다.
    ///
    /// @throws std::invalid_argument task가 비어 있는 경우.
    /// @throws std::system_error native packet 제출에 실패한 경우.
    SubmitStatus Post(Task task) override;

    /// @brief 신규 제출을 막고 pending packet의 처리 정책을 선택한다.
    ///
    /// `CancelPending`도 native queue에서 packet을 제거하지 않는다. worker가
    /// packet을 dequeue한 뒤 task body를 건너뛰며, 이미 실행 중인 task는
    /// 취소하지 않는다.
    void Stop(StopMode mode);

    /// @brief accepted packet이 모두 실행되거나 취소될 때까지 기다린다.
    bool WaitStopped(std::chrono::milliseconds timeout);

    ExecutorSnapshot Snapshot() const;

private:
    struct State;

    static void ExecutePacket(
        const std::shared_ptr<State>& state,
        Task task) noexcept;
    static bool MoveToStoppedIfDrainedLocked(State& state) noexcept;
    void RollBackSubmission(bool context_stopped) noexcept;

    std::shared_ptr<runtime::IoContextState> io_context_state_;
    std::shared_ptr<State> state_;
};

} // namespace iocp::execution
