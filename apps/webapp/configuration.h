/// @file configuration.h
/// @brief 웹앱 typed config + CLI/TOML merge

#pragma once

#include "webapp/webapp.h"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace iocp::application
{

struct WebAppApplicationOptions final
{
    WebAppApplicationOptions();

    server::WebAppOptions server;
    std::optional<std::filesystem::path> config_file;
    bool show_help{};
};

WebAppApplicationOptions LoadWebAppOptions(
    const std::vector<std::string_view>& arguments);

std::string_view WebAppUsage() noexcept;

} // namespace iocp::application
