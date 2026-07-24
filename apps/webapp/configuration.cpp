// 웹앱 typed config: TOML 파일과 CLI 인자를 typed WebAppOptions로 변환

#include "webapp/configuration.h"

#include <toml++/toml.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <charconv>
#include <climits>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace iocp::application
{

namespace
{

constexpr std::int64_t kSchemaVersion = 1;

std::uint64_t ParseUnsigned(
    const std::string_view name,
    const std::string_view value,
    const std::uint64_t maximum,
    const bool allow_zero)
{
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        (!allow_zero && parsed == 0) || parsed > maximum)
    {
        throw std::invalid_argument(
            std::string{name} + " is outside its valid range: " +
            std::string{value});
    }
    return parsed;
}

void ApplyOption(
    WebAppApplicationOptions& options,
    std::string_view name,
    const std::string_view value)
{
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
    {
        options.server.io_worker_count = static_cast<std::size_t>(
            ParseUnsigned(name, value,
                std::numeric_limits<std::size_t>::max(), false));
    }
    else if (name == "application-workers")
    {
        options.server.application_worker_count =
            static_cast<std::size_t>(
                ParseUnsigned(name, value,
                    std::numeric_limits<std::size_t>::max(), false));
    }
    else if (name == "application-queue")
    {
        options.server.maximum_application_tasks =
            static_cast<std::size_t>(
                ParseUnsigned(name, value,
                    std::numeric_limits<std::size_t>::max(), false));
    }
    else if (name == "connection-queue")
    {
        options.server.maximum_connection_tasks =
            static_cast<std::size_t>(
                ParseUnsigned(name, value,
                    std::numeric_limits<std::size_t>::max(), false));
    }
    else if (name == "home-dir")
    {
        if (value.empty())
            throw std::invalid_argument(
                "home-dir must not be empty");
        options.server.home_directory = value;
    }
    else if (name == "shutdown-timeout-ms")
    {
        using Rep = std::chrono::milliseconds::rep;
        const auto maximum = static_cast<std::uint64_t>(
            std::chrono::milliseconds::max().count());
        options.server.shutdown_timeout =
            std::chrono::milliseconds{static_cast<Rep>(
                ParseUnsigned(name, value, maximum, false))};
    }
    else
    {
        throw std::invalid_argument(
            "unknown webapp option: " + std::string{name});
    }
}

void ApplyToml(
    WebAppApplicationOptions& options,
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
            "invalid webapp TOML: " +
            std::string{error.description()});
    }

    if (const toml::node* node = root.get("schema_version"))
    {
        const auto version = node->value_exact<std::int64_t>();
        if (!version || *version != kSchemaVersion)
            throw std::invalid_argument(
                "unsupported webapp TOML schema_version");
    }

    const toml::table* server = root.get("server")->as_table();
    if (!server) return;

    // TOML key는 underscore, ApplyOption은 hyphen을 기대하므로 변환
    auto to_cli = [](const char* key) {
        std::string s(key);
        for (auto& c : s) if (c == '_') c = '-';
        return s;
    };
    auto apply_int = [&](const char* key) {
        const auto* node = server->get(key);
        if (!node) return;
        const auto val = node->value_exact<std::int64_t>();
        if (!val || *val < 0)
            throw std::invalid_argument(
                std::string{"server."} + key +
                " must be a non-negative integer");
        ApplyOption(options, to_cli(key), std::to_string(*val));
    };
    auto apply_str = [&](const char* key) {
        const auto* node = server->get(key);
        if (!node) return;
        const auto val = node->value_exact<std::string>();
        if (!val)
            throw std::invalid_argument(
                std::string{"server."} + key + " must be a string");
        ApplyOption(options, to_cli(key), *val);
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

    // home_dir가 상대 경로면 config file 기준으로 resolve
    const auto* td_node = server->get("home_dir");
    if (td_node)
    {
        const auto td = td_node->value_exact<std::string>();
        if (td)
        {
            std::filesystem::path home_path(*td);
            if (home_path.is_relative())
            {
                home_path = path.parent_path() / home_path;
            }
            options.server.home_directory =
                std::filesystem::absolute(home_path).string();
        }
    }
}

std::optional<std::filesystem::path> FindConfigFile(
    const std::vector<std::string_view>& arguments)
{
    // --config CLI가 명시됐으면 그대로 사용
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view arg = arguments[index];
        std::string_view value;
        if (arg == "--config")
        {
            if (++index >= arguments.size())
                throw std::invalid_argument("--config requires a path");
            value = arguments[index];
        }
        else if (arg.rfind("--config=", 0) == 0)
        {
            value = arg.substr(9);
        }
        else continue;

        if (value.empty())
            throw std::invalid_argument(
                "--config must specify one non-empty path");
        return std::filesystem::path{value};
    }

    // --config 없으면 exe 기준으로 config/webapp.toml 검색
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path)) == 0)
        return std::nullopt;

    std::filesystem::path exe_dir =
        std::filesystem::path(exe_path).parent_path();

    // bin/ 아래: ../../config/webapp.toml (build/windows-debug/bin → project root)
    for (int levels = 1; levels <= 3; ++levels)
    {
        auto parent = exe_dir;
        for (int i = 0; i < levels; ++i)
            parent = parent / "..";
        auto candidate = parent / "config" / "webapp.toml";
        if (std::filesystem::exists(candidate))
            return std::filesystem::canonical(candidate);
    }

    return std::nullopt;
}

void ApplyCommandLine(
    WebAppApplicationOptions& options,
    const std::vector<std::string_view>& arguments)
{
    bool positional_port_seen = false;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view arg = arguments[index];
        if (arg == "--help" || arg == "-h")
        {
            options.show_help = true;
            continue;
        }
        if (arg == "--config")
        {
            ++index;
            continue;
        }
        if (arg.rfind("--config=", 0) == 0) continue;

        if (arg.rfind("--", 0) != 0)
        {
            if (positional_port_seen)
                throw std::invalid_argument(
                    "only one positional port may be specified");
            ApplyOption(options, "port", arg);
            positional_port_seen = true;
            continue;
        }

        const std::string_view body = arg.substr(2);
        const std::size_t sep = body.find('=');
        std::string_view name = body;
        std::string_view value;
        if (sep != std::string_view::npos)
        {
            name = body.substr(0, sep);
            value = body.substr(sep + 1);
        }
        else
        {
            if (++index >= arguments.size())
                throw std::invalid_argument(
                    "--" + std::string{name} + " requires a value");
            value = arguments[index];
        }
        ApplyOption(options, name, value);
    }
}

void Validate(WebAppApplicationOptions& options)
{
    auto& s = options.server;
    if (s.io_worker_count == 0 ||
        s.application_worker_count == 0 ||
        s.maximum_application_tasks == 0 ||
        s.maximum_connection_tasks == 0 ||
        s.listener.backlog <= 0)
    {
        throw std::invalid_argument(
            "webapp worker, queue, and listener values must be positive");
    }
    if (s.home_directory.empty())
    {
        throw std::invalid_argument(
            "home_directory must not be empty");
    }
    // 상대 경로면 absolute로 resolve
    s.home_directory =
        std::filesystem::absolute(s.home_directory).string();
    if (!std::filesystem::exists(s.home_directory))
    {
        throw std::invalid_argument(
            "home directory not found: " + s.home_directory);
    }
    if (s.shutdown_timeout <= std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument(
            "webapp shutdown timeout must be positive");
    }
}

} // namespace

WebAppApplicationOptions::WebAppApplicationOptions()
{
    server.listener.port = 8080;
}

WebAppApplicationOptions LoadWebAppOptions(
    const std::vector<std::string_view>& arguments)
{
    WebAppApplicationOptions options;
    options.config_file = FindConfigFile(arguments);
    if (options.config_file)
    {
        ApplyToml(options, *options.config_file);
    }
    ApplyCommandLine(options, arguments);
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
        "  --home-dir PATH           webapp home directory (required)\n"
        "  --io-workers VALUE         IOCP worker count\n"
        "  --application-workers VALUE app thread pool size\n"
        "  --shutdown-timeout-ms VALUE\n"
        "\nExamples:\n"
        "  iocp_webapp_server --config config/webapp.toml\n"
        "  iocp_webapp_server --port 3000 --home-dir apps/webapp/templates\n";
}

} // namespace iocp::application
