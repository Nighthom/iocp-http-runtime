/// @file db.h
/// @brief 간단한 SQLite3 래퍼 — posts, sessions 영속화

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace iocp::server
{

struct Post final
{
    std::uint64_t id{};
    std::string title;
    std::string author;
    std::string content;
    std::string created_at;
};

class Database final
{
public:
    explicit Database(std::string_view path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // posts
    std::uint64_t InsertPost(
        std::string_view title,
        std::string_view author,
        std::string_view content);
    std::vector<Post> GetPosts(std::size_t limit = 50);
    std::optional<Post> GetPost(std::uint64_t id);

    // sessions
    void SaveSession(
        std::string_view token,
        std::string_view username);
    std::string LoadSession(std::string_view token);
    void DeleteSession(std::string_view token);
    void CleanExpiredSessions();

private:
    void Execute(std::string_view sql);

    sqlite3* db_{};
};

} // namespace iocp::server
