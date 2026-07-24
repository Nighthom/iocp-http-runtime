#include "echo_server/configuration.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

using iocp::application::BuildLogger;
using iocp::application::EchoApplicationOptions;
using iocp::application::EchoClientOptions;
using iocp::application::LoadEchoApplicationOptions;
using iocp::application::LoadEchoClientOptions;
using iocp::application::LoggingOptions;
using iocp::core::LogLevel;

class TemporaryTomlFile final
{
public:
    explicit TemporaryTomlFile(const std::string_view contents)
    {
        static unsigned long counter = 0;
        path_ = std::filesystem::temp_directory_path() /
            ("iocp-configuration-test-" +
             std::to_string(::GetCurrentProcessId()) + "-" +
             std::to_string(++counter) + ".toml");

        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output)
        {
            throw std::runtime_error(
                "temporary TOML file을 쓰지 못했습니다");
        }
    }

    ~TemporaryTomlFile() noexcept
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryTomlFile(const TemporaryTomlFile&) = delete;
    TemporaryTomlFile& operator=(const TemporaryTomlFile&) = delete;

    const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

auto EmptyEnvironment()
{
    return [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };
}

template <typename Action>
void CheckInvalid(Action action, const char* message)
{
    try
    {
        action();
    }
    catch (const std::invalid_argument&)
    {
        return;
    }
    throw std::runtime_error(message);
}

void TestNamedDefaults()
{
    const EchoApplicationOptions options =
        LoadEchoApplicationOptions({}, EmptyEnvironment());

    Check(
        options.server.listener.address == "127.0.0.1",
        "default listener address가 다릅니다");
    Check(
        options.server.listener.port == 9000,
        "application default port가 다릅니다");
    Check(
        options.server.io_worker_count == 2,
        "default IOCP worker count가 다릅니다");
    Check(
        options.server.connection.receive_chunk_bytes == 4096,
        "default receive chunk가 다릅니다");
    Check(
        options.server.connection.maximum_send_queue_items == 64,
        "default send queue item limit이 다릅니다");
    Check(
        options.server.connection.maximum_send_queue_bytes == 1024 * 1024,
        "default send queue byte limit이 다릅니다");
    Check(
        options.server.connection.maximum_gather_segments_per_operation == 16 &&
            options.server.connection.maximum_gather_bytes_per_operation ==
                64 * 1024 &&
            options.server.connection.maximum_outbound_batch_segments == 16,
        "default send gather/batch limit이 다릅니다");
    Check(
        options.server.shutdown_timeout == std::chrono::seconds{10},
        "default shutdown timeout이 다릅니다");
    Check(
        options.logging.console_enabled &&
            options.logging.file_enabled &&
            options.logging.append,
        "default logging policy가 다릅니다");
    Check(
        options.logging.file_path ==
            std::filesystem::path{"logs"} / "echo_server.log",
        "default log path가 다릅니다");
    Check(
        !options.config_file,
        "config file을 지정하지 않았는데 경로가 남았습니다");
}

void TestTomlConfiguration()
{
    const TemporaryTomlFile config{
        "schema_version = 1\n"
        "\n"
        "[server]\n"
        "address = \"0.0.0.0\"\n"
        "port = 9100\n"
        "backlog = 128\n"
        "io_workers = 5\n"
        "shutdown_timeout_ms = 6500\n"
        "\n"
        "[server.connection]\n"
        "receive_chunk_bytes = 2048\n"
        "send_queue_items = 32\n"
        "send_queue_bytes = 32768\n"
        "send_gather_segments = 8\n"
        "send_gather_bytes = 8192\n"
        "outbound_batch_segments = 6\n"
        "\n"
        "[logging]\n"
        "console = false\n"
        "file = true\n"
        "path = \"runtime/toml-server.log\"\n"
        "append = false\n"
        "console_level = \"warning\"\n"
        "file_level = \"debug\"\n"};
    const std::string config_path = config.Path().string();
    const EchoApplicationOptions options = LoadEchoApplicationOptions(
        {"--config", config_path},
        EmptyEnvironment());

    Check(
        options.config_file == config.Path(),
        "적용한 TOML config 경로가 보존되지 않았습니다");
    Check(
        options.server.listener.address == "0.0.0.0" &&
            options.server.listener.port == 9100 &&
            options.server.listener.backlog == 128,
        "TOML listener 설정이 적용되지 않았습니다");
    Check(
        options.server.io_worker_count == 5 &&
            options.server.shutdown_timeout ==
                std::chrono::milliseconds{6500},
        "TOML runtime 설정이 적용되지 않았습니다");
    Check(
        options.server.connection.receive_chunk_bytes == 2048 &&
            options.server.connection.maximum_send_queue_items == 32 &&
            options.server.connection.maximum_send_queue_bytes == 32768 &&
            options.server.connection
                    .maximum_gather_segments_per_operation == 8 &&
            options.server.connection
                    .maximum_gather_bytes_per_operation == 8192 &&
            options.server.connection.maximum_outbound_batch_segments == 6,
        "TOML connection 설정이 적용되지 않았습니다");
    Check(
        !options.logging.console_enabled &&
            options.logging.file_enabled &&
            !options.logging.append,
        "TOML logging boolean 설정이 적용되지 않았습니다");
    Check(
        options.logging.file_path ==
                std::filesystem::path{"runtime"} / "toml-server.log" &&
            options.logging.console_minimum_level == LogLevel::Warning &&
            options.logging.file_minimum_level == LogLevel::Debug,
        "TOML logging path 또는 level이 적용되지 않았습니다");
}

void TestTomlEnvironmentAndCliPrecedence()
{
    const TemporaryTomlFile environment_config{
        "[server]\n"
        "address = \"environment-config\"\n"
        "port = 6000\n"};
    const TemporaryTomlFile command_line_config{
        "[server]\n"
        "address = \"command-line-config\"\n"
        "port = 7000\n"
        "io_workers = 7\n"
        "\n"
        "[server.connection]\n"
        "receive_chunk_bytes = 1024\n"};
    const std::string environment_config_path =
        environment_config.Path().string();
    const std::string command_line_config_path =
        command_line_config.Path().string();
    const std::string command_line_config_argument =
        "--config=" + command_line_config_path;
    const std::unordered_map<std::string, std::string> environment{
        {"IOCP_ECHO_CONFIG", environment_config_path},
        {"IOCP_ECHO_PORT", "7100"},
        {"IOCP_ECHO_SEND_QUEUE_ITEMS", "88"},
    };
    const auto lookup =
        [&environment](const std::string_view name)
            -> std::optional<std::string> {
        const auto found = environment.find(std::string{name});
        if (found == environment.end())
        {
            return std::nullopt;
        }
        return found->second;
    };

    const EchoApplicationOptions options = LoadEchoApplicationOptions(
        {
            command_line_config_argument,
            "--port",
            "7200",
            "--receive-chunk-bytes=2048",
        },
        lookup);

    Check(
        options.config_file == command_line_config.Path() &&
            options.server.listener.address == "command-line-config",
        "CLI config path가 environment config path를 덮어쓰지 못했습니다");
    Check(
        options.server.listener.port == 7200,
        "CLI 값이 environment와 TOML 값을 덮어쓰지 못했습니다");
    Check(
        options.server.io_worker_count == 7,
        "TOML 값이 named default를 덮어쓰지 못했습니다");
    Check(
        options.server.connection.maximum_send_queue_items == 88,
        "environment 값이 TOML 값을 덮어쓰지 못했습니다");
    Check(
        options.server.connection.receive_chunk_bytes == 2048,
        "CLI connection 값이 TOML 값을 덮어쓰지 못했습니다");
}

void TestEnvironmentSelectedToml()
{
    const TemporaryTomlFile config{
        "[server]\n"
        "port = 7350\n"};
    const std::unordered_map<std::string, std::string> environment{
        {"IOCP_ECHO_CONFIG", config.Path().string()},
    };
    const auto lookup =
        [&environment](const std::string_view name)
            -> std::optional<std::string> {
        const auto found = environment.find(std::string{name});
        if (found == environment.end())
        {
            return std::nullopt;
        }
        return found->second;
    };

    const EchoApplicationOptions options =
        LoadEchoApplicationOptions({}, lookup);
    Check(
        options.config_file == config.Path() &&
            options.server.listener.port == 7350,
        "IOCP_ECHO_CONFIG가 TOML file을 선택하지 못했습니다");
}

void TestEnvironmentAndCliPrecedence()
{
    const std::unordered_map<std::string, std::string> environment{
        {"IOCP_ECHO_ADDRESS", "0.0.0.0"},
        {"IOCP_ECHO_PORT", "8100"},
        {"IOCP_ECHO_BACKLOG", "128"},
        {"IOCP_ECHO_IO_WORKERS", "6"},
        {"IOCP_ECHO_RECEIVE_CHUNK_BYTES", "2048"},
        {"IOCP_ECHO_SEND_QUEUE_ITEMS", "40"},
        {"IOCP_ECHO_SEND_QUEUE_BYTES", "9000"},
        {"IOCP_ECHO_SEND_GATHER_SEGMENTS", "7"},
        {"IOCP_ECHO_SEND_GATHER_BYTES", "4096"},
        {"IOCP_ECHO_OUTBOUND_BATCH_SEGMENTS", "5"},
        {"IOCP_ECHO_SHUTDOWN_TIMEOUT_MS", "7000"},
        {"IOCP_ECHO_CONSOLE_LOG", "off"},
        {"IOCP_ECHO_FILE_LOG", "yes"},
        {"IOCP_ECHO_LOG_FILE", "runtime/server.log"},
        {"IOCP_ECHO_LOG_APPEND", "false"},
        {"IOCP_ECHO_CONSOLE_LOG_LEVEL", "warning"},
        {"IOCP_ECHO_FILE_LOG_LEVEL", "debug"},
    };
    const auto lookup =
        [&environment](const std::string_view name)
            -> std::optional<std::string> {
        const auto found = environment.find(std::string{name});
        if (found == environment.end())
        {
            return std::nullopt;
        }
        return found->second;
    };

    const std::vector<std::string_view> arguments{
        "--address=127.0.0.1",
        "--port",
        "8200",
        "--io-workers=3",
        "--receive-chunk-bytes",
        "512",
        "--console-log",
        "true",
        "--file-log-level=error",
    };
    const EchoApplicationOptions options =
        LoadEchoApplicationOptions(arguments, lookup);

    Check(
        options.server.listener.address == "127.0.0.1",
        "CLI address가 environment를 덮어쓰지 못했습니다");
    Check(
        options.server.listener.port == 8200,
        "CLI port가 environment를 덮어쓰지 못했습니다");
    Check(
        options.server.listener.backlog == 128,
        "environment backlog가 적용되지 않았습니다");
    Check(
        options.server.io_worker_count == 3,
        "CLI worker count가 environment를 덮어쓰지 못했습니다");
    Check(
        options.server.connection.receive_chunk_bytes == 512,
        "CLI receive chunk가 적용되지 않았습니다");
    Check(
        options.server.connection.maximum_send_queue_items == 40 &&
            options.server.connection.maximum_send_queue_bytes == 9000,
        "environment send queue limit이 적용되지 않았습니다");
    Check(
        options.server.connection.maximum_gather_segments_per_operation == 7 &&
            options.server.connection.maximum_gather_bytes_per_operation ==
                4096 &&
            options.server.connection.maximum_outbound_batch_segments == 5,
        "environment send gather/batch limit이 적용되지 않았습니다");
    Check(
        options.server.shutdown_timeout ==
            std::chrono::milliseconds{7000},
        "environment shutdown timeout이 적용되지 않았습니다");
    Check(
        options.logging.console_enabled &&
            options.logging.file_enabled &&
            !options.logging.append,
        "logging boolean 설정이 적용되지 않았습니다");
    Check(
        options.logging.file_path ==
            std::filesystem::path{"runtime"} / "server.log",
        "environment log path가 적용되지 않았습니다");
    Check(
        options.logging.console_minimum_level == LogLevel::Warning &&
            options.logging.file_minimum_level == LogLevel::Error,
        "logging level precedence가 다릅니다");
}

void TestPositionalPortAndHelp()
{
    const auto positional =
        LoadEchoApplicationOptions({"9300"}, EmptyEnvironment());
    Check(
        positional.server.listener.port == 9300,
        "기존 positional port가 적용되지 않았습니다");

    const auto help =
        LoadEchoApplicationOptions({"--help"}, EmptyEnvironment());
    Check(help.show_help, "help flag가 적용되지 않았습니다");
}

void TestInvalidInput()
{
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {"--unknown", "1"},
                EmptyEnvironment());
        },
        "unknown option을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {"--io-workers", "0"},
                EmptyEnvironment());
        },
        "0 worker를 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {"--receive-chunk-bytes", "4294967296"},
                EmptyEnvironment());
        },
        "ULONG_MAX보다 큰 receive chunk를 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {"--shutdown-timeout-ms", "0"},
                EmptyEnvironment());
        },
        "0 shutdown timeout을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {
                    "--send-queue-items",
                    "4",
                    "--send-gather-segments",
                    "5",
                },
                EmptyEnvironment());
        },
        "queue보다 큰 gather segment 상한을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {
                    "--send-queue-bytes",
                    "1024",
                    "--send-gather-bytes",
                    "2048",
                },
                EmptyEnvironment());
        },
        "queue보다 큰 gather byte 상한을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {
                    "--send-queue-items",
                    "4",
                    "--outbound-batch-segments",
                    "5",
                },
                EmptyEnvironment());
        },
        "queue보다 큰 outbound batch 상한을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {"--console-log", "sometimes"},
                EmptyEnvironment());
        },
        "잘못된 boolean을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {"--file-log-level", "verbose"},
                EmptyEnvironment());
        },
        "잘못된 log level을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {"--port"},
                EmptyEnvironment());
        },
        "값이 없는 option을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {"9000", "--port", "9001"},
                EmptyEnvironment());
        },
        "port 중복 지정을 허용했습니다");
}

void TestInvalidTomlInput()
{
    const TemporaryTomlFile unknown_key{
        "[server]\n"
        "porrt = 9000\n"};
    const TemporaryTomlFile wrong_type{
        "[server]\n"
        "port = 9000.0\n"};
    const TemporaryTomlFile unsupported_version{
        "schema_version = 2\n"};
    const TemporaryTomlFile malformed{
        "[server\n"
        "port = 9000\n"};

    const auto check_file =
        [](const std::filesystem::path& path) {
        const std::string path_text = path.string();
        LoadEchoApplicationOptions(
            {"--config", path_text},
            EmptyEnvironment());
    };
    CheckInvalid(
        [&] { check_file(unknown_key.Path()); },
        "알 수 없는 TOML key를 허용했습니다");
    CheckInvalid(
        [&] { check_file(wrong_type.Path()); },
        "잘못된 TOML value type을 허용했습니다");
    CheckInvalid(
        [&] { check_file(unsupported_version.Path()); },
        "지원하지 않는 TOML schema version을 허용했습니다");
    CheckInvalid(
        [&] { check_file(malformed.Path()); },
        "잘못된 TOML 문법을 허용했습니다");

    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() /
        "iocp-configuration-test-missing.toml";
    CheckInvalid(
        [&] { check_file(missing); },
        "존재하지 않는 TOML file을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoApplicationOptions(
                {"--config", "a.toml", "--config=b.toml"},
                EmptyEnvironment());
        },
        "중복 config path를 허용했습니다");
}

void TestLoggerWithoutSinks()
{
    LoggingOptions options;
    options.console_enabled = false;
    options.file_enabled = false;

    const auto logger = BuildLogger(options);
    Check(
        logger->SinkCount() == 0,
        "logging을 끈 logger에 sink가 등록됐습니다");
}

void TestClientDefaultsAndPositionalCompatibility()
{
    const EchoClientOptions defaults =
        LoadEchoClientOptions({}, EmptyEnvironment());
    Check(
        defaults.host == "127.0.0.1" && defaults.port == 9000,
        "client default endpoint가 다릅니다");
    Check(
        defaults.send_timeout == std::chrono::seconds{5} &&
            defaults.receive_timeout == std::chrono::seconds{5},
        "client default timeout이 다릅니다");
    Check(!defaults.one_shot, "default client가 one-shot입니다");

    const EchoClientOptions positional = LoadEchoClientOptions(
        {"localhost", "9100", "hello"},
        EmptyEnvironment());
    Check(
        positional.host == "localhost" &&
            positional.port == 9100 &&
            positional.one_shot &&
            positional.message == "hello",
        "client positional 호환성이 깨졌습니다");
}

void TestClientEnvironmentAndCliPrecedence()
{
    const std::unordered_map<std::string, std::string> environment{
        {"IOCP_ECHO_CLIENT_HOST", "server.internal"},
        {"IOCP_ECHO_CLIENT_PORT", "8100"},
        {"IOCP_ECHO_CLIENT_SEND_TIMEOUT_MS", "1200"},
        {"IOCP_ECHO_CLIENT_RECEIVE_TIMEOUT_MS", "3400"},
    };
    const auto lookup =
        [&environment](const std::string_view name)
            -> std::optional<std::string> {
        const auto found = environment.find(std::string{name});
        if (found == environment.end())
        {
            return std::nullopt;
        }
        return found->second;
    };

    const EchoClientOptions options = LoadEchoClientOptions(
        {
            "--host=127.0.0.1",
            "--port",
            "8200",
            "--send-timeout-ms=2200",
            "--message",
            "configured",
        },
        lookup);
    Check(
        options.host == "127.0.0.1" && options.port == 8200,
        "client CLI endpoint precedence가 다릅니다");
    Check(
        options.send_timeout == std::chrono::milliseconds{2200} &&
            options.receive_timeout == std::chrono::milliseconds{3400},
        "client timeout precedence가 다릅니다");
    Check(
        options.one_shot && options.message == "configured",
        "client message flag가 적용되지 않았습니다");
}

void TestInvalidClientInput()
{
    CheckInvalid(
        [] {
            LoadEchoClientOptions(
                {"--send-timeout-ms", "0"},
                EmptyEnvironment());
        },
        "0 client send timeout을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoClientOptions(
                {"--receive-timeout-ms", "4294967296"},
                EmptyEnvironment());
        },
        "DWORD_MAX보다 큰 client receive timeout을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoClientOptions(
                {"localhost", "--host", "127.0.0.1"},
                EmptyEnvironment());
        },
        "client host의 flag/positional 중복을 허용했습니다");
    CheckInvalid(
        [] {
            LoadEchoClientOptions(
                {"a", "9000", "message", "extra"},
                EmptyEnvironment());
        },
        "과도한 client positional argument를 허용했습니다");
}

template <typename Action>
void Run(const char* name, Action action)
{
    action();
    std::cout << "[PASS] " << name << '\n';
}

} // namespace

int main()
{
    ::SetConsoleOutputCP(CP_UTF8);

    try
    {
        Run("named defaults", TestNamedDefaults);
        Run("TOML configuration", TestTomlConfiguration);
        Run(
            "TOML, environment and CLI precedence",
            TestTomlEnvironmentAndCliPrecedence);
        Run(
            "environment selected TOML",
            TestEnvironmentSelectedToml);
        Run(
            "environment and CLI precedence",
            TestEnvironmentAndCliPrecedence);
        Run("positional port and help", TestPositionalPortAndHelp);
        Run("invalid input", TestInvalidInput);
        Run("invalid TOML input", TestInvalidTomlInput);
        Run("logger without sinks", TestLoggerWithoutSinks);
        Run(
            "client defaults and positional compatibility",
            TestClientDefaultsAndPositionalCompatibility);
        Run(
            "client environment and CLI precedence",
            TestClientEnvironmentAndCliPrecedence);
        Run("invalid client input", TestInvalidClientInput);
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
