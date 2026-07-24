#include "execution/inline_executor.h"
#include "execution/iocp_executor.h"
#include "execution/manual_executor.h"
#include "execution/serial_executor.h"
#include "execution/thread_pool_executor.h"
#include "core/logging.h"
#include "runtime/io_context.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

using iocp::execution::ExecutionState;
using iocp::execution::InlineExecutor;
using iocp::execution::IocpExecutor;
using iocp::execution::ManualExecutor;
using iocp::execution::SerialExecutor;
using iocp::execution::StopMode;
using iocp::execution::SubmitStatus;
using iocp::execution::ThreadPoolContext;
using iocp::execution::ThreadPoolExecutor;
using iocp::runtime::IoContext;

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestInlineAndManualExecutor()
{
    using namespace std::chrono_literals;

    std::atomic<int> exception_count{0};
    InlineExecutor inline_executor([&](std::exception_ptr error) {
        Check(error != nullptr, "inline exception_ptr가 비어 있습니다");
        ++exception_count;
    });

    int inline_value = 0;
    Check(
        inline_executor.Post([&] { inline_value = 7; }) ==
            SubmitStatus::Accepted,
        "InlineExecutor가 task를 받지 않았습니다");
    inline_executor.Post([] {
        throw std::runtime_error("inline test");
    });
    Check(inline_value == 7, "inline task가 즉시 실행되지 않았습니다");
    Check(exception_count == 1, "inline task 예외가 전달되지 않았습니다");

    std::vector<int> order;
    ManualExecutor manual(2);
    Check(
        manual.Post([&] { order.push_back(1); }) == SubmitStatus::Accepted,
        "첫 manual task 제출에 실패했습니다");
    Check(
        manual.Post([&] { order.push_back(2); }) == SubmitStatus::Accepted,
        "두 번째 manual task 제출에 실패했습니다");
    Check(
        manual.Post([] {}) == SubmitStatus::Saturated,
        "manual queue 포화가 보고되지 않았습니다");

    manual.Stop(StopMode::Drain);
    Check(
        manual.Post([] {}) == SubmitStatus::Stopped,
        "stop 이후 manual task가 받아들여졌습니다");
    Check(manual.RunReady() == 2, "manual drain task 수가 다릅니다");
    Check(
        order == std::vector<int>({1, 2}),
        "ManualExecutor FIFO 순서가 다릅니다");
    Check(manual.WaitStopped(100ms), "ManualExecutor가 멈추지 않았습니다");

    ManualExecutor cancel_manual(4);
    cancel_manual.Post([] {});
    cancel_manual.Post([] {});
    cancel_manual.Stop(StopMode::CancelPending);
    const auto cancelled = cancel_manual.Snapshot();
    Check(
        cancelled.state == ExecutionState::Stopped &&
            cancelled.cancelled_tasks == 2 &&
            cancelled.pending_tasks == 0,
        "ManualExecutor CancelPending 결과가 다릅니다");
}

void TestThreadPoolAdmissionAndStop()
{
    using namespace std::chrono_literals;

    std::atomic<int> exception_count{0};
    auto context = std::make_shared<ThreadPoolContext>(
        1,
        2,
        [&](std::exception_ptr error) {
            Check(error != nullptr, "thread pool exception_ptr가 비어 있습니다");
            ++exception_count;
        });
    auto executor = std::make_shared<ThreadPoolExecutor>(context);

    std::promise<void> started_promise;
    auto started = started_promise.get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::atomic<int> completed{0};

    Check(
        executor->Post([&] {
            started_promise.set_value();
            release.wait();
            ++completed;
        }) == SubmitStatus::Accepted,
        "blocking task 제출에 실패했습니다");
    Check(
        started.wait_for(2s) == std::future_status::ready,
        "thread pool worker가 task를 시작하지 않았습니다");

    Check(
        executor->Post([&] { ++completed; }) == SubmitStatus::Accepted,
        "첫 pending task 제출에 실패했습니다");
    Check(
        executor->Post([&] {
            ++completed;
            throw std::runtime_error("thread pool test");
        }) == SubmitStatus::Accepted,
        "두 번째 pending task 제출에 실패했습니다");
    Check(
        executor->Post([] {}) == SubmitStatus::Saturated,
        "thread pool queue 포화가 보고되지 않았습니다");

    context->Stop(StopMode::Drain);
    Check(
        executor->Post([] {}) == SubmitStatus::Stopped,
        "thread pool stop 이후 task가 받아들여졌습니다");
    release_promise.set_value();
    context->Join();

    const auto drained = context->Snapshot();
    Check(
        drained.state == ExecutionState::Stopped &&
            drained.completed_tasks == 3 &&
            drained.cancelled_tasks == 0 &&
            drained.live_workers == 0,
        "thread pool drain 결과가 다릅니다");
    Check(completed == 3, "thread pool task 실행 수가 다릅니다");
    Check(exception_count == 1, "thread pool 예외가 전달되지 않았습니다");

    auto cancel_context = std::make_shared<ThreadPoolContext>(1, 2);
    auto cancel_executor =
        std::make_shared<ThreadPoolExecutor>(cancel_context);
    std::promise<void> cancel_started_promise;
    auto cancel_started = cancel_started_promise.get_future();
    std::promise<void> cancel_release_promise;
    auto cancel_release = cancel_release_promise.get_future().share();

    cancel_executor->Post([&] {
        cancel_started_promise.set_value();
        cancel_release.wait();
    });
    Check(
        cancel_started.wait_for(2s) == std::future_status::ready,
        "cancel test worker가 시작되지 않았습니다");
    cancel_executor->Post([] {});
    cancel_executor->Post([] {});

    cancel_context->Stop(StopMode::CancelPending);
    cancel_release_promise.set_value();
    cancel_context->Join();
    const auto cancelled = cancel_context->Snapshot();
    Check(
        cancelled.state == ExecutionState::Stopped &&
            cancelled.completed_tasks == 1 &&
            cancelled.cancelled_tasks == 2 &&
            cancelled.live_workers == 0,
        "thread pool CancelPending 결과가 다릅니다");
}

void TestIocpExecutorAdmissionAndDrain()
{
    using namespace std::chrono_literals;

    auto logger = std::make_shared<iocp::core::Logger>();
    IoContext context(1, logger);
    std::atomic<int> exception_count{0};
    IocpExecutor executor(
        context,
        2,
        [&](std::exception_ptr error) {
            Check(error != nullptr, "IOCP exception_ptr가 비어 있습니다");
            ++exception_count;
        });

    std::promise<void> started_promise;
    auto started = started_promise.get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::atomic<int> completed{0};
    std::atomic<DWORD> task_thread_id{0};
    const DWORD caller_thread_id = ::GetCurrentThreadId();

    Check(
        executor.Post([&] {
            task_thread_id = ::GetCurrentThreadId();
            started_promise.set_value();
            release.wait();
            ++completed;
        }) == SubmitStatus::Accepted,
        "IOCP blocking task 제출에 실패했습니다");
    Check(
        started.wait_for(2s) == std::future_status::ready,
        "IOCP worker가 task를 시작하지 않았습니다");

    Check(
        executor.Post([&] { ++completed; }) == SubmitStatus::Accepted,
        "첫 IOCP pending task 제출에 실패했습니다");
    Check(
        executor.Post([&] {
            ++completed;
            throw std::runtime_error("IOCP executor test");
        }) == SubmitStatus::Accepted,
        "두 번째 IOCP pending task 제출에 실패했습니다");
    Check(
        executor.Post([] {}) == SubmitStatus::Saturated,
        "IOCP executor 포화가 보고되지 않았습니다");

    executor.Stop(StopMode::Drain);
    Check(
        executor.Post([] {}) == SubmitStatus::Stopped,
        "IOCP executor stop 이후 task가 받아들여졌습니다");
    release_promise.set_value();
    Check(executor.WaitStopped(2s), "IOCP executor drain이 timeout됐습니다");

    const auto drained = executor.Snapshot();
    Check(
        drained.state == ExecutionState::Stopped &&
            drained.pending_tasks == 0 &&
            drained.running_tasks == 0 &&
            drained.completed_tasks == 3 &&
            drained.cancelled_tasks == 0,
        "IOCP executor drain 결과가 다릅니다");
    Check(completed == 3, "IOCP task 실행 수가 다릅니다");
    Check(exception_count == 1, "IOCP task 예외가 전달되지 않았습니다");
    Check(
        task_thread_id != 0 && task_thread_id != caller_thread_id,
        "IOCP task가 IOCP worker에서 실행되지 않았습니다");

    context.Stop();
    context.Join();
}

void TestIocpExecutorCancelPending()
{
    using namespace std::chrono_literals;

    auto logger = std::make_shared<iocp::core::Logger>();
    IoContext context(1, logger);
    IocpExecutor executor(context, 2);

    std::promise<void> started_promise;
    auto started = started_promise.get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::atomic<int> cancelled_body_runs{0};

    executor.Post([&] {
        started_promise.set_value();
        release.wait();
    });
    Check(
        started.wait_for(2s) == std::future_status::ready,
        "IOCP cancel test worker가 시작되지 않았습니다");
    executor.Post([&] { ++cancelled_body_runs; });
    executor.Post([&] { ++cancelled_body_runs; });

    executor.Stop(StopMode::CancelPending);
    release_promise.set_value();
    Check(
        executor.WaitStopped(2s),
        "IOCP executor CancelPending이 timeout됐습니다");

    const auto cancelled = executor.Snapshot();
    Check(
        cancelled.state == ExecutionState::Stopped &&
            cancelled.pending_tasks == 0 &&
            cancelled.running_tasks == 0 &&
            cancelled.completed_tasks == 1 &&
            cancelled.cancelled_tasks == 2,
        "IOCP executor CancelPending 결과가 다릅니다");
    Check(
        cancelled_body_runs == 0,
        "취소된 IOCP task body가 실행됐습니다");

    context.Stop();
    context.Join();
}

void TestIocpExecutorUnderlyingStop()
{
    using namespace std::chrono_literals;

    auto logger = std::make_shared<iocp::core::Logger>();
    IoContext context(1, logger);
    IocpExecutor executor(context, 2);

    context.Stop();
    Check(
        executor.Post([] {}) == SubmitStatus::Stopped,
        "중단된 IoContext가 IOCP task를 받아들였습니다");
    Check(
        executor.WaitStopped(100ms),
        "underlying IoContext stop이 executor 상태에 반영되지 않았습니다");
    context.Join();
}

void TestIocpExecutorPostStopRace()
{
    using namespace std::chrono_literals;

    auto logger = std::make_shared<iocp::core::Logger>();
    IoContext context(4, logger);
    IocpExecutor executor(context, 256);

    std::atomic<int> accepted{0};
    std::atomic<int> executed{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;

    constexpr int kProducerCount = 4;
    constexpr int kTasksPerProducer = 500;
    producers.reserve(kProducerCount);
    for (int producer = 0; producer < kProducerCount; ++producer)
    {
        producers.emplace_back([&] {
            while (!start.load())
            {
                std::this_thread::yield();
            }

            for (int task = 0; task < kTasksPerProducer; ++task)
            {
                const auto status = executor.Post([&] { ++executed; });
                if (status == SubmitStatus::Accepted)
                {
                    ++accepted;
                }
                else if (status == SubmitStatus::Stopped)
                {
                    return;
                }
            }
        });
    }

    start = true;
    while (accepted.load() == 0)
    {
        std::this_thread::yield();
    }
    executor.Stop(StopMode::Drain);

    for (auto& producer : producers)
    {
        producer.join();
    }
    Check(
        executor.WaitStopped(5s),
        "IOCP executor Post/Stop 경합 drain이 timeout됐습니다");

    const auto stopped = executor.Snapshot();
    Check(
        stopped.state == ExecutionState::Stopped &&
            stopped.pending_tasks == 0 &&
            stopped.running_tasks == 0,
        "IOCP executor Post/Stop 경합 후 task가 남았습니다");
    Check(
        executed == accepted,
        "Accepted IOCP task가 정확히 한 번 실행되지 않았습니다");

    context.Stop();
    context.Join();
}

void TestSerialInlineReentryAndSaturation()
{
    using namespace std::chrono_literals;

    std::atomic<int> serial_exception_count{0};
    auto inline_executor = std::make_shared<InlineExecutor>();
    std::vector<int> order;
    auto serial = SerialExecutor::Create(
        inline_executor,
        4,
        [&](std::exception_ptr error) {
            Check(error != nullptr, "serial exception_ptr가 비어 있습니다");
            ++serial_exception_count;
        });

    Check(
        serial->Post([&] {
            order.push_back(1);
            Check(
                serial->Post([&] { order.push_back(3); }) ==
                    SubmitStatus::Accepted,
                "재진입 serial task 제출에 실패했습니다");
            order.push_back(2);
        }) == SubmitStatus::Accepted,
        "inline serial task 제출에 실패했습니다");
    Check(
        order == std::vector<int>({1, 2, 3}),
        "SerialExecutor 재진입 순서가 다릅니다");
    serial->Post([] {
        throw std::runtime_error("serial test");
    });
    serial->Post([&] { order.push_back(4); });
    Check(
        serial_exception_count == 1 && order.back() == 4,
        "serial 예외 이후 다음 task가 진행되지 않았습니다");
    serial->Stop(StopMode::Drain);
    Check(serial->WaitStopped(100ms), "inline serial이 멈추지 않았습니다");

    auto manual = std::make_shared<ManualExecutor>(1);
    auto bounded_serial = SerialExecutor::Create(manual, 2);
    Check(
        bounded_serial->Post([] {}) == SubmitStatus::Accepted,
        "bounded serial 첫 task 제출에 실패했습니다");
    Check(
        bounded_serial->Post([] {}) == SubmitStatus::Accepted,
        "bounded serial 두 번째 task 제출에 실패했습니다");
    Check(
        bounded_serial->Post([] {}) == SubmitStatus::Saturated,
        "serial queue 포화가 보고되지 않았습니다");
    bounded_serial->Stop(StopMode::Drain);
    Check(manual->RunReady() == 1, "serial drain task 수가 다릅니다");
    Check(
        bounded_serial->WaitStopped(100ms),
        "bounded serial이 drain되지 않았습니다");

    auto saturated_underlying = std::make_shared<ManualExecutor>(1);
    saturated_underlying->Post([] {});
    auto rejected_serial =
        SerialExecutor::Create(saturated_underlying, 2);
    Check(
        rejected_serial->Post([] {}) == SubmitStatus::Saturated,
        "underlying saturation이 serial 호출자에게 전달되지 않았습니다");
    Check(
        rejected_serial->Snapshot().pending_tasks == 0,
        "거부된 serial task가 queue에 남았습니다");
    saturated_underlying->RunReady();
    rejected_serial->Stop(StopMode::CancelPending);
}

void TestSerialConcurrentExecution()
{
    using namespace std::chrono_literals;

    auto context = std::make_shared<ThreadPoolContext>(4, 8);
    auto pool = std::make_shared<ThreadPoolExecutor>(context);
    auto serial = SerialExecutor::Create(pool, 128);

    std::atomic<int> active{0};
    std::atomic<int> maximum_active{0};
    std::mutex order_mutex;
    std::vector<int> order;

    constexpr int kTaskCount = 100;
    for (int index = 0; index < kTaskCount; ++index)
    {
        const auto status = serial->Post([&, index] {
            const int current = ++active;
            int observed = maximum_active.load();
            while (current > observed &&
                   !maximum_active.compare_exchange_weak(observed, current))
            {
            }

            std::this_thread::sleep_for(1ms);
            {
                std::lock_guard lock(order_mutex);
                order.push_back(index);
            }
            --active;
        });
        Check(status == SubmitStatus::Accepted, "serial task가 거부됐습니다");
    }

    serial->Stop(StopMode::Drain);
    Check(serial->WaitStopped(5s), "concurrent serial drain이 timeout됐습니다");
    context->Stop(StopMode::Drain);
    context->Join();

    Check(maximum_active == 1, "serial task가 동시에 실행됐습니다");
    Check(
        order.size() == kTaskCount,
        "serial task 실행 수가 다릅니다");
    Check(
        std::is_sorted(order.begin(), order.end()),
        "단일 producer serial FIFO 순서가 깨졌습니다");
}

void TestSerialOnIocpExecutor()
{
    using namespace std::chrono_literals;

    auto logger = std::make_shared<iocp::core::Logger>();
    IoContext context(2, logger);
    auto iocp_executor = std::make_shared<IocpExecutor>(context, 4);
    auto serial = SerialExecutor::Create(iocp_executor, 128);

    std::mutex order_mutex;
    std::vector<int> order;
    constexpr int kTaskCount = 100;
    for (int index = 0; index < kTaskCount; ++index)
    {
        Check(
            serial->Post([&, index] {
                std::lock_guard lock(order_mutex);
                order.push_back(index);
            }) == SubmitStatus::Accepted,
            "IOCP 기반 serial task가 거부됐습니다");
    }

    serial->Stop(StopMode::Drain);
    Check(
        serial->WaitStopped(2s),
        "IOCP 기반 SerialExecutor가 drain되지 않았습니다");
    iocp_executor->Stop(StopMode::Drain);
    Check(
        iocp_executor->WaitStopped(2s),
        "SerialExecutor 이후 IocpExecutor가 drain되지 않았습니다");
    context.Stop();
    context.Join();

    Check(
        order.size() == kTaskCount,
        "IOCP 기반 serial task 실행 수가 다릅니다");
    Check(
        std::is_sorted(order.begin(), order.end()),
        "IOCP 기반 SerialExecutor FIFO 순서가 깨졌습니다");
}

template <typename Test>
bool RunTest(const char* name, Test test)
{
    try
    {
        test();
        std::cout << "[통과] " << name << '\n';
        return true;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[실패] " << name << ": " << exception.what() << '\n';
        return false;
    }
    catch (...)
    {
        std::cerr << "[실패] " << name << ": 알 수 없는 예외\n";
        return false;
    }
}

} // namespace

int main()
{
    ::SetConsoleOutputCP(CP_UTF8);

    int failures = 0;
    failures += !RunTest(
        "Inline/Manual executor",
        TestInlineAndManualExecutor);
    failures += !RunTest(
        "ThreadPool admission/stop",
        TestThreadPoolAdmissionAndStop);
    failures += !RunTest(
        "IOCP executor admission/drain",
        TestIocpExecutorAdmissionAndDrain);
    failures += !RunTest(
        "IOCP executor cancel pending",
        TestIocpExecutorCancelPending);
    failures += !RunTest(
        "IOCP executor underlying stop",
        TestIocpExecutorUnderlyingStop);
    failures += !RunTest(
        "IOCP executor post/stop race",
        TestIocpExecutorPostStopRace);
    failures += !RunTest(
        "Serial inline reentry/saturation",
        TestSerialInlineReentryAndSaturation);
    failures += !RunTest(
        "Serial concurrent non-overlap",
        TestSerialConcurrentExecution);
    failures += !RunTest(
        "Serial on IOCP executor",
        TestSerialOnIocpExecutor);

    if (failures == 0)
    {
        std::cout << "Execution 테스트를 모두 통과했습니다.\n";
    }
    return failures == 0 ? 0 : 1;
}
