// 최소 JSON parser/formatter 구현
#include "core/json_utils.h"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <stdexcept>

namespace iocp::core
{

JsonValue JsonValue::Parse(const std::string_view json)
{
    JsonValue result;
    std::string json_str(json);
    auto start = json_str.find('{');
    if (start == std::string::npos) return result;
    ++start;
    while (start < json_str.size())
    {
        while (start < json_str.size() &&
               (json_str[start] == ' ' || json_str[start] == '\n' ||
                json_str[start] == '\r' || json_str[start] == '\t'))
            ++start;
        if (start >= json_str.size() || json_str[start] == '}') break;
        auto key_start = json_str.find('"', start);
        if (key_start == std::string::npos) break;
        ++key_start;
        auto key_end = json_str.find('"', key_start);
        if (key_end == std::string::npos) break;
        const std::string key = json_str.substr(key_start, key_end - key_start);
        auto colon = json_str.find(':', key_end);
        if (colon == std::string::npos) break;
        auto val_start = colon + 1;
        while (val_start < json_str.size() &&
               (json_str[val_start] == ' ' || json_str[val_start] == '\t'))
            ++val_start;
        if (val_start >= json_str.size()) break;
        std::string value;
        if (json_str[val_start] == '"')
        {
            ++val_start;
            auto val_end = json_str.find('"', val_start);
            if (val_end == std::string::npos) break;
            value = Unescape(json_str.substr(val_start, val_end - val_start));
            start = val_end + 1;
        }
        else if (json_str[val_start] == 't' && json_str.substr(val_start, 4) == "true")
        { value = "true"; start = val_start + 4; }
        else if (json_str[val_start] == 'f' && json_str.substr(val_start, 5) == "false")
        { value = "false"; start = val_start + 5; }
        else
        {
            auto end = val_start;
            while (end < json_str.size() &&
                   (std::isdigit(json_str[end]) != 0 || json_str[end] == '.' ||
                    json_str[end] == '-' || json_str[end] == 'e' || json_str[end] == 'E'))
                ++end;
            value = json_str.substr(val_start, end - val_start);
            start = end;
        }
        result.fields_[key] = value;
        while (start < json_str.size() &&
               (json_str[start] == ' ' || json_str[start] == '\t')) ++start;
        if (start < json_str.size() && json_str[start] == ',') ++start;
    }
    return result;
}

std::string JsonValue::GetString(std::string_view key, std::string_view def) const
{
    auto found = fields_.find(std::string(key));
    return found != fields_.end() ? found->second : std::string(def);
}

double JsonValue::GetNumber(std::string_view key, double def) const
{
    auto found = fields_.find(std::string(key));
    if (found == fields_.end()) return def;
    double r = def;
    std::from_chars(found->second.data(), found->second.data() + found->second.size(), r);
    return r;
}

bool JsonValue::GetBool(std::string_view key, bool def) const
{
    auto found = fields_.find(std::string(key));
    return found != fields_.end() ? found->second == "true" : def;
}

std::string JsonValue::Format(const std::unordered_map<std::string, std::string>& fields)
{
    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& [k, v] : fields)
    {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << Escape(k) << "\":\"" << Escape(v) << "\"";
    }
    ss << "}";
    return ss.str();
}

std::string JsonValue::Unescape(std::string_view s)
{
    std::string r; r.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            switch (s[++i])
            {
            case '"': r += '"'; break;
            case '\\': r += '\\'; break;
            case 'n': r += '\n'; break;
            case 'r': r += '\r'; break;
            case 't': r += '\t'; break;
            default: r += s[i]; break;
            }
        }
        else r += s[i];
    }
    return r;
}

std::string JsonValue::Escape(std::string_view s)
{
    std::string r; r.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default: r += c; break;
        }
    }
    return r;
}

} // namespace iocp::core
