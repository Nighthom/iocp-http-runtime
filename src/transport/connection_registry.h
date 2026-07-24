#pragma once

/// @file connection_registry.h
/// @brief 프로세스 내 활성 TCP 연결의 shared ownership 레지스트리다.
///
/// TcpConnection이 파괴될 때 자신을 등록 해제하며, 레지스트리가 빌 때까지
/// 대기하는 `WaitEmpty`를 통해 정상적인 shutdown drain을 보장한다.
/// registry lock을 잡은 상태에서 connection method를 호출하지 않아
/// 교착을 방지한다.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace iocp::transport
{

class TcpConnection;

using ConnectionId = std::uint64_t;

struct RegistrySnapshot final
{
    std::size_t active_connections{};
    std::uint64_t total_added{};
    std::uint64_t total_removed{};
};

/// @brief active connection의 shared ownership과 조회를 담당한다.
///
/// registry lock을 잡은 상태에서 connection method를 호출하지 않는다.
class ConnectionRegistry final
{
public:
    /// @brief process 안에서 단조 증가하는 connection id를 발급한다.
    ConnectionId NextId() noexcept;

    /// @brief active connection을 registry에 추가한다.
    ///
    /// @throws std::invalid_argument null connection인 경우.
    /// @throws std::logic_error 같은 id가 이미 존재하는 경우.
    void Add(std::shared_ptr<TcpConnection> connection);

    /// @brief connection을 제거하고 마지막 대기자에게 empty 상태를 알린다.
    bool Remove(ConnectionId id) noexcept;

    /// @brief lock 밖에서 사용할 connection strong reference 목록을 반환한다.
    std::vector<std::shared_ptr<TcpConnection>> Connections() const;

    /// @brief registry가 빌 때까지 기다린다.
    bool WaitEmpty(std::chrono::milliseconds timeout);

    RegistrySnapshot Snapshot() const;

private:
    std::atomic<ConnectionId> next_id_{1};

    mutable std::mutex mutex_;
    std::condition_variable empty_condition_;
    std::unordered_map<ConnectionId, std::shared_ptr<TcpConnection>>
        connections_;
    std::uint64_t total_added_{0};
    std::uint64_t total_removed_{0};
};

} // namespace iocp::transport
