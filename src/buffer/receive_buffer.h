/// @file bounded compacting linear buffer
#pragma once

#include "buffer/buffer_status.h"
#include "buffer/byte_view.h"

#include <cstddef>
#include <vector>

namespace iocp::buffer
{

struct ReceiveBufferSnapshot final
{
    std::size_t capacity{};
    std::size_t maximum_capacity{};
    std::size_t readable_bytes{};
    std::size_t writable_bytes{};
};

/// @brief protocol session이 소유하는 bounded compacting linear buffer다.
///
/// 이 component는 thread-safe하지 않다. 반환된 view는 다음 mutation 전까지만
/// 유효하며 `Append`의 source는 이 buffer의 storage와 겹치면 안 된다.
class ReceiveBuffer final
{
public:
    ReceiveBuffer(
        std::size_t initial_capacity,
        std::size_t maximum_capacity);

    ReceiveBuffer(const ReceiveBuffer&) = delete;
    ReceiveBuffer& operator=(const ReceiveBuffer&) = delete;
    ReceiveBuffer(ReceiveBuffer&&) = delete;
    ReceiveBuffer& operator=(ReceiveBuffer&&) = delete;

    ByteView ReadableView() const noexcept;
    MutableByteView WritableView() noexcept;

    /// @brief tail에 최소 `minimum_bytes`를 쓸 수 있게 compact/grow한다.
    ///
    /// limit 초과 시 buffer 내용과 offset은 변경하지 않는다.
    BufferStatus EnsureWritable(std::size_t minimum_bytes);

    /// @brief writable view에 기록된 byte를 readable 영역에 반영한다.
    ///
    /// @throws std::out_of_range 현재 writable 영역보다 큰 경우.
    void Commit(std::size_t bytes);

    /// @brief readable 영역 앞에서 byte를 소비한다.
    ///
    /// @throws std::out_of_range 현재 readable 영역보다 큰 경우.
    void Consume(std::size_t bytes);

    /// @brief 외부 byte를 buffer tail에 복사한다.
    BufferStatus Append(ByteView bytes);

    void Clear() noexcept;

    std::size_t ReadableBytes() const noexcept;
    std::size_t WritableBytes() const noexcept;
    std::size_t Capacity() const noexcept;
    std::size_t MaximumCapacity() const noexcept;
    bool Empty() const noexcept;
    ReceiveBufferSnapshot Snapshot() const noexcept;

private:
    void Compact() noexcept;

    std::vector<std::byte> storage_;
    std::size_t maximum_capacity_{};
    std::size_t read_offset_{};
    std::size_t write_offset_{};
};

} // namespace iocp::buffer
