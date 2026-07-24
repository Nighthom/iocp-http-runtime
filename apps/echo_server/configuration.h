/// @file configuration.h
/// @brief echo typed config + CLI/environment/TOML merge

#pragma once

#include "core/logging.h"
#include "echo_server/echo_server.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iocp::application
{

/// @brief echo application의 log 출력 정책이다.
struct LoggingOptions final
{
    bool console_enabled{true};
    bool file_enabled{true};
    std::filesystem::path file_path{
        std::filesystem::path{"logs"} / "echo_server.log"};
    bool append{true};
    core::LogLevel console_minimum_level{core::LogLevel::Trace};
    core::LogLevel file_minimum_level{core::LogLevel::Trace};
};

/// @brief 외부 입력을 검증한 echo application 설정 snapshot이다.
struct EchoApplicationOptions final
{
    EchoApplicationOptions();

    server::EchoServerOptions server;
    LoggingOptions logging;
    std::optional<std::filesystem::path> config_file;
    bool show_help{false};
};

/// @brief echo test client의 endpoint와 blocking I/O timeout 설정이다.
struct EchoClientOptions final
{
    std::string host{"127.0.0.1"};
    std::uint16_t port{9000};
    std::chrono::milliseconds send_timeout{std::chrono::seconds{5}};
    std::chrono::milliseconds receive_timeout{std::chrono::seconds{5}};
    bool one_shot{false};
    std::string message;
    bool show_help{false};
};

using EnvironmentLookup =
    std::function<std::optional<std::string>(std::string_view name)>;

/// @brief environment를 적용한 뒤 CLI로 덮어써 typed 설정을 만든다.
///
/// arguments에는 executable 이름을 제외한 인자만 전달한다.
EchoApplicationOptions LoadEchoApplicationOptions(
    const std::vector<std::string_view>& arguments,
    EnvironmentLookup environment = {});

/// @brief echo client environment와 CLI를 typed 설정으로 만든다.
EchoClientOptions LoadEchoClientOptions(
    const std::vector<std::string_view>& arguments,
    EnvironmentLookup environment = {});

/// @brief 검증된 logging option에 맞는 sink를 조립한다.
std::shared_ptr<core::Logger> BuildLogger(const LoggingOptions& options);

/// @brief credential을 포함하지 않는 실제 적용 설정을 시작 로그에 남긴다.
void LogEffectiveConfiguration(
    const core::Logger& logger,
    const EchoApplicationOptions& options) noexcept;

std::string_view EchoApplicationUsage() noexcept;
std::string_view EchoClientUsage() noexcept;

} // namespace iocp::application
