/// @file config_utils.h
/// @brief 공통 TOML/CLI config 파싱 유틸리티 — 앱마다 복붙하던 패턴을 통합

#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <toml++/toml.hpp>

namespace iocp::core
{

/// @brief CLI 인자를 key-value map과 positional list로 파싱한다.
///
/// --key=value, --key value, positional arg를 모두 처리하며,
/// --help / -h는 show_help flag로 반환한다.
class CliParser final
{
public:
    explicit CliParser(const std::vector<std::string_view>& arguments);

    bool HasOption(std::string_view name) const;
    std::string_view Option(
        std::string_view name,
        std::string_view default_value = {}) const;
    const std::vector<std::string_view>& Positional() const noexcept;

    bool show_help{};
    std::optional<std::filesystem::path> config_file;

private:
    std::unordered_map<std::string, std::string_view> options_;
    std::vector<std::string_view> positional_;
};

/// @brief 부호 없는 정수 파싱 + 범위 검증
std::uint64_t ParseUnsigned(
    std::string_view name,
    std::string_view value,
    std::uint64_t maximum,
    bool allow_zero);

/// @brief TOML table에서 하위 table을 optional로 읽는다.
const toml::table* OptionalTable(
    const toml::table& parent,
    std::string_view key);

/// @brief TOML table에서 int64 읽기
std::optional<std::int64_t> ReadTomlInt(
    const toml::table& table,
    std::string_view key);

/// @brief TOML table에서 string 읽기
std::optional<std::string> ReadTomlStr(
    const toml::table& table,
    std::string_view key);

/// @brief 허용된 키 외의 키가 있는지 검사
void RejectUnknownKeys(
    const toml::table& table,
    std::initializer_list<std::string_view> allowed);

/// @brief auto-detect config file (exe 기준으로 검색, --config CLI 우선)
///
/// --config가 명시됐으면 그 경로를 반환. 없으면 exe 기준으로
/// ../../config/{config_name}, ../config/{config_name} 순서로 찾는다.
std::optional<std::filesystem::path> FindConfigFile(
    const CliParser& cli,
    std::string_view config_name);

/// @brief TOML key의 underscore를 hyphen으로 변환 (TOML→CLI key mapping)
std::string TomlKeyToCli(std::string_view toml_key);

} // namespace iocp::core
