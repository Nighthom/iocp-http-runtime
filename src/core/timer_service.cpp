// TimerService 구현 — min-heap deadline timer
#include "core/timer_service.h"

#include <algorithm>
#include <stdexcept>

namespace iocp::core
{

bool TimerService::Entry::operator<(const Entry& other) const noexcept
{
    return deadline > other.deadline; // min-heap: earliest deadline at top
}

TimerService::TimerService()
    : worker_(&TimerService::Run, this)
{
}

TimerService::~TimerService()
{
    Stop();
}

TimerId TimerService::Schedule(
    const std::chrono::milliseconds duration,
    TimerCallback callback)
{
    if (!callback)
        throw std::invalid_argument("TimerService callback must not be empty");

    const auto deadline = std::chrono::steady_clock::now() + duration;
    const TimerId id = next_id_++;

    {
        std::lock_guard lock(mutex_);
        heap_.push_back({deadline, id, std::move(callback)});
        std::push_heap(heap_.begin(), heap_.end());
    }
    cv_.notify_one();
    return id;
}

bool TimerService::Cancel(const TimerId id) noexcept
{
    std::lock_guard lock(mutex_);
    for (auto& entry : heap_)
    {
        if (entry.id == id)
        {
            entry.callback = nullptr; // soft cancel
            return true;
        }
    }
    return false;
}

TimerId TimerService::Reschedule(
    const TimerId id,
    const std::chrono::milliseconds duration,
    TimerCallback callback)
{
    Cancel(id);
    return Schedule(duration, std::move(callback));
}

void TimerService::Stop()
{
    {
        std::lock_guard lock(mutex_);
        if (stopped_.exchange(true)) return;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();

    std::lock_guard lock(mutex_);
    heap_.clear();
}

std::size_t TimerService::ActiveCount() const noexcept
{
    std::lock_guard lock(mutex_);
    return heap_.size();
}

void TimerService::Run()
{
    for (;;)
    {
        std::unique_lock lock(mutex_);
        if (stopped_.load()) return;

        if (heap_.empty())
        {
            cv_.wait(lock);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (heap_.front().deadline > now)
        {
            cv_.wait_until(lock, heap_.front().deadline);
            continue;
        }

        // deadline 지난 entry pop
        std::pop_heap(heap_.begin(), heap_.end());
        Entry entry = std::move(heap_.back());
        heap_.pop_back();

        if (!entry.callback) continue; // cancelled

        lock.unlock();
        try
        {
            entry.callback();
        }
        catch (...) {}
    }
}

} // namespace iocp::core
