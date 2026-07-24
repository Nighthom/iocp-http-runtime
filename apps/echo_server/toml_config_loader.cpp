// strict TOML → key-value override: TOML 파일을 읽어 schema version, key,
// value type을 엄격하게 검증한 뒤 ConfigurationOverride 목록으로 변환한다.

#include "echo_server/toml_config_loader.h"

#include <toml++/toml.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace iocp::application::detail
{

namespace
{

constexpr std::int64_t kSupportedSchemaVersion = 1;

bool IsAllowedKey(
    const std::string_view key,
    const std::initializer_list<std::string_view> allowed)
{
    for (const std::string_view candidate : allowed)
    {
        if (key == candidate)
        {
            return true;
        }
    }
    return false;
}

void RejectUnknownKeys(
    const toml::table& table,
    const std::string_view section,
    const std::initializer_list<std::string_view> allowed)
{
    for (const auto& [key, value] : table)
    {
        static_cast<void>(value);
        const std::string_view key_name = key.str();
        if (!IsAllowedKey(key_name, allowed))
        {
            const std::string prefix =
                section.empty() ? std::string{} : std::string{section} + ".";
            throw std::invalid_argument(
                "알 수 없는 TOML 설정입니다: " + prefix +
                std::string{key_name});
        }
    }
}

const toml::table* OptionalTable(
    const toml::table& parent,
    const std::string_view key,
    const std::string_view qualified_name)
{
    const toml::node* node = parent.get(key);
    if (node == nullptr)
    {
        return nullptr;
    }

    const toml::table* table = node->as_table();
    if (table == nullptr)
    {
        throw std::invalid_argument(
            std::string{qualified_name} + "은 TOML table이어야 합니다");
    }
    return table;
}

void AddInteger(
    const toml::table& table,
    const std::string_view key,
    const std::string_view qualified_name,
    const std::string_view option_name,
    std::vector<ConfigurationOverride>& output)
{
    const toml::node* node = table.get(key);
    if (node == nullptr)
    {
        return;
    }

    const std::optional<std::int64_t> value =
        node->value_exact<std::int64_t>();
    if (!value)
    {
        throw std::invalid_argument(
            std::string{qualified_name} + "은 정수여야 합니다");
    }
    output.push_back(
        {std::string{option_name}, std::to_string(*value)});
}

void AddBoolean(
    const toml::table& table,
    const std::string_view key,
    const std::string_view qualified_name,
    const std::string_view option_name,
    std::vector<ConfigurationOverride>& output)
{
    const toml::node* node = table.get(key);
    if (node == nullptr)
    {
        return;
    }

    const std::optional<bool> value = node->value_exact<bool>();
    if (!value)
    {
        throw std::invalid_argument(
            std::string{qualified_name} + "은 boolean이어야 합니다");
    }
    output.push_back(
        {std::string{option_name}, *value ? "true" : "false"});
}

void AddString(
    const toml::table& table,
    const std::string_view key,
    const std::string_view qualified_name,
    const std::string_view option_name,
    std::vector<ConfigurationOverride>& output)
{
    const toml::node* node = table.get(key);
    if (node == nullptr)
    {
        return;
    }

    const std::optional<std::string> value =
        node->value_exact<std::string>();
    if (!value)
    {
        throw std::invalid_argument(
            std::string{qualified_name} + "은 문자열이어야 합니다");
    }
    output.push_back({std::string{option_name}, *value});
}

void ReadSchemaVersion(const toml::table& root)
{
    const toml::node* node = root.get("schema_version");
    if (node == nullptr)
    {
        return;
    }

    const std::optional<std::int64_t> version =
        node->value_exact<std::int64_t>();
    if (!version)
    {
        throw std::invalid_argument(
            "schema_version은 정수여야 합니다");
    }
    if (*version != kSupportedSchemaVersion)
    {
        throw std::invalid_argument(
            "지원하지 않는 TOML schema_version입니다: " +
            std::to_string(*version));
    }
}

void ReadConnection(
    const toml::table& server,
    std::vector<ConfigurationOverride>& output)
{
    const toml::table* connection =
        OptionalTable(server, "connection", "server.connection");
    if (connection == nullptr)
    {
        return;
    }

    RejectUnknownKeys(
        *connection,
        "server.connection",
        {
            "receive_chunk_bytes",
            "send_queue_items",
            "send_queue_bytes",
            "send_gather_segments",
            "send_gather_bytes",
            "outbound_batch_segments",
        });
    AddInteger(
        *connection,
        "receive_chunk_bytes",
        "server.connection.receive_chunk_bytes",
        "receive-chunk-bytes",
        output);
    AddInteger(
        *connection,
        "send_queue_items",
        "server.connection.send_queue_items",
        "send-queue-items",
        output);
    AddInteger(
        *connection,
        "send_queue_bytes",
        "server.connection.send_queue_bytes",
        "send-queue-bytes",
        output);
    AddInteger(
        *connection,
        "send_gather_segments",
        "server.connection.send_gather_segments",
        "send-gather-segments",
        output);
    AddInteger(
        *connection,
        "send_gather_bytes",
        "server.connection.send_gather_bytes",
        "send-gather-bytes",
        output);
    AddInteger(
        *connection,
        "outbound_batch_segments",
        "server.connection.outbound_batch_segments",
        "outbound-batch-segments",
        output);
}

void ReadServer(
    const toml::table& root,
    std::vector<ConfigurationOverride>& output)
{
    const toml::table* server =
        OptionalTable(root, "server", "server");
    if (server == nullptr)
    {
        return;
    }

    RejectUnknownKeys(
        *server,
        "server",
        {
            "address",
            "port",
            "backlog",
            "io_workers",
            "shutdown_timeout_ms",
            "connection",
        });
    AddString(*server, "address", "server.address", "address", output);
    AddInteger(*server, "port", "server.port", "port", output);
    AddInteger(
        *server,
        "backlog",
        "server.backlog",
        "backlog",
        output);
    AddInteger(
        *server,
        "io_workers",
        "server.io_workers",
        "io-workers",
        output);
    AddInteger(
        *server,
        "shutdown_timeout_ms",
        "server.shutdown_timeout_ms",
        "shutdown-timeout-ms",
        output);
    ReadConnection(*server, output);
}

void ReadLogging(
    const toml::table& root,
    std::vector<ConfigurationOverride>& output)
{
    const toml::table* logging =
        OptionalTable(root, "logging", "logging");
    if (logging == nullptr)
    {
        return;
    }

    RejectUnknownKeys(
        *logging,
        "logging",
        {
            "console",
            "file",
            "path",
            "append",
            "console_level",
            "file_level",
        });
    AddBoolean(
        *logging,
        "console",
        "logging.console",
        "console-log",
        output);
    AddBoolean(
        *logging,
        "file",
        "logging.file",
        "file-log",
        output);
    AddString(*logging, "path", "logging.path", "log-file", output);
    AddBoolean(
        *logging,
        "append",
        "logging.append",
        "log-append",
        output);
    AddString(
        *logging,
        "console_level",
        "logging.console_level",
        "console-log-level",
        output);
    AddString(
        *logging,
        "file_level",
        "logging.file_level",
        "file-log-level",
        output);
}

} // namespace

std::vector<ConfigurationOverride> LoadTomlConfiguration(
    const std::filesystem::path& path)
{
    std::error_code error;
    const bool is_regular_file =
        std::filesystem::is_regular_file(path, error);
    if (error)
    {
        throw std::invalid_argument(
            "TOML 설정 파일을 확인하지 못했습니다: " +
            path.string() + " (" + error.message() + ")");
    }
    if (!is_regular_file)
    {
        throw std::invalid_argument(
            "TOML 설정 파일을 찾을 수 없습니다: " + path.string());
    }

    toml::table root;
    try
    {
        root = toml::parse_file(path.u8string());
    }
    catch (const toml::parse_error& parse_error)
    {
        throw std::invalid_argument(
            "TOML 설정 파일 구문이 잘못되었습니다: " +
            std::string{parse_error.description()});
    }

    RejectUnknownKeys(root, "", {"schema_version", "server", "logging"});
    ReadSchemaVersion(root);

    std::vector<ConfigurationOverride> output;
    ReadServer(root, output);
    ReadLogging(root, output);
    return output;
}

} // namespace iocp::application::detail
