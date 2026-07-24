// 웹앱 typed config: TOML 파일과 CLI 인자를 typed WebAppOptions로 변환

#include "webapp/configuration.h"

#include "core/config_utils.h"

#include <toml++/toml.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <climits>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#ifndef IOCP_SOURCE_DIR
#define IOCP_SOURCE_DIR "."
#endif

namespace iocp::application
{

namespace
{

constexpr std::int64_t kSchemaVersion = 1;

void ApplyOption(
    WebAppApplicationOptions& options,
    const std::string_view name,
    const std::string_view value)
{
    using iocp::core::ParseUnsigned;

    if (name == "address")
    {
        if (value.empty())
            throw std::invalid_argument("address must not be empty");
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
        options.server.io_worker_count = static_cast<std::size_t>(ParseUnsigned(name, value, SIZE_MAX, false));
    else if (name == "application-workers")
        options.server.application_worker_count = static_cast<std::size_t>(ParseUnsigned(name, value, SIZE_MAX, false));
    else if (name == "application-queue")
        options.server.maximum_application_tasks = static_cast<std::size_t>(ParseUnsigned(name, value, SIZE_MAX, false));
    else if (name == "connection-queue")
        options.server.maximum_connection_tasks = static_cast<std::size_t>(ParseUnsigned(name, value, SIZE_MAX, false));
    else if (name == "home-dir")
    {
        if (value.empty())
            throw std::invalid_argument("home-dir must not be empty");
        options.server.home_directory = value;
    }
    else if (name == "shutdown-timeout-ms")
    {
        using Rep = std::chrono::milliseconds::rep;
        const auto maximum = static_cast<std::uint64_t>(std::chrono::milliseconds::max().count());
        options.server.shutdown_timeout = std::chrono::milliseconds{static_cast<Rep>(ParseUnsigned(name, value, maximum, false))};
    }
    else
        throw std::invalid_argument("unknown webapp option: " + std::string{name});
}

void ApplyToml(
    WebAppApplicationOptions& options,
    const std::filesystem::path& path)
{
    using iocp::core::OptionalTable;
    using iocp::core::ReadTomlInt;
    using iocp::core::ReadTomlStr;
    using iocp::core::RejectUnknownKeys;
    using iocp::core::TomlKeyToCli;

    toml::table root;
    try { root = toml::parse_file(path.u8string()); }
    catch (const toml::parse_error& error)
    {
        throw std::invalid_argument("invalid webapp TOML: " + std::string{error.description()});
    }

    if (const toml::node* node = root.get("schema_version"))
    {
        const auto version = node->value_exact<std::int64_t>();
        if (!version || *version != kSchemaVersion)
            throw std::invalid_argument("unsupported webapp TOML schema_version");
    }

    const toml::table* server = OptionalTable(root, "server");
    if (!server) return;

    RejectUnknownKeys(*server, {"address","port","backlog","io_workers","application_workers","application_queue","connection_queue","shutdown_timeout_ms","home_dir"});

    auto apply_int = [&](const char* key) {
        const auto val = ReadTomlInt(*server, key);
        if (val && *val >= 0)
            ApplyOption(options, TomlKeyToCli(key), std::to_string(*val));
    };
    auto apply_str = [&](const char* key) {
        const auto val = ReadTomlStr(*server, key);
        if (val)
            ApplyOption(options, TomlKeyToCli(key), *val);
    };

    apply_int("port");
    apply_int("backlog");
    apply_int("io_workers");
    apply_int("application_workers");
    apply_int("application_queue");
    apply_int("connection_queue");
    apply_int("shutdown_timeout_ms");
    apply_str("address");
    apply_str("home_dir");
}

void ApplyCli(
    WebAppApplicationOptions& options,
    const iocp::core::CliParser& cli)
{
    if (const auto val = cli.Option("port"); !val.empty()) ApplyOption(options, "port", val);
    if (const auto val = cli.Option("address"); !val.empty()) ApplyOption(options, "address", val);
    if (const auto val = cli.Option("backlog"); !val.empty()) ApplyOption(options, "backlog", val);
    if (const auto val = cli.Option("io-workers"); !val.empty()) ApplyOption(options, "io-workers", val);
    if (const auto val = cli.Option("application-workers"); !val.empty()) ApplyOption(options, "application-workers", val);
    if (const auto val = cli.Option("application-queue"); !val.empty()) ApplyOption(options, "application-queue", val);
    if (const auto val = cli.Option("connection-queue"); !val.empty()) ApplyOption(options, "connection-queue", val);
    if (const auto val = cli.Option("home-dir"); !val.empty()) ApplyOption(options, "home-dir", val);
    if (const auto val = cli.Option("shutdown-timeout-ms"); !val.empty()) ApplyOption(options, "shutdown-timeout-ms", val);

    if (!cli.Positional().empty())
        ApplyOption(options, "port", cli.Positional()[0]);
}

void Validate(WebAppApplicationOptions& options)
{
    auto& s = options.server;
    if (s.io_worker_count == 0 || s.application_worker_count == 0 ||
        s.maximum_application_tasks == 0 || s.maximum_connection_tasks == 0 ||
        s.listener.backlog <= 0)
        throw std::invalid_argument("webapp worker, queue, and listener values must be positive");

    if (s.home_directory.empty())
        throw std::invalid_argument("home_directory must not be empty");

    s.home_directory = std::filesystem::absolute(s.home_directory).string();

    // 개발 빌드(IOCP_SOURCE_DIR)나 설치 배포(exe 상대) 모두 지원
    if (std::filesystem::exists(s.home_directory))
        return;

    // IOCP_SOURCE_DIR가 없으면(설치 배포), exe 기준 ../share/webapp/templates 검색
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path)) > 0)
    {
        auto exe_dir = std::filesystem::path(exe_path).parent_path();
        for (int levels = 1; levels <= 3; ++levels)
        {
            auto parent = exe_dir;
            for (int i = 0; i < levels; ++i) parent = parent / "..";
            auto candidate = std::filesystem::absolute(parent / "share" / "webapp" / "templates");
            if (std::filesystem::exists(candidate))
            {
                s.home_directory = candidate.string();
                return;
            }
        }
    }

    throw std::invalid_argument("home directory not found: " + s.home_directory +
        "\n  Try --home-dir PATH or install with: cmake --install build/release --prefix dist");
}

} // namespace

WebAppApplicationOptions::WebAppApplicationOptions()
{
    server.listener.port = 8080;
    server.home_directory = (std::filesystem::path(IOCP_SOURCE_DIR) / "apps" / "webapp" / "templates").string();
}

WebAppApplicationOptions LoadWebAppOptions(
    const std::vector<std::string_view>& arguments)
{
    using iocp::core::CliParser;
    using iocp::core::FindConfigFile;

    const CliParser cli(arguments);
    WebAppApplicationOptions options;

    if (cli.show_help)
    {
        options.show_help = true;
        return options;
    }

    const auto config = FindConfigFile(cli, "webapp.toml");
    if (config) ApplyToml(options, *config);
    ApplyCli(options, cli);
    Validate(options);

    return options;
}

std::string_view WebAppUsage() noexcept
{
    return
        "Usage: iocp_webapp_server [port] [options]\n"
        "  --config PATH              TOML config file\n"
        "  --address VALUE            listen address (default 127.0.0.1)\n"
        "  --port VALUE               listen port (default 8080)\n"
        "  --home-dir PATH            webapp home directory\n"
        "  --io-workers VALUE         IOCP worker count\n"
        "  --application-workers VALUE app thread pool size\n"
        "  --shutdown-timeout-ms VALUE\n"
        "\nExamples:\n"
        "  iocp_webapp_server\n"
        "  iocp_webapp_server --config config/webapp.toml\n"
        "  iocp_webapp_server --port 3000 --home-dir apps/webapp/templates\n";
}

} // namespace iocp::application
