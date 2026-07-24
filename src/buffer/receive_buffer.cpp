// bounded compacting linear buffer 구현
#include "buffer/receive_buffer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace iocp::buffer
{

ReceiveBuffer::ReceiveBuffer(
    const std::size_t initial_capacity,
    const std::size_t maximum_capacity)
    : storage_(initial_capacity),
      maximum_capacity_(maximum_capacity)
{
    if (initial_capacity == 0)
    {
        throw std::invalid_argument(
            "receive buffer 초기 용량은 1 이상이어야 합니다");
    }
    if (initial_capacity > maximum_capacity)
    {
        throw std::invalid_argument(
            "receive buffer 초기 용량은 최대 용량 이하여야 합니다");
    }
}

ByteView ReceiveBuffer::ReadableView() const noexcept
{
    return ByteView(
        storage_.data() + read_offset_,
        ReadableBytes());
}

MutableByteView ReceiveBuffer::WritableView() noexcept
{
    return MutableByteView(
        storage_.data() + write_offset_,
        WritableBytes());
}

BufferStatus ReceiveBuffer::EnsureWritable(
    const std::size_t minimum_bytes)
{
    if (minimum_bytes <= WritableBytes())
    {
        return BufferStatus::Ready;
    }

    const std::size_t readable_bytes = ReadableBytes();
    if (minimum_bytes > maximum_capacity_ - readable_bytes)
    {
        return BufferStatus::LimitExceeded;
    }

    if (read_offset_ != 0)
    {
        Compact();
        if (minimum_bytes <= WritableBytes())
        {
            return BufferStatus::Ready;
        }
    }

    // 용량이 부족하면 2배씩 키우되 maximum_capacity_ 절반을 넘으면
    // 바로 maximum_capacity_로 점프해 과도한 doubling을 방지한다.
    const std::size_t required_capacity =
        readable_bytes + minimum_bytes;
    std::size_t next_capacity = storage_.size();
    while (next_capacity < required_capacity)
    {
        if (next_capacity > maximum_capacity_ / 2)
        {
            next_capacity = maximum_capacity_;
            break;
        }
        next_capacity *= 2;
    }

    storage_.resize(std::max(next_capacity, required_capacity));
    return BufferStatus::Ready;
}

void ReceiveBuffer::Commit(const std::size_t bytes)
{
    if (bytes > WritableBytes())
    {
        throw std::out_of_range(
            "receive buffer writable 영역보다 많이 commit할 수 없습니다");
    }
    write_offset_ += bytes;
}

void ReceiveBuffer::Consume(const std::size_t bytes)
{
    if (bytes > ReadableBytes())
    {
        throw std::out_of_range(
            "receive buffer readable 영역보다 많이 consume할 수 없습니다");
    }

    read_offset_ += bytes;
    if (read_offset_ == write_offset_)
    {
        read_offset_ = 0;
        write_offset_ = 0;
    }
}

BufferStatus ReceiveBuffer::Append(const ByteView bytes)
{
    const BufferStatus status = EnsureWritable(bytes.Size());
    if (status != BufferStatus::Ready)
    {
        return status;
    }

    if (!bytes.Empty())
    {
        std::memcpy(
            storage_.data() + write_offset_,
            bytes.Data(),
            bytes.Size());
        write_offset_ += bytes.Size();
    }
    return BufferStatus::Ready;
}

void ReceiveBuffer::Clear() noexcept
{
    read_offset_ = 0;
    write_offset_ = 0;
}

std::size_t ReceiveBuffer::ReadableBytes() const noexcept
{
    return write_offset_ - read_offset_;
}

std::size_t ReceiveBuffer::WritableBytes() const noexcept
{
    return storage_.size() - write_offset_;
}

std::size_t ReceiveBuffer::Capacity() const noexcept
{
    return storage_.size();
}

std::size_t ReceiveBuffer::MaximumCapacity() const noexcept
{
    return maximum_capacity_;
}

bool ReceiveBuffer::Empty() const noexcept
{
    return ReadableBytes() == 0;
}

ReceiveBufferSnapshot ReceiveBuffer::Snapshot() const noexcept
{
    return ReceiveBufferSnapshot{
        Capacity(),
        MaximumCapacity(),
        ReadableBytes(),
        WritableBytes(),
    };
}

// readable byte를 storage 앞으로 memmove하여 write tail 공간을 확보한다.
// read_offset_이 0이면 호출되지 않으며, Consume 시 모든 byte를 소진했을
// 때는 offset 초기화만으로 compaction을 대신한다.
void ReceiveBuffer::Compact() noexcept
{
    const std::size_t readable_bytes = ReadableBytes();
    if (readable_bytes != 0)
    {
        std::memmove(
            storage_.data(),
            storage_.data() + read_offset_,
            readable_bytes);
    }
    read_offset_ = 0;
    write_offset_ = readable_bytes;
}

} // namespace iocp::buffer
