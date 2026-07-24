#pragma once

#include <cstddef>
#include <stdexcept>

namespace iocp::buffer
{

/// @brief 소유권이 없는 연속 byte 범위를 읽기 전용으로 참조한다.
///
/// view는 원본 storage의 수명을 연장하지 않는다. 원본이 파괴되거나
/// 재할당되면 이 view도 무효가 된다.
class ByteView final
{
public:
    static constexpr std::size_t npos =
        static_cast<std::size_t>(-1);

    constexpr ByteView() noexcept = default;

    ByteView(const std::byte* data, const std::size_t size)
        : data_(data),
          size_(size)
    {
        if (data_ == nullptr && size_ != 0)
        {
            throw std::invalid_argument(
                "null byte pointer는 비어 있지 않은 view를 만들 수 없습니다");
        }
    }

    constexpr const std::byte* Data() const noexcept
    {
        return data_;
    }

    constexpr std::size_t Size() const noexcept
    {
        return size_;
    }

    constexpr bool Empty() const noexcept
    {
        return size_ == 0;
    }

    constexpr const std::byte* begin() const noexcept
    {
        return data_;
    }

    constexpr const std::byte* end() const noexcept
    {
        return size_ == 0 ? data_ : data_ + size_;
    }

    constexpr const std::byte& operator[](
        const std::size_t index) const noexcept
    {
        return data_[index];
    }

    ByteView SubView(
        const std::size_t offset,
        const std::size_t count = npos) const
    {
        if (offset > size_)
        {
            throw std::out_of_range("ByteView offset이 범위를 벗어났습니다");
        }

        const std::size_t remaining = size_ - offset;
        const std::size_t result_size =
            count == npos ? remaining : count;
        if (result_size > remaining)
        {
            throw std::out_of_range("ByteView count가 범위를 벗어났습니다");
        }

        return ByteView(
            result_size == 0 && data_ == nullptr
                ? nullptr
                : data_ + offset,
            result_size);
    }

private:
    const std::byte* data_{};
    std::size_t size_{};
};

/// @brief 소유권이 없는 연속 byte 범위를 쓰기 가능하게 참조한다.
class MutableByteView final
{
public:
    static constexpr std::size_t npos = ByteView::npos;

    constexpr MutableByteView() noexcept = default;

    MutableByteView(std::byte* data, const std::size_t size)
        : data_(data),
          size_(size)
    {
        if (data_ == nullptr && size_ != 0)
        {
            throw std::invalid_argument(
                "null byte pointer는 비어 있지 않은 view를 만들 수 없습니다");
        }
    }

    constexpr std::byte* Data() const noexcept
    {
        return data_;
    }

    constexpr std::size_t Size() const noexcept
    {
        return size_;
    }

    constexpr bool Empty() const noexcept
    {
        return size_ == 0;
    }

    constexpr std::byte* begin() const noexcept
    {
        return data_;
    }

    constexpr std::byte* end() const noexcept
    {
        return size_ == 0 ? data_ : data_ + size_;
    }

    constexpr std::byte& operator[](
        const std::size_t index) const noexcept
    {
        return data_[index];
    }

    MutableByteView SubView(
        const std::size_t offset,
        const std::size_t count = npos) const
    {
        if (offset > size_)
        {
            throw std::out_of_range(
                "MutableByteView offset이 범위를 벗어났습니다");
        }

        const std::size_t remaining = size_ - offset;
        const std::size_t result_size =
            count == npos ? remaining : count;
        if (result_size > remaining)
        {
            throw std::out_of_range(
                "MutableByteView count가 범위를 벗어났습니다");
        }

        return MutableByteView(
            result_size == 0 && data_ == nullptr
                ? nullptr
                : data_ + offset,
            result_size);
    }

    ByteView AsReadOnly() const noexcept
    {
        return ByteView(data_, size_);
    }

private:
    std::byte* data_{};
    std::size_t size_{};
};

} // namespace iocp::buffer
