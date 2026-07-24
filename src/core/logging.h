#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace iocp::core
{

/// @brief 로그의 심각도를 나타낸다.
enum class LogLevel : std::uint8_t
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

/// @brief allocation 없이 전달하는 하나의 구조화 로그 field다.
struct LogField final
{
    std::string_view key;
    std::string_view value;
};

/// @brief UTC 기준 로그 발생 시각이다.
struct LogTimestamp final
{
    std::uint16_t year{};
    std::uint16_t month{};
    std::uint16_t day{};
    std::uint16_t hour{};
    std::uint16_t minute{};
    std::uint16_t second{};
    std::uint16_t milliseconds{};
};

/// @brief sink에 동기적으로 전달되는 읽기 전용 로그 record다.
///
/// 문자열과 field는 `ILogSink::Write`가 반환될 때까지만 유효하다. M2 logger는
/// synchronous sink만 지원한다.
struct LogRecord final
{
    LogTimestamp timestamp;
    LogLevel level{LogLevel::Info};
    std::uint64_t thread_id{};
    std::string_view event;
    std::string_view message;
    const LogField* fields{};
    std::size_t field_count{};
};

/// @brief 하나의 출력 대상으로 로그 record를 전달한다.
class ILogSink
{
public:
    virtual ~ILogSink() = default;

    /// @brief record를 출력한다.
    ///
    /// sink failure는 application 흐름을 깨뜨리지 않으며 호출자에게 예외를
    /// 전파하지 않는다.
    virtual void Write(const LogRecord& record) noexcept = 0;
};

/// @brief `stdout`, `stderr` 같은 기존 stream에 로그를 출력한다.
///
/// 전달한 stream은 이 sink보다 오래 살아야 한다. 여러 thread의 record가
/// 한 줄 안에서 섞이지 않도록 sink 내부에서 직렬화한다.
class StreamLogSink final : public ILogSink
{
public:
    StreamLogSink(
        std::ostream& stream,
        LogLevel minimum_level = LogLevel::Trace,
        LogLevel maximum_level = LogLevel::Critical) noexcept;

    void Write(const LogRecord& record) noexcept override;

private:
    std::ostream* stream_;
    LogLevel minimum_level_;
    LogLevel maximum_level_;
    std::mutex mutex_;
};

/// @brief UTF-8 text file에 로그를 출력한다.
class FileLogSink final : public ILogSink
{
public:
    /// @brief file sink를 연다.
    ///
    /// @param path 출력할 file path.
    /// @param append 기존 file 뒤에 이어 쓸지 여부.
    /// @param minimum_level 출력할 최소 level.
    /// @param maximum_level 출력할 최대 level.
    /// @throws std::system_error file을 열 수 없는 경우.
    FileLogSink(
        const std::filesystem::path& path,
        bool append = true,
        LogLevel minimum_level = LogLevel::Trace,
        LogLevel maximum_level = LogLevel::Critical);

    void Write(const LogRecord& record) noexcept override;

    /// @brief file stream이 열린 상태인지 반환한다.
    bool IsOpen() const noexcept;

private:
    std::ofstream stream_;
    LogLevel minimum_level_;
    LogLevel maximum_level_;
    mutable std::mutex mutex_;
};

/// @brief 여러 sink에 같은 구조화 record를 전달하는 logger다.
///
/// sink를 등록하지 않으면 null logger처럼 조용히 동작한다. `Log`는
/// thread-safe하며 logging failure를 호출자에게 전파하지 않는다.
class Logger final
{
public:
    Logger() = default;
    explicit Logger(std::vector<std::shared_ptr<ILogSink>> sinks);

    /// @brief 출력 sink를 추가한다.
    ///
    /// @throws std::invalid_argument `sink`가 null인 경우.
    void AddSink(std::shared_ptr<ILogSink> sink);

    /// @brief 등록된 sink 수의 snapshot을 반환한다.
    std::size_t SinkCount() const;

    /// @brief 하나의 record를 모든 sink에 동기적으로 전달한다.
    ///
    /// @param level 로그 심각도.
    /// @param event 검색과 집계에 사용하는 안정적인 영문 event code.
    /// @param message 사람이 읽는 한국어 설명.
    /// @param fields 선택적인 key/value context.
    void Log(
        LogLevel level,
        std::string_view event,
        std::string_view message,
        std::initializer_list<LogField> fields = {}) const noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;
};

/// @brief info 이하는 stdout, warning 이상은 stderr로 보내는 logger를 만든다.
std::shared_ptr<Logger> MakeConsoleLogger();

} // namespace iocp::core
