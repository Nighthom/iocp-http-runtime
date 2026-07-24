#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace iocp::transport
{

using SharedSendBuffer =
    std::shared_ptr<const std::vector<std::byte>>;

struct SendSlice final
{
    SharedSendBuffer buffer;
    std::size_t offset{};
    std::size_t size{};
};

struct SendGather final
{
    std::vector<SendSlice> slices;
    std::size_t total_bytes{};

    bool Empty() const noexcept
    {
        return slices.empty();
    }
};

enum class SendConsumeResult
{
    Progress,
    ItemCompleted,
    Invalid,
};

/// @brief connection mutex 아래에서 사용하는 bounded FIFO send 상태다.
///
/// 이 class 자체는 thread-safe하지 않다. socket이나 operation을 모르고
/// buffer lifetime, atomic batch admission, queue 상한, partial send
/// offset만 관리한다.
class SendQueue final
{
public:
    SendQueue(std::size_t maximum_items, std::size_t maximum_bytes);

    /// @brief immutable buffer를 queue 뒤에 추가한다.
    ///
    /// empty buffer는 성공으로 무시한다. item 또는 byte 상한을 넘으면
    /// `false`를 반환하며 queue를 변경하지 않는다.
    bool TryPush(SharedSendBuffer buffer) noexcept;

    /// @brief 여러 immutable segment를 하나의 admission 단위로 추가한다.
    ///
    /// null segment가 있거나 item/byte 상한을 넘으면 batch 전체를 거부한다.
    /// empty segment는 batch에서 제외하며 allocation 실패도 queue를 변경하지
    /// 않는다.
    bool TryPushBatch(
        std::vector<SharedSendBuffer> buffers) noexcept;

    /// @brief 현재 front의 아직 보내지 않은 범위를 반환한다.
    SendSlice Front() const noexcept;

    /// @brief queue 앞에서 gather count/byte 상한만큼 slice를 만든다.
    ///
    /// 반환값이 가진 shared_ptr는 queue가 clear되어도 storage를 보존한다.
    SendGather Gather(
        std::size_t maximum_segments,
        std::size_t maximum_bytes) const;

    /// @brief front에서 completion byte만큼 소비한다.
    ///
    /// completion이 여러 segment를 넘어도 logical queue 순서대로 소비한다.
    SendConsumeResult Consume(std::size_t transferred_bytes) noexcept;

    void Clear() noexcept;

    bool Empty() const noexcept;
    std::size_t ItemCount() const noexcept;
    std::size_t BatchCount() const noexcept;
    std::size_t QueuedBytes() const noexcept;
    std::size_t MaximumItems() const noexcept;
    std::size_t MaximumBytes() const noexcept;

private:
    struct Segment final
    {
        SharedSendBuffer buffer;
        std::size_t offset{};
    };

    struct Batch final
    {
        std::vector<Segment> segments;
        std::size_t next_segment{};
    };

    const std::size_t maximum_items_;
    const std::size_t maximum_bytes_;
    std::deque<Batch> batches_;
    std::size_t item_count_{0};
    std::size_t queued_bytes_{0};
};

} // namespace iocp::transport
