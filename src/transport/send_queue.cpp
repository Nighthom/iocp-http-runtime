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
            ++batch.next_segment;
            --item_count_;
            completed_segment = true;
            if (batch.next_segment == batch.segments.size())
            {
                batches_.pop_front();
            }
        }
    }

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
