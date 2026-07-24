// echo typed config: CLI, environment, TOML 우선순위로 typed 설정을 생성하고
// 검증한 뒤 logger를 조립한다.

#include "echo_server/configuration.h"
#include "echo_server/toml_config_loader.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace iocp::application
{

namespace
{

constexpr std::uint16_t kDefaultApplicationPort = 9000;

std::optional<std::string> ReadProcessEnvironment(
    const std::string_view name)
{
    const std::string owned_name{name};
    char* value = nullptr;
    std::size_t value_size = 0;
    const errno_t error =
        ::_dupenv_s(&value, &value_size, owned_name.c_str());
    if (error != 0)
    {
        throw std::system_error(
            static_cast<int>(error),
            std::generic_category(),
            "환경변수를 읽지 못했습니다: " + owned_name);
    }

    std::unique_ptr<char, decltype(&std::free)> storage(value, &std::free);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    return std::string{value};
}

std::string Lowercase(const std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value)
    {
        result.push_back(
            static_cast<char>(std::tolower(character)));
    }
    return result;
}

std::uint64_t ParseUnsigned(
    const std::string_view name,
    const std::string_view value,
    const std::uint64_t maximum,
    const bool allow_zero)
{
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed);
    if (value.empty() ||
        result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        (!allow_zero && parsed == 0) ||
        parsed > maximum)
    {
        throw std::invalid_argument(
            std::string{name} + " 값이 범위를 벗어났습니다: " +
            std::string{value});
    }
    return parsed;
}

bool ParseBoolean(
    const std::string_view name,
    const std::string_view value)
{
    const std::string normalized = Lowercase(value);
    if (normalized == "true" || normalized == "1" ||
        normalized == "on" || normalized == "yes")
    {
        return true;
    }
    if (normalized == "false" || normalized == "0" ||
        normalized == "off" || normalized == "no")
    {
        return false;
    }
    throw std::invalid_argument(
        std::string{name} + " 값은 true 또는 false여야 합니다: " +
        std::string{value});
}

core::LogLevel ParseLogLevel(
    const std::string_view name,
    const std::string_view value)
{
    const std::string normalized = Lowercase(value);
    if (normalized == "trace")
    {
        return core::LogLevel::Trace;
    }
    if (normalized == "debug")
    {
        return core::LogLevel::Debug;
    }
    if (normalized == "info")
    {
        return core::LogLevel::Info;
    }
    if (normalized == "warning" || normalized == "warn")
    {
        return core::LogLevel::Warning;
    }
    if (normalized == "error")
    {
        return core::LogLevel::Error;
    }
    if (normalized == "critical")
    {
        return core::LogLevel::Critical;
    }
    throw std::invalid_argument(
        std::string{name} + " log level이 유효하지 않습니다: " +
        std::string{value});
}

std::string_view LogLevelName(const core::LogLevel level) noexcept
{
    switch (level)
    {
    case core::LogLevel::Trace:
        return "trace";
    case core::LogLevel::Debug:
        return "debug";
    case core::LogLevel::Info:
        return "info";
    case core::LogLevel::Warning:
        return "warning";
    case core::LogLevel::Error:
        return "error";
    case core::LogLevel::Critical:
        return "critical";
    }
    return "unknown";
}

void ApplyOption(
    EchoApplicationOptions& options,
    const std::string_view name,
    const std::string_view value)
{
    if (name == "address")
    {
        if (value.empty())
        {
            throw std::invalid_argument("address는 비어 있을 수 없습니다");
        }
        options.server.listener.address = value;
    }
    else if (name == "port")
    {
        options.server.listener.port = static_cast<std::uint16_t>(
            ParseUnsigned(name, value, 65535, true));
    }
    else if (name == "backlog")
    {
        options.server.listener.backlog = static_cast<int>(
            ParseUnsigned(name, value, INT_MAX, false));
    }
    else if (name == "io-workers")
    {
        options.server.io_worker_count = static_cast<std::size_t>(
            ParseUnsigned(
                name,
                value,
                std::numeric_limits<std::size_t>::max(),
                false));
    }
    else if (name == "receive-chunk-bytes")
    {
        options.server.connection.receive_chunk_bytes =
            static_cast<std::size_t>(
                ParseUnsigned(name, value, ULONG_MAX, false));
    }
    else if (name == "send-queue-items")
    {
        options.server.connection.maximum_send_queue_items =
            static_cast<std::size_t>(
                ParseUnsigned(
                    name,
                    value,
                    std::numeric_limits<std::size_t>::max(),
                    false));
    }
    else if (name == "send-queue-bytes")
    {
        options.server.connection.maximum_send_queue_bytes =
            static_cast<std::size_t>(
                ParseUnsigned(
                    name,
                    value,
                    std::numeric_limits<std::size_t>::max(),
                    false));
    }
    else if (name == "send-gather-segments")
    {
        options.server.connection.maximum_gather_segments_per_operation =
            static_cast<std::size_t>(
                ParseUnsigned(
                    name,
                    value,
                    std::numeric_limits<DWORD>::max(),
                    false));
    }
    else if (name == "send-gather-bytes")
    {
        options.server.connection.maximum_gather_bytes_per_operation =
            static_cast<std::size_t>(
                ParseUnsigned(name, value, ULONG_MAX, false));
    }
    else if (name == "outbound-batch-segments")
    {
        options.server.connection.maximum_outbound_batch_segments =
            static_cast<std::size_t>(
                ParseUnsigned(
                    name,
                    value,
                    std::numeric_limits<std::size_t>::max(),
                    false));
    }
    else if (name == "shutdown-timeout-ms")
    {
        const auto maximum = static_cast<std::uint64_t>(
            std::chrono::milliseconds::max().count());
        using MillisecondsRep = std::chrono::milliseconds::rep;
        options.server.shutdown_timeout = std::chrono::milliseconds{
            static_cast<MillisecondsRep>(
                ParseUnsigned(name, value, maximum, false))};
    }
    else if (name == "console-log")
    {
        options.logging.console_enabled =
            ParseBoolean(name, value);
    }
    else if (name == "file-log")
    {
        options.logging.file_enabled =
            ParseBoolean(name, value);
    }
    else if (name == "log-file")
    {
        if (value.empty())
        {
            throw std::invalid_argument("log-file은 비어 있을 수 없습니다");
        }
        options.logging.file_path = value;
    }
    else if (name == "log-append")
    {
        options.logging.append = ParseBoolean(name, value);
    }
    else if (name == "console-log-level")
    {
        options.logging.console_minimum_level =
            ParseLogLevel(name, value);
    }
    else if (name == "file-log-level")
    {
        options.logging.file_minimum_level =
            ParseLogLevel(name, value);
    }
    else
    {
        throw std::invalid_argument(
            "알 수 없는 설정입니다: " + std::string{name});
    }
}

void ApplyEnvironment(
    EchoApplicationOptions& options,
    const EnvironmentLookup& lookup)
{
    constexpr std::pair<std::string_view, std::string_view> mappings[] = {
        {"IOCP_ECHO_ADDRESS", "address"},
        {"IOCP_ECHO_PORT", "port"},
        {"IOCP_ECHO_BACKLOG", "backlog"},
        {"IOCP_ECHO_IO_WORKERS", "io-workers"},
        {"IOCP_ECHO_RECEIVE_CHUNK_BYTES", "receive-chunk-bytes"},
        {"IOCP_ECHO_SEND_QUEUE_ITEMS", "send-queue-items"},
        {"IOCP_ECHO_SEND_QUEUE_BYTES", "send-queue-bytes"},
        {"IOCP_ECHO_SEND_GATHER_SEGMENTS", "send-gather-segments"},
        {"IOCP_ECHO_SEND_GATHER_BYTES", "send-gather-bytes"},
        {"IOCP_ECHO_OUTBOUND_BATCH_SEGMENTS", "outbound-batch-segments"},
        {"IOCP_ECHO_SHUTDOWN_TIMEOUT_MS", "shutdown-timeout-ms"},
        {"IOCP_ECHO_CONSOLE_LOG", "console-log"},
        {"IOCP_ECHO_FILE_LOG", "file-log"},
        {"IOCP_ECHO_LOG_FILE", "log-file"},
        {"IOCP_ECHO_LOG_APPEND", "log-append"},
        {"IOCP_ECHO_CONSOLE_LOG_LEVEL", "console-log-level"},
        {"IOCP_ECHO_FILE_LOG_LEVEL", "file-log-level"},
    };

    for (const auto& [environment_name, option_name] : mappings)
    {
        if (const auto value = lookup(environment_name))
        {
            ApplyOption(options, option_name, *value);
        }
    }
}

std::optional<std::filesystem::path> FindConfigurationFile(
    const std::vector<std::string_view>& arguments,
    const EnvironmentLookup& environment)
{
    std::optional<std::filesystem::path> selected;
    if (const auto value = environment("IOCP_ECHO_CONFIG"))
    {
        if (value->empty())
        {
            throw std::invalid_argument(
                "IOCP_ECHO_CONFIG는 비어 있을 수 없습니다");
        }
        selected = *value;
    }

    bool command_line_config_was_set = false;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument = arguments[index];
        std::string_view value;
        if (argument == "--config")
        {
            if (index + 1 >= arguments.size())
            {
                throw std::invalid_argument(
                    "설정 값이 없습니다: --config");
            }
            value = arguments[++index];
        }
        else if (argument.rfind("--config=", 0) == 0)
        {
            value = argument.substr(std::string_view{"--config="}.size());
        }
        else
        {
            continue;
        }

        if (command_line_config_was_set)
        {
            throw std::invalid_argument(
                "config는 한 번만 지정할 수 있습니다");
        }
        if (value.empty())
        {
            throw std::invalid_argument(
                "config path는 비어 있을 수 없습니다");
        }
        selected = value;
        command_line_config_was_set = true;
    }
    return selected;
}

void ApplyCommandLine(
    EchoApplicationOptions& options,
    const std::vector<std::string_view>& arguments)
{
    bool port_was_set = false;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument = arguments[index];
        if (argument == "--help" || argument == "-h")
        {
            options.show_help = true;
            continue;
        }

        if (argument.rfind("--", 0) != 0)
        {
            if (port_was_set)
            {
                throw std::invalid_argument(
                    "positional port는 한 번만 지정할 수 있습니다");
            }
            ApplyOption(options, "port", argument);
            port_was_set = true;
            continue;
        }

        const std::string_view body = argument.substr(2);
        const std::size_t separator = body.find('=');
        std::string_view name = body;
        std::string_view value;
        if (separator != std::string_view::npos)
        {
            name = body.substr(0, separator);
            value = body.substr(separator + 1);
        }
        else
        {
            if (index + 1 >= arguments.size())
            {
                throw std::invalid_argument(
                    "설정 값이 없습니다: --" + std::string{name});
            }
            value = arguments[++index];
        }

        if (name == "port")
        {
            if (port_was_set)
            {
                throw std::invalid_argument(
                    "port는 한 번만 지정할 수 있습니다");
            }
            port_was_set = true;
        }
        if (name == "config")
        {
            continue;
        }
        ApplyOption(options, name, value);
    }
}

void Validate(const EchoApplicationOptions& options)
{
    if (options.server.listener.address.empty())
    {
        throw std::invalid_argument("listener address는 비어 있을 수 없습니다");
    }
    if (options.server.io_worker_count == 0)
    {
        throw std::invalid_argument("IOCP worker count는 1 이상이어야 합니다");
    }
    if (options.server.connection.receive_chunk_bytes == 0 ||
        options.server.connection.receive_chunk_bytes > ULONG_MAX)
    {
        throw std::invalid_argument(
            "receive chunk bytes는 1..ULONG_MAX 범위여야 합니다");
    }
    if (options.server.connection.maximum_send_queue_items == 0 ||
        options.server.connection.maximum_send_queue_bytes == 0)
    {
        throw std::invalid_argument(
            "send queue limit은 1 이상이어야 합니다");
    }
    if (options.server.connection.maximum_gather_segments_per_operation == 0 ||
        options.server.connection.maximum_gather_bytes_per_operation == 0 ||
        options.server.connection.maximum_outbound_batch_segments == 0)
    {
        throw std::invalid_argument(
            "send gather/batch limit은 1 이상이어야 합니다");
    }
    if (options.server.connection.maximum_gather_segments_per_operation >
            options.server.connection.maximum_send_queue_items ||
        options.server.connection.maximum_outbound_batch_segments >
            options.server.connection.maximum_send_queue_items)
    {
        throw std::invalid_argument(
            "send gather/batch segment 상한은 queue item 상한 이하여야 합니다");
    }
    if (options.server.connection.maximum_gather_bytes_per_operation >
        options.server.connection.maximum_send_queue_bytes)
    {
        throw std::invalid_argument(
            "send gather byte 상한은 queue byte 상한 이하여야 합니다");
    }
    if (options.server.shutdown_timeout <=
        std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument(
            "shutdown timeout은 0보다 커야 합니다");
    }
    if (options.logging.file_enabled &&
        options.logging.file_path.empty())
    {
        throw std::invalid_argument(
            "file logging을 사용하면 log path가 필요합니다");
    }
}

void ApplyClientOption(
    EchoClientOptions& options,
    const std::string_view name,
    const std::string_view value)
{
    if (name == "host")
    {
        if (value.empty())
        {
            throw std::invalid_argument("client host는 비어 있을 수 없습니다");
        }
        options.host = value;
    }
    else if (name == "port")
    {
        options.port = static_cast<std::uint16_t>(
            ParseUnsigned(name, value, 65535, false));
    }
    else if (name == "send-timeout-ms")
    {
        options.send_timeout = std::chrono::milliseconds{
            static_cast<std::chrono::milliseconds::rep>(
                ParseUnsigned(
                    name,
                    value,
                    std::numeric_limits<std::uint32_t>::max(),
                    false))};
    }
    else if (name == "receive-timeout-ms")
    {
        options.receive_timeout = std::chrono::milliseconds{
            static_cast<std::chrono::milliseconds::rep>(
                ParseUnsigned(
                    name,
                    value,
                    std::numeric_limits<std::uint32_t>::max(),
                    false))};
    }
    else if (name == "message")
    {
        options.one_shot = true;
        options.message = value;
    }
    else
    {
        throw std::invalid_argument(
            "알 수 없는 client 설정입니다: " + std::string{name});
    }
}

void ApplyClientEnvironment(
    EchoClientOptions& options,
    const EnvironmentLookup& lookup)
{
    constexpr std::pair<std::string_view, std::string_view> mappings[] = {
        {"IOCP_ECHO_CLIENT_HOST", "host"},
        {"IOCP_ECHO_CLIENT_PORT", "port"},
        {"IOCP_ECHO_CLIENT_SEND_TIMEOUT_MS", "send-timeout-ms"},
        {"IOCP_ECHO_CLIENT_RECEIVE_TIMEOUT_MS", "receive-timeout-ms"},
    };

    for (const auto& [environment_name, option_name] : mappings)
    {
        if (const auto value = lookup(environment_name))
        {
            ApplyClientOption(options, option_name, *value);
        }
    }
}

void ApplyClientCommandLine(
    EchoClientOptions& options,
    const std::vector<std::string_view>& arguments)
{
    std::vector<std::string_view> positional;
    bool host_was_set = false;
    bool port_was_set = false;
    bool message_was_set = false;

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument = arguments[index];
        if (argument == "--help" || argument == "-h")
        {
            options.show_help = true;
            continue;
        }
        if (argument.rfind("--", 0) != 0)
        {
            positional.push_back(argument);
            continue;
        }

        const std::string_view body = argument.substr(2);
        const std::size_t separator = body.find('=');
        std::string_view name = body;
        std::string_view value;
        if (separator != std::string_view::npos)
        {
            name = body.substr(0, separator);
            value = body.substr(separator + 1);
        }
        else
        {
            if (index + 1 >= arguments.size())
            {
                throw std::invalid_argument(
                    "client 설정 값이 없습니다: --" +
                    std::string{name});
            }
            value = arguments[++index];
        }

        if (name == "host")
        {
            if (host_was_set)
            {
                throw std::invalid_argument(
                    "client host는 한 번만 지정할 수 있습니다");
            }
            host_was_set = true;
        }
        else if (name == "port")
        {
            if (port_was_set)
            {
                throw std::invalid_argument(
                    "client port는 한 번만 지정할 수 있습니다");
            }
            port_was_set = true;
        }
        else if (name == "message")
        {
            if (message_was_set)
            {
                throw std::invalid_argument(
                    "client message는 한 번만 지정할 수 있습니다");
            }
            message_was_set = true;
        }
        ApplyClientOption(options, name, value);
    }

    if (positional.size() > 3)
    {
        throw std::invalid_argument(
            "client positional argument는 host, port, message까지입니다");
    }
    if (!positional.empty())
    {
        if (host_was_set)
        {
            throw std::invalid_argument(
                "host의 flag와 positional 지정을 함께 사용할 수 없습니다");
        }
        ApplyClientOption(options, "host", positional[0]);
    }
    if (positional.size() >= 2)
    {
        if (port_was_set)
        {
            throw std::invalid_argument(
                "port의 flag와 positional 지정을 함께 사용할 수 없습니다");
        }
        ApplyClientOption(options, "port", positional[1]);
    }
    if (positional.size() >= 3)
    {
        if (message_was_set)
        {
            throw std::invalid_argument(
                "message의 flag와 positional 지정을 함께 사용할 수 없습니다");
        }
        ApplyClientOption(options, "message", positional[2]);
    }
}

} // namespace

EchoApplicationOptions::EchoApplicationOptions()
{
    server.listener.port = kDefaultApplicationPort;
}

EchoApplicationOptions LoadEchoApplicationOptions(
    const std::vector<std::string_view>& arguments,
    EnvironmentLookup environment)
{
    EchoApplicationOptions options;
    if (!environment)
    {
        environment = ReadProcessEnvironment;
    }

    if (const auto config_file =
            FindConfigurationFile(arguments, environment))
    {
        options.config_file = *config_file;
        for (const detail::ConfigurationOverride& entry :
             detail::LoadTomlConfiguration(*config_file))
        {
            ApplyOption(options, entry.option_name, entry.value);
        }
    }
    ApplyEnvironment(options, environment);
    ApplyCommandLine(options, arguments);
    Validate(options);
    return options;
}

EchoClientOptions LoadEchoClientOptions(
    const std::vector<std::string_view>& arguments,
    EnvironmentLookup environment)
{
    EchoClientOptions options;
    if (!environment)
    {
        environment = ReadProcessEnvironment;
    }

    ApplyClientEnvironment(options, environment);
    ApplyClientCommandLine(options, arguments);
    return options;
}

std::shared_ptr<core::Logger> BuildLogger(
    const LoggingOptions& options)
{
    auto logger = std::make_shared<core::Logger>();
    if (options.console_enabled)
    {
        if (options.console_minimum_level <= core::LogLevel::Info)
        {
            logger->AddSink(std::make_shared<core::StreamLogSink>(
                std::cout,
                options.console_minimum_level,
                core::LogLevel::Info));
        }

        const core::LogLevel stderr_minimum = std::max(
            options.console_minimum_level,
            core::LogLevel::Warning);
        logger->AddSink(std::make_shared<core::StreamLogSink>(
            std::cerr,
            stderr_minimum,
            core::LogLevel::Critical));
    }

    if (options.file_enabled)
    {
        const std::filesystem::path parent =
            options.file_path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }
        logger->AddSink(std::make_shared<core::FileLogSink>(
            options.file_path,
            options.append,
            options.file_minimum_level));
    }
    return logger;
}

void LogEffectiveConfiguration(
    const core::Logger& logger,
    const EchoApplicationOptions& options) noexcept
{
    try
    {
        const std::string port =
            std::to_string(options.server.listener.port);
        const std::string backlog =
            std::to_string(options.server.listener.backlog);
        const std::string workers =
            std::to_string(options.server.io_worker_count);
        const std::string receive_chunk = std::to_string(
            options.server.connection.receive_chunk_bytes);
        const std::string send_items = std::to_string(
            options.server.connection.maximum_send_queue_items);
        const std::string send_bytes = std::to_string(
            options.server.connection.maximum_send_queue_bytes);
        const std::string gather_segments = std::to_string(
            options.server.connection
                .maximum_gather_segments_per_operation);
        const std::string gather_bytes = std::to_string(
            options.server.connection
                .maximum_gather_bytes_per_operation);
        const std::string batch_segments = std::to_string(
            options.server.connection.maximum_outbound_batch_segments);
        const std::string shutdown_timeout =
            std::to_string(options.server.shutdown_timeout.count());
        const std::string console_enabled =
            options.logging.console_enabled ? "true" : "false";
        const std::string file_enabled =
            options.logging.file_enabled ? "true" : "false";
        const std::string file_path =
            options.logging.file_path.string();
        const std::string config_file = options.config_file
            ? options.config_file->string()
            : "<none>";

        logger.Log(
            core::LogLevel::Info,
            "application.effective_configuration",
            "검증된 echo server 설정을 적용합니다.",
            {
                {"address", options.server.listener.address},
                {"port", port},
                {"backlog", backlog},
                {"io_workers", workers},
                {"receive_chunk_bytes", receive_chunk},
                {"send_queue_items", send_items},
                {"send_queue_bytes", send_bytes},
                {"send_gather_segments", gather_segments},
                {"send_gather_bytes", gather_bytes},
                {"outbound_batch_segments", batch_segments},
                {"shutdown_timeout_ms", shutdown_timeout},
                {"config_file", config_file},
                {"console_log", console_enabled},
                {"console_log_level", LogLevelName(
                     options.logging.console_minimum_level)},
                {"file_log", file_enabled},
                {"file_path", file_path},
                {"file_log_level", LogLevelName(
                     options.logging.file_minimum_level)},
            });
    }
    catch (...)
    {
        logger.Log(
            core::LogLevel::Warning,
            "application.effective_configuration_failed",
            "적용 설정 로그를 생성하지 못했습니다.");
    }
}

std::string_view EchoApplicationUsage() noexcept
{
    return
        "사용법: iocp_echo_server [port] [options]\n"
        "  --config PATH\n"
        "  --address VALUE\n"
        "  --port VALUE\n"
        "  --backlog VALUE\n"
        "  --io-workers VALUE\n"
        "  --receive-chunk-bytes VALUE\n"
        "  --send-queue-items VALUE\n"
        "  --send-queue-bytes VALUE\n"
        "  --send-gather-segments VALUE\n"
        "  --send-gather-bytes VALUE\n"
        "  --outbound-batch-segments VALUE\n"
        "  --shutdown-timeout-ms VALUE\n"
        "  --console-log true|false\n"
        "  --file-log true|false\n"
        "  --log-file PATH\n"
        "  --log-append true|false\n"
        "  --console-log-level trace|debug|info|warning|error|critical\n"
        "  --file-log-level trace|debug|info|warning|error|critical\n"
        "  --help\n";
}

std::string_view EchoClientUsage() noexcept
{
    return
        "사용법: iocp_echo_client [host] [port] [message] [options]\n"
        "  --host VALUE\n"
        "  --port VALUE\n"
        "  --message VALUE\n"
        "  --send-timeout-ms VALUE\n"
        "  --receive-timeout-ms VALUE\n"
        "  --help\n";
}

} // namespace iocp::application
