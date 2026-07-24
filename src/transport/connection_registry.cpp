#include "transport/connection_registry.h"

#include "transport/tcp_connection.h"

#include <stdexcept>
#include <utility>

namespace iocp::transport
{

ConnectionId ConnectionRegistry::NextId() noexcept
{
    return next_id_.fetch_add(1, std::memory_order_relaxed);
}

void ConnectionRegistry::Add(std::shared_ptr<TcpConnection> connection)
{
    if (!connection)
    {
        throw std::invalid_argument("registry에 null connection을 추가할 수 없습니다");
    }

    std::lock_guard lock(mutex_);
    const auto [iterator, inserted] =
        connections_.emplace(connection->Id(), std::move(connection));
    if (!inserted)
    {
        throw std::logic_error("같은 connection id가 registry에 이미 존재합니다");
    }

    static_cast<void>(iterator);
    ++total_added_;
}

bool ConnectionRegistry::Remove(const ConnectionId id) noexcept
{
    bool became_empty = false;
    try
    {
        std::lock_guard lock(mutex_);
        const std::size_t removed = connections_.erase(id);
        if (removed == 0)
        {
            return false;
        }

        ++total_removed_;
        became_empty = connections_.empty();
    }
    catch (...)
    {
        return false;
    }

    if (became_empty)
    {
        empty_condition_.notify_all();
    }
    return true;
}

std::vector<std::shared_ptr<TcpConnection>>
ConnectionRegistry::Connections() const
{
    std::vector<std::shared_ptr<TcpConnection>> snapshot;
    std::lock_guard lock(mutex_);
    snapshot.reserve(connections_.size());
    for (const auto& [id, connection] : connections_)
    {
        static_cast<void>(id);
        snapshot.push_back(connection);
    }
    return snapshot;
}

bool ConnectionRegistry::WaitEmpty(const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return empty_condition_.wait_for(lock, timeout, [this] {
        return connections_.empty();
    });
}

RegistrySnapshot ConnectionRegistry::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return RegistrySnapshot{
        connections_.size(),
        total_added_,
        total_removed_,
    };
}

} // namespace iocp::transport
