// send_queue.cpp
// Bounded FIFO send queue의 batch admission, gather, consume을 구현한다.
// admission 단계에서 item/byte 상한을 모두 검사해 batch 전체의 원자성을
// 보장한다. Consume은 논리 큐 순서대로 byte를 소비하며, segment가 완전히
// 전송되면 shared_ptr을 해제해 WSASend와 buffer 수명을 분리한다.
#include "transport/send_queue.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace iocp::transport
{

SendQueue::SendQueue(
    const std::size_t maximum_items,
    const std::size_t maximum_bytes)
    : maximum_items_(maximum_items),
      maximum_bytes_(maximum_bytes)
{
    if (maximum_items_ == 0 || maximum_bytes_ == 0)
    {
        throw std::invalid_argument(
            "send queue item/byte 상한은 1 이상이어야 합니다");
    }
}

bool SendQueue::TryPush(SharedSendBuffer buffer) noexcept
{
    try
    {
        std::vector<SharedSendBuffer> buffers;
        buffers.push_back(std::move(buffer));
        return TryPushBatch(std::move(buffers));
    }
    catch (...)
    {
        return false;
    }
}

bool SendQueue::TryPushBatch(
    std::vector<SharedSendBuffer> buffers) noexcept
{
    // admission 검사: item 상한과 byte 상한을 미리 계산한다.
    // batch 전체를 한 번에 검증하므로 중간에 실패해도 queue 상태가
    // 변경되지 않는다. null segment는 즉시 거부, empty segment는
    // batch에서 제외한다.
    std::size_t batch_items = 0;
    std::size_t batch_bytes = 0;
    const std::size_t available_items =
        maximum_items_ - item_count_;
    for (const SharedSendBuffer& buffer : buffers)
    {
        if (!buffer)
        {
            return false;
        }
        if (buffer->empty())
        {
            continue;
        }
        if (batch_items == available_items)
        {
            return false;
        }
        if (buffer->size() > maximum_bytes_ - queued_bytes_ - batch_bytes)
        {
            return false;
        }
        ++batch_items;
        batch_bytes += buffer->size();
    }

    if (batch_items == 0)
    {
        return true;
    }

    try
    {
        // admission 통과 후에만 queue를 변경한다. 각 segment는 offset 0으로
        // 시작하며, Consume이 진행되면서 offset이 전진한다.
        Batch batch;
        batch.segments.reserve(batch_items);
        for (SharedSendBuffer& buffer : buffers)
        {
            if (!buffer->empty())
            {
                batch.segments.push_back(
                    Segment{std::move(buffer), 0});
            }
        }

        batches_.push_back(std::move(batch));
        item_count_ += batch_items;
        queued_bytes_ += batch_bytes;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

SendSlice SendQueue::Front() const noexcept
{
    if (batches_.empty())
    {
        return {};
    }

    const Batch& batch = batches_.front();
    const Segment& item = batch.segments[batch.next_segment];
    return SendSlice{
        item.buffer,
        item.offset,
        item.buffer->size() - item.offset,
    };
}

SendGather SendQueue::Gather(
    const std::size_t maximum_segments,
    const std::size_t maximum_bytes) const
{
    // 모든 batch의 모든 미완료 segment를 next_segment부터 순회하며
    // configured segment/byte 상한까지 slice로 모은다. 각 slice는
    // 원본 shared_ptr을 참조하므로 gather가 살아있는 동안 buffer
    // 수명이 보장되고, WSASend completion 전에 queue가 clear되어도
    // 안전하다.
    SendGather gather;
    if (maximum_segments == 0 ||
        maximum_bytes == 0 ||
        batches_.empty())
    {
        return gather;
    }

    gather.slices.reserve(std::min(maximum_segments, item_count_));
    for (const Batch& batch : batches_)
    {
        for (std::size_t index = batch.next_segment;
             index < batch.segments.size() &&
             gather.slices.size() < maximum_segments &&
             gather.total_bytes < maximum_bytes;
             ++index)
        {
            const Segment& segment = batch.segments[index];
            const std::size_t remaining =
                segment.buffer->size() - segment.offset;
            const std::size_t bytes = std::min(
                remaining,
                maximum_bytes - gather.total_bytes);
            gather.slices.push_back(
                SendSlice{segment.buffer, segment.offset, bytes});
            gather.total_bytes += bytes;
        }

        if (gather.slices.size() == maximum_segments ||
            gather.total_bytes == maximum_bytes)
        {
            break;
        }
    }
    return gather;
}

SendConsumeResult SendQueue::Consume(
    const std::size_t transferred_bytes) noexcept
{
    // completion으로 보고된 byte만큼 논리 큐 순서대로 소비한다.
    // WSASend가 여러 segment를 gather해 보낸 경우에도 front부터
    // 순차적으로 offset을 전진시킨다.
    if (batches_.empty() ||
        transferred_bytes == 0 ||
        transferred_bytes > queued_bytes_)
    {
        return SendConsumeResult::Invalid;
    }

    std::size_t bytes_to_consume = transferred_bytes;
    bool completed_segment = false;
    while (bytes_to_consume != 0)
    {
        Batch& batch = batches_.front();
        Segment& segment = batch.segments[batch.next_segment];
        const std::size_t remaining =
            segment.buffer->size() - segment.offset;
        const std::size_t consumed =
            std::min(bytes_to_consume, remaining);

        segment.offset += consumed;
        queued_bytes_ -= consumed;
        bytes_to_consume -= consumed;
        if (segment.offset == segment.buffer->size())
        {
            // segment 완전 소비: next_segment를 전진시켜 shared_ptr
            // 참조를 해제할 수 있게 한다.
            ++batch.next_segment;
            --item_count_;
            completed_segment = true;
            if (batch.next_segment == batch.segments.size())
            {
                // batch 내 모든 segment가 소비되었으면 batch를 제거한다.
                batches_.pop_front();
            }
        }
    }

    // 새로 front가 된 segment의 offset이 0이면(새 buffer의 시작) ItemCompleted,
    // 아니면 Progress를 반환한다. 이 구분은 상위 레이어에서 buffer 소유권을
    // 언제 해제할지 판단하는 용도로 사용할 수 있다.
    return completed_segment &&
            (batches_.empty() ||
             batches_.front()
                     .segments[batches_.front().next_segment]
                     .offset == 0)
        ? SendConsumeResult::ItemCompleted
        : SendConsumeResult::Progress;
}

void SendQueue::Clear() noexcept
{
    batches_.clear();
    item_count_ = 0;
    queued_bytes_ = 0;
}

bool SendQueue::Empty() const noexcept
{
    return batches_.empty();
}

std::size_t SendQueue::ItemCount() const noexcept
{
    return item_count_;
}

std::size_t SendQueue::BatchCount() const noexcept
{
    return batches_.size();
}

std::size_t SendQueue::QueuedBytes() const noexcept
{
    return queued_bytes_;
}

std::size_t SendQueue::MaximumItems() const noexcept
{
    return maximum_items_;
}

std::size_t SendQueue::MaximumBytes() const noexcept
{
    return maximum_bytes_;
}

} // namespace iocp::transport
