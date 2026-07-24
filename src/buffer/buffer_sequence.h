#pragma once

#include "buffer/byte_view.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace iocp::buffer
{

/// @brief 최대 두 개의 연속 span을 하나의 logical byte sequence로 표현한다.
///
/// sequence는 storage를 소유하지 않는다. 원본 storage가 mutation되거나
/// 파괴되면 모든 segment도 무효가 된다.
class BufferSequence final
{
public:
    constexpr BufferSequence() noexcept = default;

    explicit constexpr BufferSequence(const ByteView first) noexcept
    {
        Add(first);
    }

    constexpr BufferSequence(
        const ByteView first,
        const ByteView second) noexcept
    {
        Add(first);
        Add(second);
    }

    constexpr std::size_t SegmentCount() const noexcept
    {
        return segment_count_;
    }

    constexpr std::size_t Size() const noexcept
    {
        return size_;
    }

    constexpr bool Empty() const noexcept
    {
        return size_ == 0;
    }

    ByteView Segment(const std::size_t index) const
    {
        if (index >= segment_count_)
        {
            throw std::out_of_range(
                "BufferSequence segment index가 범위를 벗어났습니다");
        }
        return segments_[index];
    }

    std::byte At(const std::size_t index) const
    {
        if (index >= size_)
        {
            throw std::out_of_range(
                "BufferSequence byte index가 범위를 벗어났습니다");
        }

        if (index < segments_[0].Size())
        {
            return segments_[0][index];
        }
        return segments_[1][index - segments_[0].Size()];
    }

    /// @brief logical sequence의 일부를 연속 destination에 복사한다.
    void CopyTo(
        const std::size_t offset,
        const MutableByteView destination) const
    {
        if (offset > size_ ||
            destination.Size() > size_ - offset)
        {
            throw std::out_of_range(
                "BufferSequence copy 범위가 readable byte를 넘습니다");
        }

        std::size_t source_offset = offset;
        std::size_t copied = 0;
        for (std::size_t index = 0;
             index < segment_count_ && copied < destination.Size();
             ++index)
        {
            const ByteView segment = segments_[index];
            if (source_offset >= segment.Size())
            {
                source_offset -= segment.Size();
                continue;
            }

            const std::size_t bytes = std::min(
                segment.Size() - source_offset,
                destination.Size() - copied);
            std::memcpy(
                destination.Data() + copied,
                segment.Data() + source_offset,
                bytes);
            copied += bytes;
            source_offset = 0;
        }
    }

private:
    constexpr void Add(const ByteView segment) noexcept
    {
        if (segment.Empty())
        {
            return;
        }
        segments_[segment_count_++] = segment;
        size_ += segment.Size();
    }

    std::array<ByteView, 2> segments_{};
    std::size_t segment_count_{};
    std::size_t size_{};
};

/// @brief 최대 두 writable span을 하나의 logical sequence로 표현한다.
class MutableBufferSequence final
{
public:
    constexpr MutableBufferSequence() noexcept = default;

    explicit constexpr MutableBufferSequence(
        const MutableByteView first) noexcept
    {
        Add(first);
    }

    constexpr MutableBufferSequence(
        const MutableByteView first,
        const MutableByteView second) noexcept
    {
        Add(first);
        Add(second);
    }

    constexpr std::size_t SegmentCount() const noexcept
    {
        return segment_count_;
    }

    constexpr std::size_t Size() const noexcept
    {
        return size_;
    }

    constexpr bool Empty() const noexcept
    {
        return size_ == 0;
    }

    MutableByteView Segment(const std::size_t index) const
    {
        if (index >= segment_count_)
        {
            throw std::out_of_range(
                "MutableBufferSequence segment index가 범위를 벗어났습니다");
        }
        return segments_[index];
    }

    /// @brief 연속 source를 logical writable sequence 앞에서부터 복사한다.
    void CopyFrom(const ByteView source) const
    {
        if (source.Size() > size_)
        {
            throw std::out_of_range(
                "MutableBufferSequence copy가 writable byte를 넘습니다");
        }

        std::size_t copied = 0;
        for (std::size_t index = 0;
             index < segment_count_ && copied < source.Size();
             ++index)
        {
            const MutableByteView segment = segments_[index];
            const std::size_t bytes = std::min(
                segment.Size(),
                source.Size() - copied);
            std::memcpy(
                segment.Data(),
                source.Data() + copied,
                bytes);
            copied += bytes;
        }
    }

    BufferSequence AsReadOnly() const noexcept
    {
        if (segment_count_ == 0)
        {
            return {};
        }
        if (segment_count_ == 1)
        {
            return BufferSequence(segments_[0].AsReadOnly());
        }
        return BufferSequence(
            segments_[0].AsReadOnly(),
            segments_[1].AsReadOnly());
    }

private:
    constexpr void Add(const MutableByteView segment) noexcept
    {
        if (segment.Empty())
        {
            return;
        }
        segments_[segment_count_++] = segment;
        size_ += segment.Size();
    }

    std::array<MutableByteView, 2> segments_{};
    std::size_t segment_count_{};
    std::size_t size_{};
};

} // namespace iocp::buffer
