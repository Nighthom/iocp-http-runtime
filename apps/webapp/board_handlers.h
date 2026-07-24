/// @file board_handlers.h
/// @brief 게시판 handler 함수 — socket/parser 독립, 단위 테스트 가능

#pragma once

#include "protocol/http/http_message.h"
#include "protocol/http/simple_template.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iocp::webapp
{

struct Post final
{
    std::uint64_t id{};
    std::string title;
    std::string author;
    std::string content;
    std::chrono::system_clock::time_point created_at;
};

/// @brief template 파일이 있는 directory를 설정한다.
void SetHomeDirectory(std::string path);
std::string GetHomeDirectory();

/// @brief login page 렌더링
protocol::http::HttpResponse HandleLoginPage(
    const std::string& error = {});

/// @brief login POST 처리. 성공 시 session token 반환, 실패 시 빈 문자열.
std::string TryLogin(
    const std::string& username,
    const std::string& password);

/// @brief 게시판 목록 페이지 렌더링
protocol::http::HttpResponse HandleBoardPage(
    const std::string& username,
    const std::vector<Post>& posts);

/// @brief 게시글 상세 페이지 렌더링
protocol::http::HttpResponse HandlePostDetail(const Post& post);

/// @brief 글쓰기 폼 렌더링
protocol::http::HttpResponse HandleWriteForm();

/// @brief 게시글 생성. 성공 시 post 반환.
Post CreatePost(
    const std::string& title,
    const std::string& content,
    const std::string& author);

/// @brief stylesheet 렌더링
protocol::http::HttpResponse HandleStyles();

/// @brief HTML escape
std::string HtmlEscape(const std::string& value);

} // namespace iocp::webapp
