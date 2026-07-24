#include "buffer/ring_receive_buffer.h"

#include <algorithm>
#include <stdexcept>

namespace iocp::buffer
{

RingReceiveBuffer::RingReceiveBuffer(
    const std::size_t initial_capacity,
    const std::size_t maximum_capacity)
    : storage_(initial_capacity),
      maximum_capacity_(maximum_capacity)
{
    if (initial_capacity == 0)
    {
        throw std::invalid_argument(
            "ring receive buffer 초기 용량은 1 이상이어야 합니다");
    }
    if (initial_capacity > maximum_capacity)
    {
        throw std::invalid_argument(
            "ring receive buffer 초기 용량은 최대 용량 이하여야 합니다");
    }
}

BufferSequence RingReceiveBuffer::ReadableSequence() const noexcept
{
    if (readable_bytes_ == 0)
    {
        return {};
    }

    const std::size_t first_bytes = std::min(
        readable_bytes_,
        storage_.size() - read_offset_);
    const std::size_t second_bytes =
        readable_bytes_ - first_bytes;
    return BufferSequence(
        ByteView(storage_.data() + read_offset_, first_bytes),
        ByteView(storage_.data(), second_bytes));
}

MutableBufferSequence RingReceiveBuffer::WritableSequence() noexcept
{
    const std::size_t writable_bytes = WritableBytes();
    if (writable_bytes == 0)
    {
        return {};
    }

    const std::size_t write_offset = WriteOffset();
    const std::size_t first_bytes = std::min(
        writable_bytes,
        storage_.size() - write_offset);
    const std::size_t second_bytes =
        writable_bytes - first_bytes;
    return MutableBufferSequence(
        MutableByteView(storage_.data() + write_offset, first_bytes),
        MutableByteView(storage_.data(), second_bytes));
}

BufferStatus RingReceiveBuffer::EnsureWritable(
    const std::size_t minimum_bytes)
{
    if (minimum_bytes <= WritableBytes())
    {
        return BufferStatus::Ready;
    }
    if (minimum_bytes > maximum_capacity_ - readable_bytes_)
    {
        return BufferStatus::LimitExceeded;
    }

    const std::size_t required_capacity =
        readable_bytes_ + minimum_bytes;
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

    Grow(std::max(next_capacity, required_capacity));
    return BufferStatus::Ready;
}

void RingReceiveBuffer::Commit(const std::size_t bytes)
{
    if (bytes > WritableBytes())
    {
        throw std::out_of_range(
            "ring receive buffer writable 영역보다 많이 commit할 수 없습니다");
    }
    readable_bytes_ += bytes;
}

void RingReceiveBuffer::Consume(const std::size_t bytes)
{
    if (bytes > readable_bytes_)
    {
        throw std::out_of_range(
            "ring receive buffer readable 영역보다 많이 consume할 수 없습니다");
    }

    if (bytes == readable_bytes_)
    {
        read_offset_ = 0;
        readable_bytes_ = 0;
        return;
    }

    read_offset_ =
        (read_offset_ + bytes) % storage_.size();
    readable_bytes_ -= bytes;
}

BufferStatus RingReceiveBuffer::Append(const ByteView bytes)
{
    const BufferStatus status = EnsureWritable(bytes.Size());
    if (status != BufferStatus::Ready)
    {
        return status;
    }

    WritableSequence().CopyFrom(bytes);
    Commit(bytes.Size());
    return BufferStatus::Ready;
}

void RingReceiveBuffer::Clear() noexcept
{
    read_offset_ = 0;
    readable_bytes_ = 0;
}

std::size_t RingReceiveBuffer::ReadableBytes() const noexcept
{
    return readable_bytes_;
}

std::size_t RingReceiveBuffer::WritableBytes() const noexcept
{
    return storage_.size() - readable_bytes_;
}

std::size_t RingReceiveBuffer::Capacity() const noexcept
{
    return storage_.size();
}

std::size_t RingReceiveBuffer::MaximumCapacity() const noexcept
{
    return maximum_capacity_;
}

bool RingReceiveBuffer::Empty() const noexcept
{
    return readable_bytes_ == 0;
}

RingReceiveBufferSnapshot RingReceiveBuffer::Snapshot() const noexcept
{
    const std::size_t writable_bytes = WritableBytes();
    std::size_t writable_segments = 0;
    if (writable_bytes != 0)
    {
        const std::size_t first_bytes = std::min(
            writable_bytes,
            storage_.size() - WriteOffset());
        writable_segments =
            first_bytes == writable_bytes ? 1 : 2;
    }

    return RingReceiveBufferSnapshot{
        Capacity(),
        MaximumCapacity(),
        ReadableBytes(),
        writable_bytes,
        ReadableSequence().SegmentCount(),
        writable_segments,
    };
}

std::size_t RingReceiveBuffer::WriteOffset() const noexcept
{
    return (read_offset_ + readable_bytes_) % storage_.size();
}

void RingReceiveBuffer::Grow(const std::size_t capacity)
{
    std::vector<std::byte> next(capacity);
    ReadableSequence().CopyTo(
        0,
        MutableByteView(next.data(), readable_bytes_));
    storage_.swap(next);
    read_offset_ = 0;
}

} // namespace iocp::buffer
