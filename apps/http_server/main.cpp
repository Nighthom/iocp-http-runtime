// entry point: CLI→TOML→HttpServer::Create

#include "http_server/configuration.h"
#include "core/logging.h"
#include "http_server/http_server.h"

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
        {
            arguments.emplace_back(argv[index]);
        }

        const auto options =
            iocp::application::LoadHttpApplicationOptions(
                arguments);
        if (options.show_help)
        {
            std::cout << iocp::application::HttpApplicationUsage();
            return 0;
        }

        auto logger = iocp::core::MakeConsoleLogger();
        auto server =
            iocp::server::HttpServer::Create(
                logger,
                options.server);

        std::cout
            << "HTTP server가 http://"
            << options.server.listener.address << ':'
            << server->LocalPort() << "/ 에서 실행 중입니다.\n"
            << "종료하려면 Enter를 누르세요.\n";
        std::cin.get();
        return server->Stop() ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "HTTP server 실행 실패: "
                  << exception.what() << '\n';
        return 1;
    }
}
