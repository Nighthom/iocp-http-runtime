// 간단한 게시판 + 로그인 웹 애플리케이션 진입점
// 예: iocp_webapp --port 8080 --template-dir ../apps/webapp/templates

#include "webapp/webapp.h"

#include "core/logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <string>

static std::filesystem::path FindTemplateDir()
{
    // 빌드 디렉터리에서 실행 시: build/windows-debug → ../../apps/webapp/templates
    // 프로젝트 루트에서 실행 시: apps/webapp/templates

    char exe_path[MAX_PATH];
    const auto len = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    if (len == 0) return "apps/webapp/templates";

    std::filesystem::path exe_dir =
        std::filesystem::path(exe_path).parent_path();

    // 1. bin/ 아래에서 실행: ../../../apps/webapp/templates
    auto candidate = exe_dir / ".." / ".." / ".." / "apps" / "webapp" / "templates";
    if (std::filesystem::exists(candidate / "login.html"))
        return std::filesystem::canonical(candidate);

    // 2. exe 기준: ../../apps/webapp/templates (flat build dir)
    candidate = exe_dir / ".." / ".." / "apps" / "webapp" / "templates";
    if (std::filesystem::exists(candidate / "login.html"))
        return std::filesystem::canonical(candidate);

    // 3. exe 기준: ../apps/webapp/templates
    candidate = exe_dir / ".." / "apps" / "webapp" / "templates";
    if (std::filesystem::exists(candidate / "login.html"))
        return std::filesystem::canonical(candidate);

    // 4. cwd 기준
    candidate = "apps/webapp/templates";
    if (std::filesystem::exists(candidate / "login.html"))
        return std::filesystem::canonical(candidate);

    // 4. fallback
    return "apps/webapp/templates";
}

int main(const int argc, char* argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);

    std::uint16_t port = 8080;
    std::string template_dir;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
        else if (arg == "--template-dir" && i + 1 < argc)
            template_dir = argv[++i];
    }

    if (template_dir.empty())
        template_dir = FindTemplateDir().string();

    const auto logger = iocp::core::MakeConsoleLogger();
    logger->Log(
        iocp::core::LogLevel::Info,
        "webapp.starting",
        "웹앱 서버를 시작합니다.",
        {{"port", std::to_string(port)},
         {"templates", template_dir}});

    iocp::server::WebAppOptions options;
    options.listener.port = port;
    options.listener.address = "127.0.0.1";
    options.template_directory = template_dir;

    auto server = iocp::server::WebAppServer::Create(
        logger, std::move(options));

    logger->Log(
        iocp::core::LogLevel::Info,
        "webapp.listening",
        "웹앱이 요청을 기다리고 있습니다.",
        {{"port", std::to_string(server->LocalPort())},
         {"url", "http://127.0.0.1:" +
                     std::to_string(server->LocalPort())}});

    std::cout << "웹앱이 실행 중입니다. 종료하려면 Enter를 누르세요...\n";
    std::cout << "URL: http://127.0.0.1:"
              << server->LocalPort() << "\n";
    std::cin.get();

    server->Stop();
    logger->Log(
        iocp::core::LogLevel::Info,
        "webapp.stopped",
        "웹앱 서버를 종료했습니다.");
    return 0;
}
