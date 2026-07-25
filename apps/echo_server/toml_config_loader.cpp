// strict TOML → key-value override: TOML 파일을 읽어 schema version, key,
// value type을 엄격하게 검증한 뒤 ConfigurationOverride 목록으로 변환한다.

#include "echo_server/toml_config_loader.h"
#include "core/config_utils.h"

#include <toml++/toml.hpp>

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace iocp::application::detail
{

namespace
{

constexpr std::int64_t kSupportedSchemaVersion = 1;

void AddInteger(
    const toml::table& table,
    const std::string_view key,
    const std::string_view option_name,
    std::vector<ConfigurationOverride>& output)
{
    const auto value = core::ReadTomlInt(table, key);
    if (!value)
    {
        return;
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
    const std::string_view option_name,
    std::vector<ConfigurationOverride>& output)
{
    const auto value = core::ReadTomlStr(table, key);
    if (!value)
    {
        return;
    }
    output.push_back({std::string{option_name}, *value});
}

void ReadSchemaVersion(const toml::table& root)
{
    const auto version = core::ReadTomlInt(root, "schema_version");
    if (!version)
    {
        return;
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
    const auto* connection =
        core::OptionalTable(server, "connection");
    if (connection == nullptr)
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
    AddInteger(
        *connection,
        "receive_chunk_bytes",
        "receive-chunk-bytes",
        output);
    AddInteger(
        *connection,
        "send_queue_items",
        "send-queue-items",
        output);
    AddInteger(
        *connection,
        "send_queue_bytes",
        "send-queue-bytes",
        output);
    AddInteger(
        *connection,
        "send_gather_segments",
        "send-gather-segments",
        output);
    AddInteger(
        *connection,
        "send_gather_bytes",
        "send-gather-bytes",
        output);
    AddInteger(
        *connection,
        "outbound_batch_segments",
        "outbound-batch-segments",
        output);
}

void ReadServer(
    const toml::table& root,
    std::vector<ConfigurationOverride>& output)
{
    const auto* server =
        core::OptionalTable(root, "server");
    if (server == nullptr)
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
            "shutdown_timeout_ms",
            "connection",
        });
    AddString(*server, "address", "address", output);
    AddInteger(*server, "port", "port", output);
    AddInteger(*server, "backlog", "backlog", output);
    AddInteger(*server, "io_workers", "io-workers", output);
    AddInteger(
        *server,
        "shutdown_timeout_ms",
        "shutdown-timeout-ms",
        output);
    ReadConnection(*server, output);
}

void ReadLogging(
    const toml::table& root,
    std::vector<ConfigurationOverride>& output)
{
    const auto* logging =
        core::OptionalTable(root, "logging");
    if (logging == nullptr)
    {
        return;
    }

    core::RejectUnknownKeys(
        *logging,
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
    AddString(*logging, "path", "log-file", output);
    AddBoolean(
        *logging,
        "append",
        "logging.append",
        "log-append",
        output);
    AddString(
        *logging,
        "console_level",
        "console-log-level",
        output);
    AddString(
        *logging,
        "file_level",
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

    core::RejectUnknownKeys(root, {"schema_version", "server", "logging"});
    ReadSchemaVersion(root);

    std::vector<ConfigurationOverride> output;
    ReadServer(root, output);
    ReadLogging(root, output);
    return output;
}

} // namespace iocp::application::detail
