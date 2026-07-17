using namespace std;

#include <cstdlib>
#include <iostream>

#include "qtng/utils/logging.h"

namespace qtng {
namespace utils {

const char *LogStream::levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Critical:
        return "CRIT";
    case LogLevel::Fatal:
        return "FATAL";
    default:
        return "DEBUG";
    }
}

void LogStream::flush()
{
    cerr << levelName(level) << " [" << (category ? category : "qtng") << "] " << message << endl;
    if (level == LogLevel::Fatal) {
        abort();
    }
}

LogStream::LogStream(LogLevel level, const char *category)
    : level(level)
    , category(category)
    , active(true)
{
}

LogStream::~LogStream()
{
    if (active) {
        flush();
    }
}

LogStream::LogStream(LogStream &&other) noexcept
    : level(other.level)
    , category(other.category)
    , message(std::move(other.message))
    , active(other.active)
{
    other.active = false;
}

LogStream ngDebug(const char *category)
{
    return LogStream(LogLevel::Debug, category);
}

LogStream ngInfo(const char *category)
{
    return LogStream(LogLevel::Info, category);
}

LogStream ngWarning(const char *category)
{
    return LogStream(LogLevel::Warning, category);
}

LogStream ngCritical(const char *category)
{
    return LogStream(LogLevel::Critical, category);
}

LogStream ngFatal(const char *category)
{
    return LogStream(LogLevel::Fatal, category);
}

void logMessage(LogLevel level, const char *category, const string &message)
{
    LogStream(level, category) << message;
}

}  // namespace utils
}  // namespace qtng
