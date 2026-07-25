// HTTP typed config: CLI 인자와 TOML 파일을 typed HttpServerOptions로 변환하고
// 설정 간 consistency를 검증한다.

#include "http_server/configuration.h"
#include "core/config_utils.h"

#include <toml++/toml.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>

#include <chrono>
#include <climits>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace iocp::application
{

namespace
{

constexpr std::int64_t kSchemaVersion = 1;

void ApplyOption(
    HttpApplicationOptions& options,
    const std::string_view name,
    const std::string_view value)
{
    const auto ToSize = [&](const bool allow_zero = false) {
        return static_cast<std::size_t>(core::ParseUnsigned(
            name,
            value,
            std::numeric_limits<std::size_t>::max(),
            allow_zero));
    };

    if (name == "address")
    {
        if (value.empty())
        {
            throw std::invalid_argument("address must not be empty");
        }
        options.server.listener.address = value;
    }
    else if (name == "port")
    {
        options.server.listener.port =
            static_cast<std::uint16_t>(
                core::ParseUnsigned(name, value, 65535, true));
    }
    else if (name == "backlog")
    {
        options.server.listener.backlog = static_cast<int>(
            core::ParseUnsigned(name, value, INT_MAX, false));
    }
    else if (name == "io-workers")
    {
        options.server.io_worker_count = ToSize();
    }
    else if (name == "application-workers")
    {
        options.server.application_worker_count = ToSize();
    }
    else if (name == "application-queue")
    {
        options.server.maximum_application_tasks = ToSize();
    }
    else if (name == "connection-queue")
    {
        options.server.maximum_connection_tasks = ToSize();
    }
    else if (name == "receive-chunk-bytes")
    {
        options.server.connection.receive_chunk_bytes =
            static_cast<std::size_t>(
                core::ParseUnsigned(name, value, ULONG_MAX, false));
    }
    else if (name == "send-queue-items")
    {
        options.server.connection.maximum_send_queue_items = ToSize();
    }
    else if (name == "send-queue-bytes")
    {
        options.server.connection.maximum_send_queue_bytes = ToSize();
    }
    else if (name == "send-gather-segments")
    {
        options.server.connection
            .maximum_gather_segments_per_operation =
            static_cast<std::size_t>(
                core::ParseUnsigned(
                    name,
                    value,
                    std::numeric_limits<DWORD>::max(),
                    false));
    }
    else if (name == "send-gather-bytes")
    {
        options.server.connection.maximum_gather_bytes_per_operation =
            static_cast<std::size_t>(
                core::ParseUnsigned(name, value, ULONG_MAX, false));
    }
    else if (name == "outbound-batch-segments")
    {
        options.server.connection.maximum_outbound_batch_segments =
            ToSize();
    }
    else if (name == "http-initial-buffer-bytes")
    {
        options.server.session.initial_buffer_bytes = ToSize();
    }
    else if (name == "http-maximum-buffer-bytes")
    {
        options.server.session.maximum_buffer_bytes = ToSize();
    }
    else if (name == "http-request-line-bytes")
    {
        options.server.session.parser.maximum_request_line_bytes =
            ToSize();
    }
    else if (name == "http-header-bytes")
    {
        options.server.session.parser.maximum_header_bytes = ToSize();
    }
    else if (name == "http-header-count")
    {
        options.server.session.parser.maximum_header_count = ToSize();
    }
    else if (name == "http-body-bytes")
    {
        options.server.session.parser.maximum_body_bytes =
            ToSize(true);
    }
    else if (name == "http-max-requests")
    {
        options.server.session.maximum_requests_per_connection =
            ToSize();
    }
    else if (name == "http-server-name")
    {
        if (value.empty())
        {
            throw std::invalid_argument(
                "HTTP server name must not be empty");
        }
        options.server.encoder.server_name = value;
    }
    else if (name == "http-response-header-bytes")
    {
        options.server.encoder.maximum_header_bytes = ToSize();
    }
    else if (name == "shutdown-timeout-ms")
    {
        using Rep = std::chrono::milliseconds::rep;
        const auto maximum = static_cast<std::uint64_t>(
            std::chrono::milliseconds::max().count());
        options.server.shutdown_timeout =
            std::chrono::milliseconds{static_cast<Rep>(
                core::ParseUnsigned(name, value, maximum, false))};
    }
    else
    {
        throw std::invalid_argument(
            "unknown HTTP server option: " + std::string{name});
    }
}

void ApplyInteger(
    HttpApplicationOptions& options,
    const toml::table& table,
    const std::string_view key,
    const std::string_view qualified_name,
    const std::string_view option_name)
{
    const auto value = core::ReadTomlInt(table, key);
    if (!value)
    {
        return;
    }
    if (*value < 0)
    {
        throw std::invalid_argument(
            std::string{qualified_name} +
            " must be a non-negative integer");
    }
    ApplyOption(options, option_name, std::to_string(*value));
}

void ApplyString(
    HttpApplicationOptions& options,
    const toml::table& table,
    const std::string_view key,
    const std::string_view /*qualified_name*/,
    const std::string_view option_name)
{
    const auto value = core::ReadTomlStr(table, key);
    if (!value)
    {
        return;
    }
    ApplyOption(options, option_name, *value);
}

void ApplyConnectionTable(
    HttpApplicationOptions& options,
    const toml::table& server)
{
    const auto* connection =
        core::OptionalTable(server, "connection");
    if (!connection)
    {
        return;
    }
    core::RejectUnknownKeys(
        *connection,
        {
            "receive_chunk_bytes",
            "send_queue_items",
            "send_queue_bytes",
            "send_gather_segments",
            "send_gather_bytes",
            "outbound_batch_segments",
        });
    ApplyInteger(
        options,
        *connection,
        "receive_chunk_bytes",
        "server.connection.receive_chunk_bytes",
        "receive-chunk-bytes");
    ApplyInteger(
        options,
        *connection,
        "send_queue_items",
        "server.connection.send_queue_items",
        "send-queue-items");
    ApplyInteger(
        options,
        *connection,
        "send_queue_bytes",
        "server.connection.send_queue_bytes",
        "send-queue-bytes");
    ApplyInteger(
        options,
        *connection,
        "send_gather_segments",
        "server.connection.send_gather_segments",
        "send-gather-segments");
    ApplyInteger(
        options,
        *connection,
        "send_gather_bytes",
        "server.connection.send_gather_bytes",
        "send-gather-bytes");
    ApplyInteger(
        options,
        *connection,
        "outbound_batch_segments",
        "server.connection.outbound_batch_segments",
        "outbound-batch-segments");
}

void ApplyServerTable(
    HttpApplicationOptions& options,
    const toml::table& root)
{
    const auto* server =
        core::OptionalTable(root, "server");
    if (!server)
    {
        return;
    }
    core::RejectUnknownKeys(
        *server,
        {
            "address",
            "port",
            "backlog",
            "io_workers",
            "application_workers",
            "application_queue",
            "connection_queue",
            "shutdown_timeout_ms",
            "connection",
        });
    ApplyString(
        options, *server, "address", "server.address", "address");
    ApplyInteger(
        options, *server, "port", "server.port", "port");
    ApplyInteger(
        options, *server, "backlog", "server.backlog", "backlog");
    ApplyInteger(
        options,
        *server,
        "io_workers",
        "server.io_workers",
        "io-workers");
    ApplyInteger(
        options,
        *server,
        "application_workers",
        "server.application_workers",
        "application-workers");
    ApplyInteger(
        options,
        *server,
        "application_queue",
        "server.application_queue",
        "application-queue");
    ApplyInteger(
        options,
        *server,
        "connection_queue",
        "server.connection_queue",
        "connection-queue");
    ApplyInteger(
        options,
        *server,
        "shutdown_timeout_ms",
        "server.shutdown_timeout_ms",
        "shutdown-timeout-ms");
    ApplyConnectionTable(options, *server);
}

void ApplyHttpTable(
    HttpApplicationOptions& options,
    const toml::table& root)
{
    const auto* http =
        core::OptionalTable(root, "http");
    if (!http)
    {
        return;
    }
    core::RejectUnknownKeys(
        *http,
        {
            "initial_buffer_bytes",
            "maximum_buffer_bytes",
            "maximum_request_line_bytes",
            "maximum_header_bytes",
            "maximum_header_count",
            "maximum_body_bytes",
            "maximum_requests_per_connection",
            "server_name",
            "maximum_response_header_bytes",
        });
    ApplyInteger(
        options,
        *http,
        "initial_buffer_bytes",
        "http.initial_buffer_bytes",
        "http-initial-buffer-bytes");
    ApplyInteger(
        options,
        *http,
        "maximum_buffer_bytes",
        "http.maximum_buffer_bytes",
        "http-maximum-buffer-bytes");
    ApplyInteger(
        options,
        *http,
        "maximum_request_line_bytes",
        "http.maximum_request_line_bytes",
        "http-request-line-bytes");
    ApplyInteger(
        options,
        *http,
        "maximum_header_bytes",
        "http.maximum_header_bytes",
        "http-header-bytes");
    ApplyInteger(
        options,
        *http,
        "maximum_header_count",
        "http.maximum_header_count",
        "http-header-count");
    ApplyInteger(
        options,
        *http,
        "maximum_body_bytes",
        "http.maximum_body_bytes",
        "http-body-bytes");
    ApplyInteger(
        options,
        *http,
        "maximum_requests_per_connection",
        "http.maximum_requests_per_connection",
        "http-max-requests");
    ApplyString(
        options,
        *http,
        "server_name",
        "http.server_name",
        "http-server-name");
    ApplyInteger(
        options,
        *http,
        "maximum_response_header_bytes",
        "http.maximum_response_header_bytes",
        "http-response-header-bytes");
}

void ApplyToml(
    HttpApplicationOptions& options,
    const std::filesystem::path& path)
{
    toml::table root;
    try
    {
        root = toml::parse_file(path.u8string());
    }
    catch (const toml::parse_error& error)
    {
        throw std::invalid_argument(
            "invalid HTTP server TOML: " +
            std::string{error.description()});
    }

    core::RejectUnknownKeys(
        root,
        {"schema_version", "server", "http"});
    if (const auto* node = root.get("schema_version"))
    {
        const auto version =
            node->value_exact<std::int64_t>();
        if (!version || *version != kSchemaVersion)
        {
            throw std::invalid_argument(
                "unsupported HTTP server TOML schema_version");
        }
    }
    ApplyServerTable(options, root);
    ApplyHttpTable(options, root);
}

void ApplyCommandLine(
    HttpApplicationOptions& options,
    const std::vector<std::string_view>& arguments)
{
    bool positional_port_seen = false;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument = arguments[index];
        if (argument == "--help" || argument == "-h")
        {
            options.show_help = true;
            continue;
        }
        if (argument == "--config")
        {
            ++index;
            continue;
        }
        if (argument.rfind("--config=", 0) == 0)
        {
            continue;
        }
        if (argument.rfind("--", 0) != 0)
        {
            if (positional_port_seen)
            {
                throw std::invalid_argument(
                    "only one positional port may be specified");
            }
            ApplyOption(options, "port", argument);
            positional_port_seen = true;
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
            if (++index >= arguments.size())
            {
                throw std::invalid_argument(
                    "--" + std::string{name} + " requires a value");
            }
            value = arguments[index];
        }
        ApplyOption(options, name, value);
    }
}

void Validate(const HttpApplicationOptions& options)
{
    const auto& server = options.server;
    const auto& connection = server.connection;
    const auto& session = server.session;
    const auto& parser = session.parser;
    const auto& encoder = server.encoder;

    if (server.listener.address.empty() ||
        server.listener.backlog <= 0 ||
        server.io_worker_count == 0 ||
        server.application_worker_count == 0 ||
        server.maximum_application_tasks == 0 ||
        server.maximum_connection_tasks == 0)
    {
        throw std::invalid_argument(
            "HTTP server worker, queue, and listener values must be positive");
    }
    if (connection.maximum_send_queue_items == 0 ||
        connection.maximum_send_queue_bytes == 0 ||
        connection.receive_chunk_bytes == 0 ||
        connection.maximum_gather_segments_per_operation == 0 ||
        connection.maximum_gather_bytes_per_operation == 0 ||
        connection.maximum_outbound_batch_segments == 0 ||
        connection.maximum_gather_segments_per_operation >
            connection.maximum_send_queue_items ||
        connection.maximum_outbound_batch_segments >
            connection.maximum_send_queue_items ||
        connection.maximum_gather_bytes_per_operation >
            connection.maximum_send_queue_bytes)
    {
        throw std::invalid_argument(
            "HTTP transport queue and gather limits are inconsistent");
    }
    if (session.initial_buffer_bytes == 0 ||
        session.initial_buffer_bytes > session.maximum_buffer_bytes ||
        session.maximum_requests_per_connection == 0 ||
        parser.maximum_request_line_bytes == 0 ||
        parser.maximum_header_bytes < 2 ||
        parser.maximum_header_count == 0)
    {
        throw std::invalid_argument(
            "HTTP parser and session limits are inconsistent");
    }
    const std::size_t fixed_request_bytes =
        parser.maximum_request_line_bytes +
        parser.maximum_header_bytes;
    if (fixed_request_bytes <
            parser.maximum_request_line_bytes ||
        parser.maximum_body_bytes >
            std::numeric_limits<std::size_t>::max() -
                fixed_request_bytes ||
        session.maximum_buffer_bytes <
            fixed_request_bytes + parser.maximum_body_bytes)
    {
        throw std::invalid_argument(
            "HTTP receive buffer cannot hold the configured request limits");
    }
    if (encoder.server_name.empty() ||
        encoder.maximum_header_bytes == 0 ||
        encoder.maximum_header_bytes >
            connection.maximum_send_queue_bytes ||
        parser.maximum_body_bytes >
            connection.maximum_send_queue_bytes -
                encoder.maximum_header_bytes)
    {
        throw std::invalid_argument(
            "HTTP response limits exceed the send queue capacity");
    }
    if (server.shutdown_timeout <=
        std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument(
            "HTTP shutdown timeout must be positive");
    }
}

} // namespace

HttpApplicationOptions::HttpApplicationOptions()
{
    server.listener.port = 8080;
}

HttpApplicationOptions LoadHttpApplicationOptions(
    const std::vector<std::string_view>& arguments)
{
    HttpApplicationOptions options;
    const core::CliParser cli(arguments);
    options.show_help = cli.show_help;
    options.config_file = core::FindConfigFile(cli, "http_server.toml");
    if (options.config_file)
    {
        ApplyToml(options, *options.config_file);
    }
    ApplyCommandLine(options, arguments);
    Validate(options);
    return options;
}

std::string_view HttpApplicationUsage() noexcept
{
    return
        "Usage: iocp_http_server [port] [options]\n"
        "  --config PATH\n"
        "  --address VALUE\n"
        "  --port VALUE\n"
        "  --io-workers VALUE\n"
        "  --application-workers VALUE\n"
        "  --application-queue VALUE\n"
        "  --connection-queue VALUE\n"
        "  --http-body-bytes VALUE\n"
        "  --http-max-requests VALUE\n"
        "  --http-server-name VALUE\n"
        "  --shutdown-timeout-ms VALUE\n"
        "All TOML fields also have matching long CLI options.\n";
}

} // namespace iocp::application
