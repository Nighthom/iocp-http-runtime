// config_utils.cpp — 공통 TOML/CLI 파싱 구현
#include "core/config_utils.h"

#include <toml++/toml.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <charconv>
#include <stdexcept>

namespace iocp::core
{

// --- CliParser ---

CliParser::CliParser(
    const std::vector<std::string_view>& arguments)
{
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view arg = arguments[index];

        if (arg == "--help" || arg == "-h")
        {
            show_help = true;
            continue;
        }

        if (arg == "--config")
        {
            if (++index >= arguments.size())
                throw std::invalid_argument("--config requires a path");
            config_file = std::filesystem::path{arguments[index]};
            continue;
        }
        if (arg.rfind("--config=", 0) == 0)
        {
            config_file =
                std::filesystem::path{arg.substr(9)};
            continue;
        }

        if (arg.rfind("--", 0) != 0)
        {
            positional_.push_back(arg);
            continue;
        }

        const std::string_view body = arg.substr(2);
        const std::size_t sep = body.find('=');
        std::string name;
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
                    "--" + std::string{body} + " requires a value");
            name = body;
            value = arguments[index];
        }
        options_.emplace(std::move(name), value);
    }
}

bool CliParser::HasOption(const std::string_view name) const
{
    return options_.find(std::string{name}) != options_.end();
}

std::string_view CliParser::Option(
    const std::string_view name,
    const std::string_view default_value) const
{
    const auto found = options_.find(std::string{name});
    return found != options_.end() ? found->second : default_value;
}

const std::vector<std::string_view>& CliParser::Positional() const noexcept
{
    return positional_;
}

// --- ParseUnsigned ---

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

// --- TOML helpers ---

const toml::table* OptionalTable(
    const toml::table& parent,
    const std::string_view key)
{
    const toml::node* node = parent.get(key);
    if (node == nullptr) return nullptr;
    const toml::table* table = node->as_table();
    if (table == nullptr)
        throw std::invalid_argument(
            std::string{key} + " must be a TOML table");
    return table;
}

std::optional<std::int64_t> ReadTomlInt(
    const toml::table& table,
    const std::string_view key)
{
    const toml::node* node = table.get(key);
    if (!node) return std::nullopt;
    const auto value = node->value_exact<std::int64_t>();
    if (!value)
        throw std::invalid_argument(
            std::string{key} + " must be an integer");
    return *value;
}

std::optional<std::string> ReadTomlStr(
    const toml::table& table,
    const std::string_view key)
{
    const toml::node* node = table.get(key);
    if (!node) return std::nullopt;
    const auto value = node->value_exact<std::string>();
    if (!value)
        throw std::invalid_argument(
            std::string{key} + " must be a string");
    return *value;
}

void RejectUnknownKeys(
    const toml::table& table,
    const std::initializer_list<std::string_view> allowed)
{
    for (const auto& [key, value] : table)
    {
        static_cast<void>(value);
        bool found = false;
        for (const auto& candidate : allowed)
        {
            if (key.str() == candidate)
            {
                found = true;
                break;
            }
        }
        if (!found)
            throw std::invalid_argument(
                "unknown TOML key: " + std::string{key.str()});
    }
}

// --- FindConfigFile ---

std::optional<std::filesystem::path> FindConfigFile(
    const CliParser& cli,
    const std::string_view config_name)
{
    if (cli.config_file)
        return *cli.config_file;

    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path)) == 0)
        return std::nullopt;

    std::filesystem::path exe_dir =
        std::filesystem::path(exe_path).parent_path();

    for (int levels = 1; levels <= 3; ++levels)
    {
        auto parent = exe_dir;
        for (int i = 0; i < levels; ++i)
            parent = parent / "..";
        auto candidate = parent / "config" / config_name;
        if (std::filesystem::exists(candidate))
            return std::filesystem::canonical(candidate);
    }

    return std::nullopt;
}

// --- TomlKeyToCli ---

std::string TomlKeyToCli(const std::string_view toml_key)
{
    std::string result(toml_key);
    for (auto& c : result)
        if (c == '_') c = '-';
    return result;
}

} // namespace iocp::core
