// 게시판 handler 구현 — template 기반, socket/parser 독립
#include "webapp/board_handlers.h"

#include "protocol/http/simple_template.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace iocp::webapp
{

namespace
{

using iocp::protocol::http::SimpleTemplate;

std::string g_template_dir = "apps/webapp/templates";

SimpleTemplate LoadTemplate(const std::string& name)
{
    return SimpleTemplate::Load(g_template_dir + "/" + name);
}

std::string FormatDate(
    const std::chrono::system_clock::time_point& time)
{
    const auto t = std::chrono::system_clock::to_time_t(time);
    std::tm tm;
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::atomic<std::uint64_t> g_next_post_id{1};

} // namespace

void SetTemplateDirectory(std::string path)
{
    g_template_dir = std::move(path);
}

std::string GetTemplateDirectory()
{
    return g_template_dir;
}

std::string HtmlEscape(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto c : value)
    {
        switch (c)
        {
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '&': result += "&amp;"; break;
        case '"': result += "&quot;"; break;
        default: result += c; break;
        }
    }
    return result;
}

protocol::http::HttpResponse HandleLoginPage(
    const std::string& error)
{
    auto tmpl = LoadTemplate("login.html");

    if (!error.empty())
    {
        tmpl.Set("error",
            "<p class=\"error\">" + error + "</p>");
    }

    return protocol::http::MakeTextResponse(
        200, tmpl.Render(),
        "text/html; charset=utf-8");
}

std::string TryLogin(
    const std::string& username,
    const std::string& password)
{
    struct UserEntry { std::string name; std::string pass; };
    static const UserEntry kUsers[] = {
        {"admin", "admin123"},
        {"user", "pass123"},
    };

    for (const auto& user : kUsers)
    {
        if (user.name == username && user.pass == password)
        {
            return username;
        }
    }
    return {};
}

protocol::http::HttpResponse HandleBoardPage(
    const std::string& username,
    const std::vector<Post>& posts)
{
    auto tmpl = LoadTemplate("board.html");
    tmpl.Set("username", HtmlEscape(username));

    if (posts.empty())
    {
        tmpl.Set("posts",
            "<div class=\"card\"><p>게시글이 없습니다. 첫 게시글을 작성해보세요!</p></div>");
    }
    else
    {
        auto row_tmpl = LoadTemplate("post_list.html");
        std::string rows;

        for (const auto& post : posts)
        {
            rows += "<tr>"
                    "<td>" + std::to_string(post.id) + "</td>"
                    "<td><a href=\"/post?id=" +
                    std::to_string(post.id) + "\">" +
                    HtmlEscape(post.title) + "</a></td>"
                    "<td>" + HtmlEscape(post.author) + "</td>"
                    "<td>" + FormatDate(post.created_at) + "</td>"
                    "</tr>";
        }

        row_tmpl.Set("rows", rows);
        tmpl.Set("posts", row_tmpl.Render());
    }

    return protocol::http::MakeTextResponse(
        200, tmpl.Render(),
        "text/html; charset=utf-8");
}

protocol::http::HttpResponse HandlePostDetail(const Post& post)
{
    auto tmpl = LoadTemplate("post_detail.html");
    tmpl.Set("title", HtmlEscape(post.title));
    tmpl.Set("author", HtmlEscape(post.author));
    tmpl.Set("date", FormatDate(post.created_at));
    tmpl.Set("content", HtmlEscape(post.content));
    return protocol::http::MakeTextResponse(
        200, tmpl.Render(),
        "text/html; charset=utf-8");
}

protocol::http::HttpResponse HandleWriteForm()
{
    auto tmpl = LoadTemplate("write_form.html");
    return protocol::http::MakeTextResponse(
        200, tmpl.Render(),
        "text/html; charset=utf-8");
}

Post CreatePost(
    const std::string& title,
    const std::string& content,
    const std::string& author)
{
    if (title.empty() || content.empty())
    {
        throw std::invalid_argument(
            "제목과 내용은 비워둘 수 없습니다");
    }

    Post post;
    post.id = g_next_post_id++;
    post.title = HtmlEscape(title);
    post.author = author;
    post.content = HtmlEscape(content);
    post.created_at = std::chrono::system_clock::now();
    return post;
}

protocol::http::HttpResponse HandleStyles()
{
    return protocol::http::MakeTextResponse(
        200,
        // CSS content
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:'Segoe UI',system-ui,-apple-system,sans-serif;"
        "background:#f0f2f5;color:#1a1a2e;line-height:1.6;min-height:100vh}"
        ".container{max-width:900px;margin:0 auto;padding:2rem 1rem}"
        "h1{font-size:1.8rem;color:#16213e;margin-bottom:1rem}"
        "h2{font-size:1.4rem;color:#0f3460;margin-bottom:1rem}"
        ".card{background:white;border-radius:12px;padding:2rem;"
        "box-shadow:0 2px 8px rgba(0,0,0,0.08);margin-bottom:1rem}"
        ".header{display:flex;justify-content:space-between;"
        "align-items:center;margin-bottom:1.5rem}"
        ".user-info{display:flex;align-items:center;gap:1rem;color:#555}"
        ".form-group{margin-bottom:1rem}"
        ".form-group label{display:block;margin-bottom:.25rem;"
        "font-weight:600;color:#333;font-size:.9rem}"
        ".form-group input,.form-group textarea{width:100%;"
        "padding:.6rem .8rem;border:1.5px solid #ddd;border-radius:8px;"
        "font-size:.95rem;transition:border-color .2s;font-family:inherit}"
        ".form-group input:focus,.form-group textarea:focus{"
        "outline:none;border-color:#0f3460}"
        ".btn{display:inline-block;padding:.6rem 1.4rem;background:#0f3460;"
        "color:white;border:none;border-radius:8px;cursor:pointer;"
        "font-size:.95rem;text-decoration:none;font-weight:500;"
        "transition:background .2s}"
        ".btn:hover{background:#1a4a8a}"
        ".btn-sm{padding:.3rem .8rem;background:#eee;color:#333;"
        "border-radius:6px;text-decoration:none;font-size:.85rem;"
        "transition:background .2s}"
        ".btn-sm:hover{background:#ddd}"
        ".btn-secondary{padding:.6rem 1.4rem;background:#e0e0e0;"
        "color:#333;border-radius:8px;text-decoration:none;"
        "font-size:.95rem;transition:background .2s}"
        ".btn-secondary:hover{background:#ccc}"
        ".form-actions{display:flex;gap:.5rem;justify-content:flex-end;"
        "margin-top:1rem}"
        ".toolbar{margin-bottom:1rem}"
        ".error{background:#fef2f2;color:#dc2626;padding:.8rem;"
        "border-radius:8px;margin-bottom:1rem}"
        ".hint{margin-top:1rem;font-size:.85rem;color:#888}"
        ".post-list{width:100%;border-collapse:collapse;background:white;"
        "border-radius:12px;overflow:hidden;box-shadow:0 2px 8px rgba(0,0,0,0.08)}"
        ".post-list th{background:#0f3460;color:white;padding:.8rem;"
        "text-align:left;font-weight:500}"
        ".post-list td{padding:.8rem;border-bottom:1px solid #eee}"
        ".post-list a{color:#0f3460;text-decoration:none;font-weight:500}"
        ".post-list a:hover{text-decoration:underline}"
        ".post-list tr:hover td{background:#f8f9fa}"
        ".post-meta{display:flex;gap:2rem;color:#888;font-size:.85rem;"
        "margin-bottom:1.5rem;padding-bottom:1rem;border-bottom:1px solid #eee}"
        ".post-content{white-space:pre-wrap;line-height:1.8;margin-bottom:1.5rem}"
        ".post-actions{text-align:right;padding-top:1rem;border-top:1px solid #eee}",
        "text/css; charset=utf-8");
}

} // namespace iocp::webapp
