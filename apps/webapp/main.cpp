// 웹앱 진입점: CLI→TOML→WebAppServer::Create
// 예: iocp_webapp_server --config config/webapp.toml
//      iocp_webapp_server --port 3000 --template-dir apps/webapp/templates

#include "webapp/configuration.h"

#include "core/logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <iostream>
#include <string_view>
#include <vector>

int main(const int argc, char* argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);

    try
    {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index)
            arguments.emplace_back(argv[index]);

        const auto options =
            iocp::application::LoadWebAppOptions(arguments);
        if (options.show_help)
        {
            std::cout << iocp::application::WebAppUsage();
            return 0;
        }

        const auto logger = iocp::core::MakeConsoleLogger();
        logger->Log(
            iocp::core::LogLevel::Info,
            "webapp.starting",
            "웹앱 서버를 시작합니다.",
            {{"port", std::to_string(
                          options.server.listener.port)},
             {"templates", options.server.home_directory}});

        auto server = iocp::server::WebAppServer::Create(
            logger, options.server);

        std::cout
            << "웹앱이 실행 중입니다. 종료하려면 Enter를 누르세요...\n"
            << "URL: http://"
            << options.server.listener.address << ':'
            << server->LocalPort() << "\n";
        std::cin.get();

        server->Stop();
        logger->Log(
            iocp::core::LogLevel::Info,
            "webapp.stopped",
            "웹앱 서버를 종료했습니다.");
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "웹앱 실행 실패: " << exception.what() << '\n';
        return 1;
    }
}
