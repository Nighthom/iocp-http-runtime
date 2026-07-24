/// @file simple_template.h
/// @brief {{key}} 패턴 치환만 지원하는 가벼운 템플릿 엔진

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iocp::protocol::http
{

/// @brief 파일에서 template을 읽고 {{variable}}을 값으로 치환한다.
///
/// nested template, loop, condition은 지원하지 않는다. application handler가
/// socket 없이 단위 테스트 가능하려면 `std::string`만 받는 `Render`로 충분하다.
class SimpleTemplate final
{
public:
    /// @brief template file을 읽어 컴파일한다.
    /// @throws std::runtime_error file을 읽을 수 없을 때.
    static SimpleTemplate Load(const std::filesystem::path& path);

    /// @brief raw template string으로 생성한다. (단위 테스트용)
    explicit SimpleTemplate(std::string source);

    /// @brief template에 {{key}}를 values로 치환한 결과를 반환한다.
    std::string Render(
        const std::unordered_map<std::string, std::string>& values) const;

    /// @brief 단일 치환 편의 함수
    SimpleTemplate& Set(
        std::string key,
        std::string value);
    std::string Render() const;

private:
    std::string source_;
    mutable std::unordered_map<std::string, std::string> values_;
};

} // namespace iocp::protocol::http
