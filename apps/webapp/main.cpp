// 간단한 게시판 + 로그인 웹 애플리케이션 진입점
// 예: iocp_webapp --port 8080

#include "webapp/webapp.h"

#include "core/logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <iostream>
#include <string>

int main(const int argc, char* argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);

    std::uint16_t port = 8080;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--port" && i + 1 < argc)
        {
            port = static_cast<std::uint16_t>(
                std::stoul(argv[++i]));
        }
    }

    const auto logger = iocp::core::MakeConsoleLogger();
    logger->Log(
        iocp::core::LogLevel::Info,
        "webapp.starting",
        "웹앱 서버를 시작합니다.",
        {{"port", std::to_string(port)}});

    iocp::server::WebAppOptions options;
    options.listener.port = port;
    options.listener.address = "127.0.0.1";

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
