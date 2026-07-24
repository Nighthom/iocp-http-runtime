#include "http_server/configuration.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestDefaults()
{
    const auto options =
        iocp::application::LoadHttpApplicationOptions({});
    Check(
        options.server.listener.port == 8080 &&
            options.server.application_worker_count == 2 &&
            options.server.connection.maximum_send_queue_bytes ==
                2 * 1024 * 1024 &&
            options.server.session.parser.maximum_body_bytes ==
                1024 * 1024,
        "HTTP application defaults were incorrect");
}

void TestTomlAndCliPrecedence()
{
    const std::filesystem::path config =
        std::filesystem::path{IOCP_SOURCE_DIR} /
        "config" /
        "http_server.toml";
    const std::string config_argument =
        "--config=" + config.string();
    const std::vector<std::string_view> arguments{
        config_argument,
        "--port",
        "0",
        "--application-workers=3",
        "--http-max-requests",
        "7",
        "--http-server-name=test-http",
    };

    const auto options =
        iocp::application::LoadHttpApplicationOptions(arguments);
    Check(
        options.config_file == config &&
            options.server.listener.port == 0 &&
            options.server.listener.backlog == 128 &&
            options.server.application_worker_count == 3 &&
            options.server.session.maximum_requests_per_connection ==
                7 &&
            options.server.encoder.server_name == "test-http",
        "HTTP TOML and CLI precedence was incorrect");
}

void TestInvalidConfiguration()
{
    bool unknown_rejected = false;
    try
    {
        static_cast<void>(
            iocp::application::LoadHttpApplicationOptions(
                {"--unknown", "1"}));
    }
    catch (const std::invalid_argument&)
    {
        unknown_rejected = true;
    }
    Check(unknown_rejected, "unknown HTTP option was accepted");

    bool inconsistent_rejected = false;
    try
    {
        static_cast<void>(
            iocp::application::LoadHttpApplicationOptions(
                {"--send-queue-bytes", "1024"}));
    }
    catch (const std::invalid_argument&)
    {
        inconsistent_rejected = true;
    }
    Check(
        inconsistent_rejected,
        "inconsistent HTTP send limits were accepted");
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
    int failures = 0;
    failures += !RunTest(
        "HTTP configuration defaults",
        TestDefaults);
    failures += !RunTest(
        "HTTP TOML and CLI precedence",
        TestTomlAndCliPrecedence);
    failures += !RunTest(
        "HTTP invalid configuration",
        TestInvalidConfiguration);
    return failures == 0 ? 0 : 1;
}
