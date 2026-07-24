#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <type_traits>

namespace iocp::runtime
{

class CompletionOperation;

/// @brief Windows가 돌려준 `OVERLAPPED*`에서 C++ owner를 찾기 위한 context다.
///
/// `overlapped`는 반드시 첫 member여야 한다. IOCP가 반환한 pointer를 이
/// standard-layout context로 복구한 뒤 owner를 찾는다.
struct NativeOverlapped final
{
    OVERLAPPED overlapped{};
    CompletionOperation* owner{};
};

static_assert(std::is_standard_layout_v<NativeOverlapped>);
static_assert(offsetof(NativeOverlapped, overlapped) == 0);

/// @brief 하나의 accepted overlapped I/O가 completion될 때까지 필요한 상태다.
///
/// native API가 요청을 받아들인 뒤 caller가 ownership을 completion path로
/// 넘긴다. IOCP worker는 `OVERLAPPED*`에서 이 객체를 복구해 정확히 한 번
/// `Complete`를 호출하고 파괴한다.
class CompletionOperation
{
public:
    CompletionOperation() noexcept
    {
        native_.owner = this;
    }

    virtual ~CompletionOperation() = default;

    CompletionOperation(const CompletionOperation&) = delete;
    CompletionOperation& operator=(const CompletionOperation&) = delete;
    CompletionOperation(CompletionOperation&&) = delete;
    CompletionOperation& operator=(CompletionOperation&&) = delete;

    /// @brief overlapped I/O의 terminal 결과를 처리한다.
    ///
    /// @param transferred_bytes IOCP가 보고한 byte 수.
    /// @param error 성공이면 비어 있고 실패면 native error를 가진다.
    /// @param completion_key handle association 때 등록한 key.
    ///
    /// @par Execution
    /// `IoContext` worker에서 실행된다.
    ///
    /// @par Failure
    /// completion 경계를 넘어 예외를 전파하지 않아야 한다.
    virtual void Complete(
        std::uint32_t transferred_bytes,
        std::error_code error,
        std::uintptr_t completion_key) noexcept = 0;

    /// @brief Windows overlapped API에 전달할 안정적인 pointer를 반환한다.
    OVERLAPPED* NativeHandle() noexcept
    {
        return &native_.overlapped;
    }

    /// @brief IOCP가 반환한 pointer에서 operation owner를 복구한다.
    ///
    /// @pre `overlapped`는 이 class의 `NativeHandle`에서 얻은 non-null pointer다.
    static CompletionOperation* FromNative(OVERLAPPED* overlapped) noexcept
    {
        auto* native = reinterpret_cast<NativeOverlapped*>(overlapped);
        return native->owner;
    }

private:
    NativeOverlapped native_{};
};

} // namespace iocp::runtime
