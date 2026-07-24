#include "runtime/io_context.h"

#include <memory>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace iocp::runtime
{

namespace
{

constexpr ULONG_PTR kTaskCompletionKey =
    std::numeric_limits<ULONG_PTR>::max();
constexpr ULONG_PTR kStopCompletionKey = kTaskCompletionKey - 1;

bool IsReservedCompletionKey(const std::uintptr_t completion_key) noexcept
{
    return completion_key == kTaskCompletionKey ||
        completion_key == kStopCompletionKey;
}

class QueuedTaskPacket final
{
public:
    explicit QueuedTaskPacket(std::function<void()> task)
        : task_(std::move(task))
    {
    }

    void Execute()
    {
        task_();
    }

private:
    std::function<void()> task_;
};

} // namespace

class IoContextState final
{
public:
    IoContextState(
        const std::size_t worker_count,
        std::shared_ptr<core::Logger> logger)
        : worker_count_(worker_count),
          logger_(std::move(logger))
    {
        if (worker_count_ == 0)
        {
            throw std::invalid_argument("IOCP worker 수는 1 이상이어야 합니다");
        }
        if (!logger_)
        {
            throw std::invalid_argument("IoContext에는 Logger가 필요합니다");
        }

        port_ = ::CreateIoCompletionPort(
            INVALID_HANDLE_VALUE,
            nullptr,
            0,
            static_cast<DWORD>(worker_count_));
        if (port_ == nullptr)
        {
            const DWORD error = ::GetLastError();
            const std::string error_text = std::to_string(error);
            logger_->Log(
                core::LogLevel::Critical,
                "iocp.create_failed",
                "IO completion port 생성에 실패했습니다.",
                {{"win32_error", error_text}});
            throw std::system_error(
                static_cast<int>(error),
                std::system_category(),
                "CreateIoCompletionPort");
        }
    }

    ~IoContextState()
    {
        Close();
    }

    void Associate(const HANDLE handle, const std::uintptr_t completion_key)
    {
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        {
            throw std::invalid_argument("IOCP에 연결할 handle이 유효하지 않습니다");
        }
        if (IsReservedCompletionKey(completion_key))
        {
            throw std::invalid_argument(
                "IOCP runtime이 예약한 completion key는 사용할 수 없습니다");
        }

        DWORD error = ERROR_SUCCESS;
        {
            std::lock_guard lock(lifecycle_mutex_);
            if (stopping_)
            {
                throw std::logic_error(
                    "stop이 시작된 IoContext에는 handle을 연결할 수 없습니다");
            }

            if (::CreateIoCompletionPort(
                    handle,
                    port_,
                    static_cast<ULONG_PTR>(completion_key),
                    0) == nullptr)
            {
                error = ::GetLastError();
            }
        }

        const std::string key_text = std::to_string(completion_key);
        if (error != ERROR_SUCCESS)
        {
            const std::string error_text = std::to_string(error);
            logger_->Log(
                core::LogLevel::Error,
                "iocp.associate_failed",
                "native handle을 IO completion port에 연결하지 못했습니다.",
                {
                    {"completion_key", key_text},
                    {"win32_error", error_text},
                });
            throw std::system_error(
                static_cast<int>(error),
                std::system_category(),
                "CreateIoCompletionPort(associate)");
        }

        logger_->Log(
            core::LogLevel::Debug,
            "iocp.handle_associated",
            "native handle을 IO completion port에 연결했습니다.",
            {{"completion_key", key_text}});
    }

    bool PostTask(std::function<void()> task)
    {
        if (!task)
        {
            throw std::invalid_argument(
                "IOCP에 제출할 task는 비어 있을 수 없습니다");
        }

        auto packet = std::make_unique<QueuedTaskPacket>(std::move(task));
        DWORD error = ERROR_SUCCESS;
        {
            // task packet과 stop packet 순서를 같은 lock으로 직렬화한다.
            std::lock_guard lock(lifecycle_mutex_);
            if (stopping_)
            {
                return false;
            }

            if (!::PostQueuedCompletionStatus(
                    port_,
                    0,
                    kTaskCompletionKey,
                    reinterpret_cast<OVERLAPPED*>(packet.get())))
            {
                error = ::GetLastError();
            }
            else
            {
                packet.release();
            }
        }

        if (error != ERROR_SUCCESS)
        {
            const std::string error_text = std::to_string(error);
            logger_->Log(
                core::LogLevel::Error,
                "iocp.task_post_failed",
                "IOCP task packet 등록에 실패했습니다.",
                {{"win32_error", error_text}});
            throw std::system_error(
                static_cast<int>(error),
                std::system_category(),
                "PostQueuedCompletionStatus(task)");
        }
        return true;
    }

    void RunWorker(const std::size_t worker_index) noexcept
    {
        const std::string worker_text = std::to_string(worker_index);
        logger_->Log(
            core::LogLevel::Debug,
            "iocp.worker_started",
            "IOCP worker를 시작했습니다.",
            {{"worker_index", worker_text}});

        HANDLE port = nullptr;
        {
            std::lock_guard lock(lifecycle_mutex_);
            port = port_;
        }

        if (port == nullptr)
        {
            logger_->Log(
                core::LogLevel::Warning,
                "iocp.worker_without_port",
                "completion port가 닫힌 상태에서 worker가 시작됐습니다.",
                {{"worker_index", worker_text}});
            return;
        }

        for (;;)
        {
            DWORD transferred_bytes = 0;
            ULONG_PTR completion_key = 0;
            OVERLAPPED* overlapped = nullptr;

            const BOOL succeeded = ::GetQueuedCompletionStatus(
                port,
                &transferred_bytes,
                &completion_key,
                &overlapped,
                INFINITE);
            const DWORD error = succeeded ? ERROR_SUCCESS : ::GetLastError();

            if (completion_key == kTaskCompletionKey &&
                overlapped != nullptr)
            {
                auto packet = std::unique_ptr<QueuedTaskPacket>(
                    reinterpret_cast<QueuedTaskPacket*>(overlapped));
                try
                {
                    packet->Execute();
                }
                catch (const std::exception& exception)
                {
                    logger_->Log(
                        core::LogLevel::Critical,
                        "iocp.task_packet_failed",
                        "IOCP task packet 경계 밖으로 예외가 전파됐습니다.",
                        {{"exception", exception.what()}});
                }
                catch (...)
                {
                    logger_->Log(
                        core::LogLevel::Critical,
                        "iocp.task_packet_failed",
                        "IOCP task packet 경계 밖으로 알 수 없는 예외가 전파됐습니다.");
                }
                continue;
            }

            if (overlapped == nullptr)
            {
                if (succeeded && completion_key == kStopCompletionKey)
                {
                    logger_->Log(
                        core::LogLevel::Debug,
                        "iocp.worker_stopped",
                        "IOCP worker가 stop packet을 받고 종료합니다.",
                        {{"worker_index", worker_text}});
                    return;
                }

                if (!succeeded)
                {
                    const std::string error_text = std::to_string(error);
                    logger_->Log(
                        core::LogLevel::Error,
                        "iocp.dequeue_failed",
                        "completion dequeue에 실패해 worker를 종료합니다.",
                        {
                            {"worker_index", worker_text},
                            {"win32_error", error_text},
                        });
                    return;
                }

                const std::string key_text =
                    std::to_string(static_cast<std::uintptr_t>(completion_key));
                logger_->Log(
                    core::LogLevel::Warning,
                    "iocp.unexpected_packet",
                    "operation이 없는 completion packet을 무시했습니다.",
                    {
                        {"worker_index", worker_text},
                        {"completion_key", key_text},
                    });
                continue;
            }

            // GQCS가 FALSE를 반환해도 OVERLAPPED가 non-null이면 실패한 I/O의
            // completion이다. 이 경로에서도 operation ownership을 회수한다.
            auto operation = std::unique_ptr<CompletionOperation>(
                CompletionOperation::FromNative(overlapped));

            operation->Complete(
                static_cast<std::uint32_t>(transferred_bytes),
                succeeded
                    ? std::error_code{}
                    : std::error_code(
                          static_cast<int>(error),
                          std::system_category()),
                static_cast<std::uintptr_t>(completion_key));
        }
    }

    void Stop() noexcept
    {
        DWORD post_error = ERROR_SUCCESS;
        HANDLE port_to_close = nullptr;

        {
            std::lock_guard lock(lifecycle_mutex_);
            if (stopping_)
            {
                return;
            }

            stopping_ = true;
            for (std::size_t index = 0; index < worker_count_; ++index)
            {
                if (!::PostQueuedCompletionStatus(
                        port_,
                        0,
                        kStopCompletionKey,
                        nullptr))
                {
                    post_error = ::GetLastError();
                    port_to_close = std::exchange(port_, nullptr);
                    break;
                }
            }
        }

        if (port_to_close != nullptr)
        {
            ::CloseHandle(port_to_close);
        }

        if (post_error != ERROR_SUCCESS)
        {
            const std::string error_text = std::to_string(post_error);
            logger_->Log(
                core::LogLevel::Critical,
                "iocp.stop_packet_failed",
                "worker stop packet 등록에 실패해 completion port를 닫았습니다.",
                {{"win32_error", error_text}});
            return;
        }

        logger_->Log(
            core::LogLevel::Info,
            "iocp.stop_requested",
            "모든 IOCP worker에 stop packet을 등록했습니다.");
    }

    bool IsStopping() const noexcept
    {
        try
        {
            std::lock_guard lock(lifecycle_mutex_);
            return stopping_;
        }
        catch (...)
        {
            return true;
        }
    }

    void Close() noexcept
    {
        HANDLE port_to_close = nullptr;
        try
        {
            std::lock_guard lock(lifecycle_mutex_);
            stopping_ = true;
            port_to_close = std::exchange(port_, nullptr);
        }
        catch (...)
        {
            return;
        }

        if (port_to_close != nullptr)
        {
            ::CloseHandle(port_to_close);
        }
    }

private:
    const std::size_t worker_count_;
    std::shared_ptr<core::Logger> logger_;

    mutable std::mutex lifecycle_mutex_;
    HANDLE port_{nullptr};
    bool stopping_{false};
};

IoContext::IoContext(
    const std::size_t worker_count,
    std::shared_ptr<core::Logger> logger)
    : logger_(std::move(logger)),
      state_(std::make_shared<IoContextState>(worker_count, logger_))
{
    workers_.reserve(worker_count);

    try
    {
        for (std::size_t index = 0; index < worker_count; ++index)
        {
            workers_.emplace_back(
                [state = state_, index] { state->RunWorker(index); });
        }
    }
    catch (...)
    {
        state_->Stop();
        JoinNoThrow();
        state_->Close();
        throw;
    }

    const std::string worker_count_text = std::to_string(worker_count);
    logger_->Log(
        core::LogLevel::Info,
        "iocp.runtime_started",
        "IOCP runtime을 시작했습니다.",
        {{"worker_count", worker_count_text}});
}

IoContext::~IoContext()
{
    Stop();
    JoinNoThrow();
    state_->Close();
}

void IoContext::Associate(
    const HANDLE handle,
    const std::uintptr_t completion_key)
{
    state_->Associate(handle, completion_key);
}

void IoContext::Stop() noexcept
{
    state_->Stop();
}

void IoContext::Join()
{
    if (!state_->IsStopping())
    {
        throw std::logic_error("IoContext::Join 전에 Stop을 호출해야 합니다");
    }

    const std::thread::id caller = std::this_thread::get_id();
    for (const auto& worker : workers_)
    {
        if (worker.joinable() && worker.get_id() == caller)
        {
            throw std::logic_error(
                "IOCP worker는 자신이 속한 IoContext를 Join할 수 없습니다");
        }
    }

    for (auto& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    logger_->Log(
        core::LogLevel::Info,
        "iocp.workers_joined",
        "모든 IOCP worker가 종료됐습니다.");
}

bool IoContext::IsStopping() const noexcept
{
    return state_->IsStopping();
}

std::size_t IoContext::WorkerCount() const noexcept
{
    return workers_.size();
}

bool IoContext::PostTask(
    const std::shared_ptr<IoContextState>& state,
    std::function<void()> task)
{
    return state->PostTask(std::move(task));
}

void IoContext::JoinNoThrow() noexcept
{
    const std::thread::id caller = std::this_thread::get_id();
    for (auto& worker : workers_)
    {
        if (!worker.joinable())
        {
            continue;
        }

        if (worker.get_id() == caller)
        {
            logger_->Log(
                core::LogLevel::Critical,
                "iocp.self_destroy",
                "IOCP worker에서 IoContext가 파괴되어 현재 worker를 detach합니다.");
            worker.detach();
            continue;
        }

        try
        {
            worker.join();
        }
        catch (const std::exception& exception)
        {
            logger_->Log(
                core::LogLevel::Critical,
                "iocp.join_failed",
                "IOCP worker Join 중 예외가 발생했습니다.",
                {{"exception", exception.what()}});
            if (worker.joinable())
            {
                worker.detach();
            }
        }
        catch (...)
        {
            logger_->Log(
                core::LogLevel::Critical,
                "iocp.join_failed",
                "IOCP worker Join 중 알 수 없는 예외가 발생했습니다.");
            if (worker.joinable())
            {
                worker.detach();
            }
        }
    }
}

} // namespace iocp::runtime
