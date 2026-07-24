#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace iocp::application::detail
{

struct ConfigurationOverride final
{
    std::string option_name;
    std::string value;
};

/// @brief TOML 파일을 기존 echo application option 이름과 값으로 변환한다.
///
/// 파일 문법, schema version, key와 value type을 엄격하게 검증한다.
std::vector<ConfigurationOverride> LoadTomlConfiguration(
    const std::filesystem::path& path);

} // namespace iocp::application::detail
