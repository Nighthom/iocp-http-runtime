#include "buffer/buffer_sequence.h"
#include "buffer/byte_view.h"
#include "buffer/receive_buffer.h"
#include "buffer/ring_receive_buffer.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using iocp::buffer::BufferStatus;
using iocp::buffer::BufferSequence;
using iocp::buffer::ByteView;
using iocp::buffer::MutableBufferSequence;
using iocp::buffer::MutableByteView;
using iocp::buffer::ReceiveBuffer;
using iocp::buffer::RingReceiveBuffer;

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::vector<std::byte> ToBytes(const std::string& text)
{
    const auto* first =
        reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(first, first + text.size());
}

ByteView ViewOf(const std::vector<std::byte>& bytes)
{
    return ByteView(bytes.data(), bytes.size());
}

std::string ToString(const ByteView bytes)
{
    return std::string(
        reinterpret_cast<const char*>(bytes.Data()),
        bytes.Size());
}

std::string ToString(const BufferSequence bytes)
{
    std::vector<std::byte> flattened(bytes.Size());
    bytes.CopyTo(
        0,
        MutableByteView(flattened.data(), flattened.size()));
    return ToString(ViewOf(flattened));
}

template <typename Action>
bool ThrowsOutOfRange(Action action)
{
    try
    {
        action();
        return false;
    }
    catch (const std::out_of_range&)
    {
        return true;
    }
}

void TestByteViews()
{
    std::array<std::byte, 4> bytes{
        std::byte{'a'},
        std::byte{'b'},
        std::byte{'c'},
        std::byte{'d'},
    };

    MutableByteView writable(bytes.data(), bytes.size());
    writable[1] = std::byte{'B'};
    const ByteView readable = writable.AsReadOnly();
    Check(readable.Size() == 4, "read-only view 크기가 다릅니다");
    Check(
        ToString(readable) == "aBcd",
        "mutable view 변경이 read-only view에 반영되지 않았습니다");
    Check(
        ToString(readable.SubView(1, 2)) == "Bc",
        "ByteView subview 결과가 다릅니다");
    Check(
        writable.SubView(4).Empty(),
        "끝 offset의 MutableByteView가 비어 있지 않습니다");

    Check(
        ThrowsOutOfRange([&] { readable.SubView(5); }),
        "범위를 벗어난 ByteView offset이 허용됐습니다");
    Check(
        ThrowsOutOfRange([&] { writable.SubView(3, 2); }),
        "범위를 벗어난 MutableByteView count가 허용됐습니다");

    bool invalid_null_rejected = false;
    try
    {
        ByteView invalid(nullptr, 1);
        static_cast<void>(invalid);
    }
    catch (const std::invalid_argument&)
    {
        invalid_null_rejected = true;
    }
    Check(
        invalid_null_rejected,
        "null pointer로 비어 있지 않은 ByteView를 만들었습니다");
}

void TestBufferSequences()
{
    const auto first = ToBytes("abc");
    const auto second = ToBytes("defg");
    const BufferSequence readable(ViewOf(first), ViewOf(second));

    Check(
        readable.SegmentCount() == 2 &&
            readable.Size() == 7 &&
            ToString(readable) == "abcdefg",
        "두 span BufferSequence의 logical byte가 다릅니다");
    Check(
        readable.At(2) == std::byte{'c'} &&
            readable.At(3) == std::byte{'d'},
        "BufferSequence span 경계의 byte lookup이 다릅니다");

    std::array<std::byte, 5> copied{};
    readable.CopyTo(
        1,
        MutableByteView(copied.data(), copied.size()));
    Check(
        ToString(ByteView(copied.data(), copied.size())) == "bcdef",
        "BufferSequence offset copy 결과가 다릅니다");
    Check(
        ThrowsOutOfRange([&] { readable.At(7); }) &&
            ThrowsOutOfRange([&] { readable.Segment(2); }),
        "BufferSequence 범위 검증이 동작하지 않습니다");

    std::array<std::byte, 2> left{};
    std::array<std::byte, 3> right{};
    const MutableBufferSequence writable(
        MutableByteView(left.data(), left.size()),
        MutableByteView(right.data(), right.size()));
    const auto source = ToBytes("hello");
    writable.CopyFrom(ViewOf(source));
    Check(
        writable.SegmentCount() == 2 &&
            ToString(writable.AsReadOnly()) == "hello",
        "MutableBufferSequence cross-span copy 결과가 다릅니다");
}

void TestReceiveBufferCommitConsumeAndCompact()
{
    ReceiveBuffer buffer(8, 16);
    auto first = ToBytes("abcdef");
    Check(
        buffer.Append(ViewOf(first)) == BufferStatus::Ready,
        "첫 append에 실패했습니다");
    Check(
        ToString(buffer.ReadableView()) == "abcdef",
        "첫 append 내용이 다릅니다");

    buffer.Consume(4);
    Check(
        ToString(buffer.ReadableView()) == "ef",
        "consume 이후 readable 내용이 다릅니다");

    auto second = ToBytes("ghij");
    Check(
        buffer.Append(ViewOf(second)) == BufferStatus::Ready,
        "compact가 필요한 append에 실패했습니다");
    Check(
        buffer.Capacity() == 8,
        "grow 없이 가능한 append가 capacity를 늘렸습니다");
    Check(
        ToString(buffer.ReadableView()) == "efghij",
        "compact 이후 byte 순서가 깨졌습니다");

    buffer.Consume(buffer.ReadableBytes());
    Check(buffer.Empty(), "전체 consume 이후 buffer가 비지 않았습니다");
    Check(
        buffer.WritableBytes() == buffer.Capacity(),
        "전체 consume 이후 offset이 초기화되지 않았습니다");

    MutableByteView writable = buffer.WritableView();
    const char* text = "xyz";
    std::memcpy(writable.Data(), text, 3);
    buffer.Commit(3);
    Check(
        ToString(buffer.ReadableView()) == "xyz",
        "WritableView/Commit 결과가 다릅니다");
    Check(
        ThrowsOutOfRange([&] { buffer.Consume(4); }),
        "readable 영역을 넘는 consume이 허용됐습니다");
}

void TestReceiveBufferGrowthAndLimit()
{
    ReceiveBuffer buffer(4, 10);
    auto first = ToBytes("abcd");
    auto second = ToBytes("efg");
    auto third = ToBytes("hi");
    auto overflow = ToBytes("jk");

    Check(
        buffer.Append(ViewOf(first)) == BufferStatus::Ready,
        "초기 capacity append에 실패했습니다");
    Check(
        buffer.Append(ViewOf(second)) == BufferStatus::Ready &&
            buffer.Capacity() == 8,
        "receive buffer 첫 grow 결과가 다릅니다");
    Check(
        buffer.Append(ViewOf(third)) == BufferStatus::Ready &&
            buffer.Capacity() == 10,
        "receive buffer maximum capacity grow 결과가 다릅니다");

    const std::string before = ToString(buffer.ReadableView());
    const auto before_snapshot = buffer.Snapshot();
    Check(
        buffer.Append(ViewOf(overflow)) == BufferStatus::LimitExceeded,
        "receive buffer limit 초과가 보고되지 않았습니다");
    const auto after_snapshot = buffer.Snapshot();
    Check(
        ToString(buffer.ReadableView()) == before &&
            after_snapshot.capacity == before_snapshot.capacity &&
            after_snapshot.readable_bytes ==
                before_snapshot.readable_bytes,
        "limit 초과가 buffer 상태를 변경했습니다");
    Check(
        buffer.EnsureWritable(std::numeric_limits<std::size_t>::max()) ==
            BufferStatus::LimitExceeded,
        "overflow 크기의 writable 요청이 거부되지 않았습니다");
}

void TestReceiveBufferValidation()
{
    bool zero_initial_rejected = false;
    try
    {
        ReceiveBuffer invalid(0, 4);
        static_cast<void>(invalid);
    }
    catch (const std::invalid_argument&)
    {
        zero_initial_rejected = true;
    }
    Check(
        zero_initial_rejected,
        "0인 receive buffer 초기 용량이 허용됐습니다");

    bool inverted_limit_rejected = false;
    try
    {
        ReceiveBuffer invalid(8, 4);
        static_cast<void>(invalid);
    }
    catch (const std::invalid_argument&)
    {
        inverted_limit_rejected = true;
    }
    Check(
        inverted_limit_rejected,
        "최대보다 큰 receive buffer 초기 용량이 허용됐습니다");

    ReceiveBuffer buffer(4, 4);
    Check(
        ThrowsOutOfRange([&] { buffer.Commit(5); }),
        "writable 영역을 넘는 commit이 허용됐습니다");
}

void TestReceiveBufferAgainstReferenceModel()
{
    constexpr std::size_t kMaximumCapacity = 64;
    ReceiveBuffer buffer(4, kMaximumCapacity);
    std::vector<std::byte> model;
    std::mt19937 random(0x10C0FFEEu);
    std::uint32_t next_value = 0;

    for (int step = 0; step < 2000; ++step)
    {
        const bool should_append =
            model.empty() || (random() % 3) != 0;
        if (should_append)
        {
            const std::size_t size = 1 + (random() % 9);
            std::vector<std::byte> input(size);
            for (auto& value : input)
            {
                value = static_cast<std::byte>(next_value++ & 0xffu);
            }

            const bool fits =
                size <= kMaximumCapacity - model.size();
            const BufferStatus status = buffer.Append(ViewOf(input));
            Check(
                (status == BufferStatus::Ready) == fits,
                "reference model과 append limit 결과가 다릅니다");
            if (fits)
            {
                model.insert(model.end(), input.begin(), input.end());
            }
        }
        else
        {
            const std::size_t size =
                1 + (random() % model.size());
            buffer.Consume(size);
            model.erase(model.begin(), model.begin() + size);
        }

        const ByteView actual = buffer.ReadableView();
        Check(
            actual.Size() == model.size(),
            "reference model과 readable 크기가 다릅니다");
        Check(
            std::equal(actual.begin(), actual.end(), model.begin()),
            "reference model과 readable byte가 다릅니다");
        Check(
            buffer.Capacity() <= kMaximumCapacity,
            "receive buffer가 maximum capacity를 넘었습니다");
    }

    buffer.Clear();
    Check(
        buffer.Empty() && buffer.WritableBytes() == buffer.Capacity(),
        "Clear가 receive buffer offset을 초기화하지 않았습니다");
}

void TestRingReceiveBufferWrapGrowAndLimit()
{
    RingReceiveBuffer buffer(8, 16);
    const auto first = ToBytes("abcdef");
    Check(
        buffer.Append(ViewOf(first)) == BufferStatus::Ready,
        "ring 첫 append에 실패했습니다");

    buffer.Consume(5);
    const auto second = ToBytes("ghijkl");
    Check(
        buffer.Append(ViewOf(second)) == BufferStatus::Ready,
        "ring wrap append에 실패했습니다");
    Check(
        buffer.Capacity() == 8 &&
            buffer.ReadableSequence().SegmentCount() == 2 &&
            ToString(buffer.ReadableSequence()) == "fghijkl",
        "ring wrap 이후 logical byte sequence가 다릅니다");

    buffer.Consume(4);
    Check(
        buffer.EnsureWritable(8) == BufferStatus::Ready &&
            buffer.Capacity() == 16 &&
            ToString(buffer.ReadableSequence()) == "jkl",
        "ring grow가 readable byte를 보존하지 못했습니다");

    const auto third = ToBytes("mnopqrst");
    Check(
        buffer.Append(ViewOf(third)) == BufferStatus::Ready &&
            ToString(buffer.ReadableSequence()) == "jklmnopqrst",
        "ring grow 이후 append 결과가 다릅니다");

    const auto before = buffer.Snapshot();
    const auto overflow = ToBytes("uvwxyz");
    Check(
        buffer.Append(ViewOf(overflow)) == BufferStatus::LimitExceeded,
        "ring maximum capacity 초과가 거부되지 않았습니다");
    const auto after = buffer.Snapshot();
    Check(
        ToString(buffer.ReadableSequence()) == "jklmnopqrst" &&
            before.capacity == after.capacity &&
            before.readable_bytes == after.readable_bytes,
        "ring limit 초과가 기존 상태를 변경했습니다");

    RingReceiveBuffer committed(4, 4);
    const auto value = ToBytes("xyz");
    committed.WritableSequence().CopyFrom(ViewOf(value));
    committed.Commit(value.size());
    Check(
        ToString(committed.ReadableSequence()) == "xyz",
        "ring WritableSequence/Commit 결과가 다릅니다");
    Check(
        ThrowsOutOfRange([&] { committed.Commit(2); }) &&
            ThrowsOutOfRange([&] { committed.Consume(4); }),
        "ring commit/consume 범위 검증이 동작하지 않습니다");
}

void TestRingReceiveBufferValidation()
{
    bool zero_initial_rejected = false;
    try
    {
        RingReceiveBuffer invalid(0, 4);
        static_cast<void>(invalid);
    }
    catch (const std::invalid_argument&)
    {
        zero_initial_rejected = true;
    }
    Check(
        zero_initial_rejected,
        "0인 ring buffer 초기 용량이 허용됐습니다");

    bool inverted_limit_rejected = false;
    try
    {
        RingReceiveBuffer invalid(8, 4);
        static_cast<void>(invalid);
    }
    catch (const std::invalid_argument&)
    {
        inverted_limit_rejected = true;
    }
    Check(
        inverted_limit_rejected,
        "최대보다 큰 ring buffer 초기 용량이 허용됐습니다");
}

void TestRingReceiveBufferAgainstReferenceModel()
{
    constexpr std::size_t kMaximumCapacity = 64;
    RingReceiveBuffer buffer(4, kMaximumCapacity);
    std::vector<std::byte> model;
    std::mt19937 random(0xB00FFEEu);
    std::uint32_t next_value = 0;
    bool observed_segmented_read = false;

    for (int step = 0; step < 2000; ++step)
    {
        const bool should_append =
            model.empty() || (random() % 3) != 0;
        if (should_append)
        {
            const std::size_t size = 1 + (random() % 9);
            std::vector<std::byte> input(size);
            for (auto& value : input)
            {
                value = static_cast<std::byte>(next_value++ & 0xffu);
            }

            const bool fits =
                size <= kMaximumCapacity - model.size();
            const BufferStatus status = buffer.Append(ViewOf(input));
            Check(
                (status == BufferStatus::Ready) == fits,
                "ring reference model과 append limit 결과가 다릅니다");
            if (fits)
            {
                model.insert(model.end(), input.begin(), input.end());
            }
        }
        else
        {
            const std::size_t size =
                1 + (random() % model.size());
            buffer.Consume(size);
            model.erase(model.begin(), model.begin() + size);
        }

        const BufferSequence actual = buffer.ReadableSequence();
        observed_segmented_read =
            observed_segmented_read || actual.SegmentCount() == 2;
        std::vector<std::byte> flattened(actual.Size());
        actual.CopyTo(
            0,
            MutableByteView(flattened.data(), flattened.size()));
        Check(
            flattened == model,
            "ring reference model과 logical byte가 다릅니다");
        Check(
            buffer.Capacity() <= kMaximumCapacity &&
                actual.SegmentCount() <= 2,
            "ring capacity 또는 segment invariant가 깨졌습니다");
    }

    Check(
        observed_segmented_read,
        "ring reference test가 wrap된 readable sequence를 만들지 못했습니다");
    buffer.Clear();
    Check(
        buffer.Empty() && buffer.WritableBytes() == buffer.Capacity(),
        "ring Clear가 offset을 초기화하지 않았습니다");
}

template <typename Test>
bool RunTest(const char* name, Test test)
{
    try
    {
        test();
        std::cout << "[통과] " << name << '\n';
        return true;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[실패] " << name << ": " << exception.what() << '\n';
        return false;
    }
    catch (...)
    {
        std::cerr << "[실패] " << name << ": 알 수 없는 예외\n";
        return false;
    }
}

} // namespace

int main()
{
    ::SetConsoleOutputCP(CP_UTF8);

    int failures = 0;
    failures += !RunTest("ByteView", TestByteViews);
    failures += !RunTest("BufferSequence", TestBufferSequences);
    failures += !RunTest(
        "ReceiveBuffer commit/consume/compact",
        TestReceiveBufferCommitConsumeAndCompact);
    failures += !RunTest(
        "ReceiveBuffer growth/limit",
        TestReceiveBufferGrowthAndLimit);
    failures += !RunTest(
        "ReceiveBuffer validation",
        TestReceiveBufferValidation);
    failures += !RunTest(
        "ReceiveBuffer reference model",
        TestReceiveBufferAgainstReferenceModel);
    failures += !RunTest(
        "RingReceiveBuffer wrap/grow/limit",
        TestRingReceiveBufferWrapGrowAndLimit);
    failures += !RunTest(
        "RingReceiveBuffer validation",
        TestRingReceiveBufferValidation);
    failures += !RunTest(
        "RingReceiveBuffer reference model",
        TestRingReceiveBufferAgainstReferenceModel);

    if (failures == 0)
    {
        std::cout << "Buffer 테스트를 모두 통과했습니다.\n";
    }
    return failures == 0 ? 0 : 1;
}
