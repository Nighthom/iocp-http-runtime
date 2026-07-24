#include "echo_server/configuration.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char* argv[])
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
            iocp::application::LoadEchoApplicationOptions(arguments);
        if (options.show_help)
        {
            std::cout << iocp::application::EchoApplicationUsage();
            return 0;
        }

        auto logger = iocp::application::BuildLogger(options.logging);
        iocp::application::LogEffectiveConfiguration(*logger, options);
        auto server =
            iocp::server::EchoServer::Create(logger, options.server);

        std::cout << "Echo server가 " << options.server.listener.address
                  << ':' << server->LocalPort()
                  << "에서 실행 중입니다.\n"
                  << "종료하려면 Enter를 누르세요.\n";
        std::cin.get();

        if (!server->Stop())
        {
            std::cerr << "정해진 시간 안에 server를 종료하지 못했습니다.\n";
            return 1;
        }
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Echo server 실행 실패: " << exception.what() << '\n';
        return 1;
    }
}
