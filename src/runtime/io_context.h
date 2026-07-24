/// @file io_context.h
/// @brief IoContext — 하나의 Windows completion port와 여러 worker thread를
/// 소유하고 수명을 관리한다.
///
/// handle association, task packet injection, stop/join shutdown contract를
/// 제공하며, application은 IocpExecutor를 통해 이 컨텍스트에 작업을 제출한다.

#pragma once

#include "core/logging.h"
#include "runtime/completion_operation.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace iocp::execution
{

class IocpExecutor;

} // namespace iocp::execution

namespace iocp::runtime
{

class IoContextState;

/// @brief 하나의 Windows completion port와 dequeue worker를 소유한다.
///
/// native `CompletionOperation`과 `IocpExecutor`의 짧은 custom task packet을
/// dispatch한다. 느린 application task는 별도 context로 분리한다.
class IoContext final
{
public:
    /// @brief completion port를 만들고 worker thread를 시작한다.
    ///
    /// @param worker_count dequeue worker 수.
    /// @param logger lifecycle과 native failure를 기록할 logger.
    /// @throws std::invalid_argument argument가 유효하지 않은 경우.
    /// @throws std::system_error completion port 또는 thread 생성에 실패한 경우.
    IoContext(
        std::size_t worker_count,
        std::shared_ptr<core::Logger> logger);
    ~IoContext();

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    IoContext(IoContext&&) = delete;
    IoContext& operator=(IoContext&&) = delete;

    /// @brief overlapped I/O handle을 이 completion port에 연결한다.
    ///
    /// @param handle socket 또는 overlapped I/O를 지원하는 native handle.
    /// @param completion_key completion 때 함께 받을 application key. runtime이
    /// 예약한 internal key는 사용할 수 없다.
    /// @throws std::logic_error stop이 시작된 경우.
    /// @throws std::system_error association에 실패한 경우.
    void Associate(HANDLE handle, std::uintptr_t completion_key);

    /// @brief worker마다 하나의 stop packet을 등록한다.
    ///
    /// @pre 이후 도착할 kernel completion이 없어야 한다. listener와
    /// connection의 outstanding operation, `IocpExecutor`의 pending/running
    /// task를 먼저 0으로 만든다.
    void Stop() noexcept;

    /// @brief 모든 worker가 종료될 때까지 기다린다.
    ///
    /// @throws std::logic_error `Stop` 전이거나 worker 자신이 호출한 경우.
    void Join();

    /// @brief stop이 시작됐는지 반환한다.
    bool IsStopping() const noexcept;

    /// @brief 생성 시 지정한 worker 수를 반환한다.
    std::size_t WorkerCount() const noexcept;

private:
    static bool PostTask(
        const std::shared_ptr<IoContextState>& state,
        std::function<void()> task);

    void JoinNoThrow() noexcept;

    friend class execution::IocpExecutor;

    std::shared_ptr<core::Logger> logger_;
    std::shared_ptr<IoContextState> state_;
    std::vector<std::thread> workers_;
};

} // namespace iocp::runtime
