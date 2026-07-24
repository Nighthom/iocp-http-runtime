#pragma once

/// @file
/// @brief 제출한 스레드에서 task를 즉시 동기 실행하는 executor.
///
/// Post 호출 스레드에서 task를 곧바로 실행하므로 별도의 queue나 worker가
/// 존재하지 않는다. SerialExecutor의 underlying으로 사용 시 재진입되는
/// 특성을 가지며, 테스트나 short task의 inline 실행에 적합하다.

#include "execution/executor.h"

#include <stdexcept>
#include <utility>

namespace iocp::execution
{

/// @brief 제출한 thread에서 task를 즉시 실행하는 executor다.
class InlineExecutor final : public IExecutor
{
public:
    explicit InlineExecutor(TaskExceptionHandler exception_handler = {})
        : exception_handler_(std::move(exception_handler))
    {
    }

    SubmitStatus Post(Task task) override
    {
        if (!task)
        {
            throw std::invalid_argument("executor task는 비어 있을 수 없습니다");
        }

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
                    // exception handler failure가 executor 경계를 넘지 않게 한다.
                }
            }
        }
        return SubmitStatus::Accepted;
    }

private:
    TaskExceptionHandler exception_handler_;
};

} // namespace iocp::execution
