#pragma once

#include "http_server/http_server.h"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace iocp::application
{

struct HttpApplicationOptions final
{
    HttpApplicationOptions();

    server::HttpServerOptions server;
    std::optional<std::filesystem::path> config_file;
    bool show_help{};
};

/// @brief default, 선택한 TOML, CLI override 순서로 HTTP 설정을 읽는다.
HttpApplicationOptions LoadHttpApplicationOptions(
    const std::vector<std::string_view>& arguments);

std::string_view HttpApplicationUsage() noexcept;

} // namespace iocp::application
