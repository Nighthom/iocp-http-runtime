/// @file json_utils.h
/// @brief 최소 JSON parse/format — 외부 라이브러리 없이 key-value 수준만

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iocp::core
{

class JsonValue final
{
public:
    static JsonValue Parse(std::string_view json);
    std::string GetString(std::string_view key, std::string_view def = {}) const;
    double GetNumber(std::string_view key, double def = 0.0) const;
    bool GetBool(std::string_view key, bool def = false) const;

    static std::string Format(
        const std::unordered_map<std::string, std::string>& fields);

private:
    std::unordered_map<std::string, std::string> fields_;
    static std::string Unescape(std::string_view s);
    static std::string Escape(std::string_view s);
};

} // namespace iocp::core
