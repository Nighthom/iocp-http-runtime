// SQLite3 database wrapper 구현
#include "webapp/db.h"

#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace iocp::server
{

Database::Database(const std::string_view path)
{
    if (sqlite3_open(path.data(), &db_) != SQLITE_OK)
    {
        const auto err = db_ ? sqlite3_errmsg(db_) : "unknown";
        throw std::runtime_error(
            std::string{"Failed to open database: "} + err);
    }

    sqlite3_exec(db_,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS posts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title TEXT NOT NULL,"
        "  author TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  token TEXT PRIMARY KEY,"
        "  username TEXT NOT NULL,"
        "  created_at TEXT NOT NULL"
        ");",
        nullptr, nullptr, nullptr);
}

Database::~Database()
{
    if (db_) sqlite3_close(db_);
}

std::uint64_t Database::InsertPost(
    const std::string_view title,
    const std::string_view author,
    const std::string_view content)
{
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &t);
    char date_buf[32];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", &tm);

    std::ostringstream sql;
    sql << "INSERT INTO posts(title,author,content,created_at) VALUES('"
        << title << "','" << author << "','" << content << "','"
        << date_buf << "')";
    Execute(sql.str());
    return static_cast<std::uint64_t>(
        sqlite3_last_insert_rowid(db_));
}

std::vector<Post> Database::GetPosts(const std::size_t limit)
{
    std::vector<Post> results;
    std::ostringstream sql;
    sql << "SELECT id,title,author,content,created_at FROM posts "
        << "ORDER BY id DESC LIMIT " << limit;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.str().c_str(),
            -1, &stmt, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            Post p;
            p.id = static_cast<std::uint64_t>(
                sqlite3_column_int64(stmt, 0));
            p.title = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 1));
            p.author = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 2));
            p.content = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 3));
            p.created_at = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 4));
            results.push_back(std::move(p));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

std::optional<Post> Database::GetPost(const std::uint64_t id)
{
    Post p;
    std::ostringstream sql;
    sql << "SELECT id,title,author,content,created_at FROM posts "
        << "WHERE id=" << id;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.str().c_str(),
            -1, &stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            p.id = static_cast<std::uint64_t>(
                sqlite3_column_int64(stmt, 0));
            p.title = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 1));
            p.author = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 2));
            p.content = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 3));
            p.created_at = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 4));
            sqlite3_finalize(stmt);
            return p;
        }
        sqlite3_finalize(stmt);
    }
    return std::nullopt;
}

void Database::SaveSession(
    const std::string_view token,
    const std::string_view username)
{
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &t);
    char date_buf[32];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", &tm);

    std::ostringstream sql;
    sql << "INSERT OR REPLACE INTO sessions(token,username,created_at) "
        << "VALUES('" << token << "','" << username << "','"
        << date_buf << "')";
    Execute(sql.str());
}

std::string Database::LoadSession(const std::string_view token)
{
    std::string username;
    std::ostringstream sql;
    sql << "SELECT username FROM sessions WHERE token='"
        << token << "'";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.str().c_str(),
            -1, &stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            username = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return username;
}

void Database::DeleteSession(const std::string_view token)
{
    std::ostringstream sql;
    sql << "DELETE FROM sessions WHERE token='" << token << "'";
    Execute(sql.str());
}

void Database::CleanExpiredSessions()
{
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(
        now - std::chrono::hours{1});
    std::tm tm;
    localtime_s(&tm, &t);
    char cutoff[32];
    std::strftime(cutoff, sizeof(cutoff), "%Y-%m-%d %H:%M:%S", &tm);

    std::ostringstream sql;
    sql << "DELETE FROM sessions WHERE created_at < '"
        << cutoff << "'";
    Execute(sql.str());
}

void Database::Execute(const std::string_view sql)
{
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.data(), nullptr,
            nullptr, &err) != SQLITE_OK)
    {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("DB error: " + msg);
    }
}

} // namespace iocp::server
