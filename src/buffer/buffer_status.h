/// @file buffer 연산 결과 상태 (Ready / LimitExceeded)
#pragma once

namespace iocp::buffer
{

enum class BufferStatus
{
    Ready,
    LimitExceeded,
};

} // namespace iocp::buffer
