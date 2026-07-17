#ifndef QTNG_UTILS_LOGGING_H
#define QTNG_UTILS_LOGGING_H

#include <sstream>
#include <string>

namespace qtng {
namespace utils {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Critical,
    Fatal,
};

class LogStream
{
public:
    LogStream(LogLevel level, const char *category);
    ~LogStream();
    LogStream(const LogStream &) = delete;
    LogStream &operator=(const LogStream &) = delete;
    LogStream(LogStream &&other) noexcept;
    LogStream &operator=(LogStream &&other) = delete;

    template<typename T>
    LogStream &operator<<(const T &value)
    {
        message += toString(value);
        return *this;
    }

private:
    static std::string toString(const std::string &value) { return value; }
    static std::string toString(const char *value) { return value ? value : ""; }
    template<typename T>
    static std::string toString(const T &value)
    {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    static const char *levelName(LogLevel level);
    void flush();

    LogLevel level;
    const char *category;
    std::string message;
    bool active;
};

LogStream ngDebug(const char *category = "qtng");
LogStream ngInfo(const char *category = "qtng");
LogStream ngWarning(const char *category = "qtng");
LogStream ngCritical(const char *category = "qtng");
LogStream ngFatal(const char *category = "qtng");

void logMessage(LogLevel level, const char *category, const std::string &message);

} // namespace utils
} // namespace qtng

// Declare one logger category per translation unit, exactly like Qt's
// Q_LOGGING_CATEGORY. Must be placed at NAMESPACE SCOPE (never inside a
// function body): it emits a file-static category pointer and inline no-arg
// overloads of ngDebug()/ngInfo()/ngWarning()/ngCritical()/ngFatal() that bind
// to it. Subsequent code in the same TU can then write `ngDebug() << ...`.
//
// Use it inside `namespace qtng { ... }` (as the src/ files do) WITHOUT a
// `using namespace qtng::utils;` in scope: the utils overloads carry a default
// category argument, so a using-directive would make the no-arg `ngDebug()`
// ambiguous with the macro-generated overloads. At most one NG_LOGGER per TU.
#define NG_LOGGER(name) \
    static const char *ng_logger_category = name; \
    inline ::qtng::utils::LogStream ngDebug() { return ::qtng::utils::ngDebug(ng_logger_category); } \
    inline ::qtng::utils::LogStream ngInfo() { return ::qtng::utils::ngInfo(ng_logger_category); } \
    inline ::qtng::utils::LogStream ngWarning() { return ::qtng::utils::ngWarning(ng_logger_category); } \
    inline ::qtng::utils::LogStream ngCritical() { return ::qtng::utils::ngCritical(ng_logger_category); } \
    inline ::qtng::utils::LogStream ngFatal() { return ::qtng::utils::ngFatal(ng_logger_category); }

#endif
