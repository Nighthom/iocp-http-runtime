#pragma once

#include "buffer/buffer_sequence.h"
#include "buffer/buffer_status.h"

#include <cstddef>
#include <vector>

namespace iocp::buffer
{

struct RingReceiveBufferSnapshot final
{
    std::size_t capacity{};
    std::size_t maximum_capacity{};
    std::size_t readable_bytes{};
    std::size_t writable_bytes{};
    std::size_t readable_segments{};
    std::size_t writable_segments{};
};

/// @brief protocol session이 소유하는 bounded ring receive storage다.
///
/// normal consume/append는 readable byte를 compact하지 않는다. grow할 때만
/// logical byte 순서를 새 storage로 복사한다. 반환된 sequence는 다음
/// mutation 전까지만 유효하며 Append source는 내부 storage와 겹치면 안 된다.
class RingReceiveBuffer final
{
public:
    RingReceiveBuffer(
        std::size_t initial_capacity,
        std::size_t maximum_capacity);

    RingReceiveBuffer(const RingReceiveBuffer&) = delete;
    RingReceiveBuffer& operator=(const RingReceiveBuffer&) = delete;
    RingReceiveBuffer(RingReceiveBuffer&&) = delete;
    RingReceiveBuffer& operator=(RingReceiveBuffer&&) = delete;

    BufferSequence ReadableSequence() const noexcept;
    MutableBufferSequence WritableSequence() noexcept;

    /// @brief 최소 writable byte를 확보하며 필요할 때만 grow한다.
    ///
    /// limit 초과 시 buffer 내용과 offset은 변경하지 않는다.
    BufferStatus EnsureWritable(std::size_t minimum_bytes);

    void Commit(std::size_t bytes);
    void Consume(std::size_t bytes);
    BufferStatus Append(ByteView bytes);
    void Clear() noexcept;

    std::size_t ReadableBytes() const noexcept;
    std::size_t WritableBytes() const noexcept;
    std::size_t Capacity() const noexcept;
    std::size_t MaximumCapacity() const noexcept;
    bool Empty() const noexcept;
    RingReceiveBufferSnapshot Snapshot() const noexcept;

private:
    std::size_t WriteOffset() const noexcept;
    void Grow(std::size_t capacity);

    std::vector<std::byte> storage_;
    std::size_t maximum_capacity_{};
    std::size_t read_offset_{};
    std::size_t readable_bytes_{};
};

} // namespace iocp::buffer
