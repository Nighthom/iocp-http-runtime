// {{key}} 치환 템플릿 엔진 구현
#include "protocol/http/simple_template.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace iocp::protocol::http
{

SimpleTemplate SimpleTemplate::Load(
    const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error(
            "template file을 열 수 없습니다: " + path.string());
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return SimpleTemplate(buffer.str());
}

SimpleTemplate::SimpleTemplate(std::string source)
    : source_(std::move(source))
{
}

std::string SimpleTemplate::Render(
    const std::unordered_map<std::string, std::string>& values) const
{
    std::string result;
    result.reserve(source_.size());
    std::size_t pos = 0;

    for (;;)
    {
        const auto open = source_.find("{{", pos);
        if (open == std::string::npos)
        {
            result.append(source_, pos, std::string::npos);
            break;
        }

        result.append(source_, pos, open - pos);
        const auto close = source_.find("}}", open + 2);
        if (close == std::string::npos)
        {
            // unmatched {{ -> 그대로 출력
            result.append(source_, open, std::string::npos);
            break;
        }

        const std::string key(
            source_.begin() + static_cast<std::ptrdiff_t>(open) + 2,
            source_.begin() + static_cast<std::ptrdiff_t>(close));

        const auto found = values.find(key);
        if (found != values.end())
        {
            result.append(found->second);
        }

        pos = close + 2;
    }

    return result;
}

SimpleTemplate& SimpleTemplate::Set(
    std::string key,
    std::string value)
{
    values_.emplace(std::move(key), std::move(value));
    return *this;
}

std::string SimpleTemplate::Render() const
{
    return Render(values_);
}

} // namespace iocp::protocol::http
