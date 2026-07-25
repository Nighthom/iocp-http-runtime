/// @file timer_service.h
/// @brief deadline heap 기반의 경량 timer — schedule/cancel/reschedule

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace iocp::core
{

using TimerId = std::uint64_t;
using TimerCallback = std::function<void()>;

/// @brief deadline 기준 min-heap으로 관리하는 범용 타이머 서비스.
///
/// worker thread 하나가 sleep_until(next_deadline)로 대기하다
/// deadline이 지난 callback을 순차 실행한다. callback은 최소한의
/// non-blocking 작업만 수행해야 한다 (e.g. BeginClose, flag set).
class TimerService final
{
public:
    TimerService();
    ~TimerService();

    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    /// @brief duration 후에 callback을 한 번 실행한다.
    [[nodiscard]] TimerId Schedule(
        std::chrono::milliseconds duration,
        TimerCallback callback);

    /// @brief 등록된 timer를 취소한다 (실행 전에만).
    bool Cancel(TimerId id) noexcept;

    /// @brief timer를 취소하고 새 duration으로 다시 등록한다.
    [[nodiscard]] TimerId Reschedule(
        TimerId id,
        std::chrono::milliseconds duration,
        TimerCallback callback);

    /// @brief 모든 timer를 취소하고 worker를 종료한다.
    void Stop();

    std::size_t ActiveCount() const noexcept;

private:
    struct Entry final
    {
        std::chrono::steady_clock::time_point deadline;
        TimerId id;
        TimerCallback callback;
        bool operator<(const Entry& other) const noexcept;
    };

    void Run();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Entry> heap_;
    std::atomic<TimerId> next_id_{1};
    std::atomic<bool> stopped_{false};
    std::thread worker_;
};

} // namespace iocp::core
