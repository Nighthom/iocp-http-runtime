#include "core/logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace iocp::core
{

namespace
{

bool Accepts(
    const LogLevel level,
    const LogLevel minimum_level,
    const LogLevel maximum_level) noexcept
{
    return level >= minimum_level && level <= maximum_level;
}

const char* LevelName(const LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Critical:
        return "CRITICAL";
    }
    return "UNKNOWN";
}

void WriteEscaped(std::ostream& stream, const std::string_view value)
{
    stream << '"';
    for (const char character : value)
    {
        switch (character)
        {
        case '\\':
            stream << "\\\\";
            break;
        case '"':
            stream << "\\\"";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            stream << character;
            break;
        }
    }
    stream << '"';
}

std::string FormatRecord(const LogRecord& record)
{
    std::ostringstream stream;
    stream << std::setfill('0')
           << std::setw(4) << record.timestamp.year << '-'
           << std::setw(2) << record.timestamp.month << '-'
           << std::setw(2) << record.timestamp.day << 'T'
           << std::setw(2) << record.timestamp.hour << ':'
           << std::setw(2) << record.timestamp.minute << ':'
           << std::setw(2) << record.timestamp.second << '.'
           << std::setw(3) << record.timestamp.milliseconds
           << "Z [" << LevelName(record.level) << "] [thread="
           << record.thread_id << "] event=";
    WriteEscaped(stream, record.event);
    stream << " message=";
    WriteEscaped(stream, record.message);

    for (std::size_t index = 0; index < record.field_count; ++index)
    {
        stream << ' ' << record.fields[index].key << '=';
        WriteEscaped(stream, record.fields[index].value);
    }

    stream << '\n';
    return stream.str();
}

LogTimestamp CurrentUtcTimestamp() noexcept
{
    SYSTEMTIME system_time{};
    ::GetSystemTime(&system_time);

    return LogTimestamp{
        system_time.wYear,
        system_time.wMonth,
        system_time.wDay,
        system_time.wHour,
        system_time.wMinute,
        system_time.wSecond,
        system_time.wMilliseconds,
    };
}

} // namespace

StreamLogSink::StreamLogSink(
    std::ostream& stream,
    const LogLevel minimum_level,
    const LogLevel maximum_level) noexcept
    : stream_(&stream),
      minimum_level_(minimum_level),
      maximum_level_(maximum_level)
{
}

void StreamLogSink::Write(const LogRecord& record) noexcept
{
    if (!Accepts(record.level, minimum_level_, maximum_level_))
    {
        return;
    }

    try
    {
        const std::string line = FormatRecord(record);
        std::lock_guard lock(mutex_);
        *stream_ << line;
        stream_->flush();
    }
    catch (...)
    {
        // logging failure가 application의 실행 흐름을 바꾸지 않게 삼킨다.
    }
}

FileLogSink::FileLogSink(
    const std::filesystem::path& path,
    const bool append,
    const LogLevel minimum_level,
    const LogLevel maximum_level)
    : stream_(
          path,
          std::ios::binary | std::ios::out |
              (append ? std::ios::app : std::ios::trunc)),
      minimum_level_(minimum_level),
      maximum_level_(maximum_level)
{
    if (!stream_.is_open())
    {
        throw std::system_error(
            std::make_error_code(std::errc::io_error),
            "로그 file을 열 수 없습니다: " + path.string());
    }
}

void FileLogSink::Write(const LogRecord& record) noexcept
{
    if (!Accepts(record.level, minimum_level_, maximum_level_))
    {
        return;
    }

    try
    {
        const std::string line = FormatRecord(record);
        std::lock_guard lock(mutex_);
        stream_ << line;
        stream_.flush();
    }
    catch (...)
    {
        // file I/O failure는 향후 health metric으로 노출하고 여기서는 전파하지 않는다.
    }
}

bool FileLogSink::IsOpen() const noexcept
{
    try
    {
        std::lock_guard lock(mutex_);
        return stream_.is_open();
    }
    catch (...)
    {
        return false;
    }
}

Logger::Logger(std::vector<std::shared_ptr<ILogSink>> sinks)
    : sinks_(std::move(sinks))
{
    for (const auto& sink : sinks_)
    {
        if (!sink)
        {
            throw std::invalid_argument("Logger sink는 null일 수 없습니다");
        }
    }
}

void Logger::AddSink(std::shared_ptr<ILogSink> sink)
{
    if (!sink)
    {
        throw std::invalid_argument("Logger sink는 null일 수 없습니다");
    }

    std::lock_guard lock(mutex_);
    sinks_.push_back(std::move(sink));
}

std::size_t Logger::SinkCount() const
{
    std::lock_guard lock(mutex_);
    return sinks_.size();
}

void Logger::Log(
    const LogLevel level,
    const std::string_view event,
    const std::string_view message,
    const std::initializer_list<LogField> fields) const noexcept
{
    const LogRecord record{
        CurrentUtcTimestamp(),
        level,
        static_cast<std::uint64_t>(::GetCurrentThreadId()),
        event,
        message,
        fields.begin(),
        fields.size(),
    };

    std::vector<std::shared_ptr<ILogSink>> sinks;
    try
    {
        std::lock_guard lock(mutex_);
        sinks = sinks_;
    }
    catch (...)
    {
        return;
    }

    // sink I/O 중에는 Logger mutex를 잡지 않는다. 느린 file 출력이 sink 추가를
    // 막거나 custom sink의 logger 재진입이 deadlock을 만들지 않게 한다.
    for (const auto& sink : sinks)
    {
        sink->Write(record);
    }
}

std::shared_ptr<Logger> MakeConsoleLogger()
{
    auto logger = std::make_shared<Logger>();
    logger->AddSink(std::make_shared<StreamLogSink>(
        std::cout,
        LogLevel::Trace,
        LogLevel::Info));
    logger->AddSink(std::make_shared<StreamLogSink>(
        std::cerr,
        LogLevel::Warning,
        LogLevel::Critical));
    return logger;
}

} // namespace iocp::core
