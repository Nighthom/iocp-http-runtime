// board_handlers 단위 테스트 — socket/server 없이 handler 함수 직접 검증

#include "webapp/board_handlers.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using namespace iocp::webapp;

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestSuccessfulLogin()
{
    const auto user = TryLogin("admin", "admin123");
    Check(user == "admin", "valid login should return username");

    const auto invalid = TryLogin("admin", "wrong");
    Check(invalid.empty(), "invalid password should return empty");
}

void TestNonexistentUser()
{
    const auto user = TryLogin("nobody", "pass");
    Check(user.empty(), "nonexistent user should return empty");
}

void TestCreatePost()
{
    const auto post = CreatePost("제목", "내용입니다", "admin");
    Check(post.id > 0, "post should have an id");
    Check(post.title.find('<') == std::string::npos,
        "post title should be HTML escaped");
    Check(post.author == "admin",
        "post author should be preserved");
    Check(post.content.find("내용") != std::string::npos,
        "post content should contain original text");
}

void TestCreateEmptyPost()
{
    bool threw = false;
    try
    {
        CreatePost("", "내용", "admin");
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "empty title should throw");

    threw = false;
    try
    {
        CreatePost("제목", "", "admin");
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "empty content should throw");
}

void TestHtmlEscape()
{
    Check(HtmlEscape("<script>") == "&lt;script&gt;",
        "< should become &lt;");
    Check(HtmlEscape("a & b") == "a &amp; b",
        "& should become &amp;");
    Check(HtmlEscape("\"quote\"") == "&quot;quote&quot;",
        "\" should become &quot;");
    Check(HtmlEscape("normal text") == "normal text",
        "normal text should be unchanged");
}

void TestLoginPageHandler()
{
    auto resp = HandleLoginPage();
    Check(resp.status_code == 200,
        "login page should return 200");
    Check(resp.headers.size() >= 1,
        "login page should have Content-Type header");
}

void TestLoginPageWithError()
{
    auto resp = HandleLoginPage("잘못된 비밀번호");
    Check(resp.status_code == 200,
        "login page with error should return 200");

    const auto body = iocp::protocol::http::StringFromBytes(resp.body);
    Check(body.find("잘못된 비밀번호") != std::string::npos,
        "error message should appear in page");
    Check(body.find("class=\"error\"") != std::string::npos,
        "error div should have error class");
}

void TestBoardPageHandler()
{
    std::vector<Post> posts;
    auto resp = HandleBoardPage("admin", posts);
    Check(resp.status_code == 200,
        "empty board page should return 200");

    const auto body = iocp::protocol::http::StringFromBytes(resp.body);
    Check(body.find("admin님") != std::string::npos,
        "username should appear in board page");
    Check(body.find("게시글이 없습니다") != std::string::npos,
        "empty board should show no-posts message");
}

void TestBoardPageWithPosts()
{
    std::vector<Post> posts;
    Post p;
    p.id = 1;
    p.title = "테스트 글";
    p.author = "user";
    p.content = "내용";
    p.created_at = std::chrono::system_clock::now();
    posts.push_back(p);

    auto resp = HandleBoardPage("admin", posts);
    Check(resp.status_code == 200, "board page should return 200");

    const auto body = iocp::protocol::http::StringFromBytes(resp.body);
    Check(body.find("테스트 글") != std::string::npos,
        "post title should appear in board");
    Check(body.find("user") != std::string::npos,
        "post author should appear in board");
    Check(body.find("/post?id=1") != std::string::npos,
        "post link should appear in board");
}

void TestPostDetailHandler()
{
    Post p;
    p.id = 1;
    p.title = "제목";
    p.author = "admin";
    p.content = "본문 내용";
    p.created_at = std::chrono::system_clock::now();

    auto resp = HandlePostDetail(p);
    Check(resp.status_code == 200,
        "post detail should return 200");

    const auto body = iocp::protocol::http::StringFromBytes(resp.body);
    Check(body.find("제목") != std::string::npos,
        "post title should appear in detail");
    Check(body.find("admin") != std::string::npos,
        "post author should appear in detail");
    Check(body.find("본문 내용") != std::string::npos,
        "post content should appear in detail");
    Check(body.find("/board") != std::string::npos,
        "back to board link should appear");
}

void TestWriteFormHandler()
{
    auto resp = HandleWriteForm();
    Check(resp.status_code == 200,
        "write form should return 200");

    const auto body = iocp::protocol::http::StringFromBytes(resp.body);
    Check(body.find("action=\"/write\"") != std::string::npos,
        "write form should POST to /write");
    Check(body.find("name=\"title\"") != std::string::npos,
        "form should have title field");
    Check(body.find("name=\"content\"") != std::string::npos,
        "form should have content field");
}

void TestStylesHandler()
{
    auto resp = HandleStyles();
    Check(resp.status_code == 200,
        "styles should return 200");
    // Check for key CSS properties
    const auto body = iocp::protocol::http::StringFromBytes(resp.body);
    Check(body.find("font-family") != std::string::npos,
        "styles should contain CSS rules");
}

void TestTemplateRendering()
{
    iocp::protocol::http::SimpleTemplate tmpl("<h1>{{title}}</h1><p>{{body}}</p>");
    std::unordered_map<std::string, std::string> values;
    values["title"] = "Hello";
    values["body"] = "World";
    const auto result = tmpl.Render(values);
    Check(result == "<h1>Hello</h1><p>World</p>",
        "simple template should substitute correctly");
}

void TestTemplateMissingKey()
{
    iocp::protocol::http::SimpleTemplate tmpl("<p>{{exists}} {{missing}}</p>");
    const auto result = tmpl.Render(
        {{"exists", "value"}});
    Check(result == "<p>value </p>",
        "missing key should be replaced with empty string");
}

template <typename Test>
bool RunTest(const char* name, Test test)
{
    try
    {
        test();
        std::cout << "[PASS] " << name << '\n';
        return true;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << name << ": "
                  << exception.what() << '\n';
        return false;
    }
}

} // namespace

int main()
{
    iocp::webapp::SetTemplateDirectory(
        std::string(IOCP_SOURCE_DIR) + "/apps/webapp/templates");

    int failures = 0;
    failures += !RunTest(
        "successful login",
        TestSuccessfulLogin);
    failures += !RunTest(
        "nonexistent user",
        TestNonexistentUser);
    failures += !RunTest(
        "create post",
        TestCreatePost);
    failures += !RunTest(
        "create empty post",
        TestCreateEmptyPost);
    failures += !RunTest(
        "HTML escape",
        TestHtmlEscape);
    failures += !RunTest(
        "login page handler",
        TestLoginPageHandler);
    failures += !RunTest(
        "login page with error",
        TestLoginPageWithError);
    failures += !RunTest(
        "board page handler (empty)",
        TestBoardPageHandler);
    failures += !RunTest(
        "board page with posts",
        TestBoardPageWithPosts);
    failures += !RunTest(
        "post detail handler",
        TestPostDetailHandler);
    failures += !RunTest(
        "write form handler",
        TestWriteFormHandler);
    failures += !RunTest(
        "styles handler",
        TestStylesHandler);
    failures += !RunTest(
        "template rendering",
        TestTemplateRendering);
    failures += !RunTest(
        "template missing key",
        TestTemplateMissingKey);
    return failures == 0 ? 0 : 1;
}
