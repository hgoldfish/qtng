#ifndef QTNG_UTILS_LOGGING_H
#define QTNG_UTILS_LOGGING_H

#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "qtng/config.h"
QTNG_NAMESPACE_BEGIN
namespace utils {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Critical,
    Fatal,
};

namespace detail {

// C++11 void_t. Used only to turn "this substitution is valid" into an SFINAE boolean.
template<typename...>
struct MakeVoid
{
    typedef void type;
};

// Does `std::ostream << value` compile? Types that answer yes keep the plain stream path,
// which is what the qtng containers, String and the numeric/enum types rely on. Note that the
// test is "the expression compiles", not "the type has a sensible textual form": a C array of
// non-char elements answers yes by decaying to a pointer, so `ngDebug() << intArray` still
// prints an address (as it always did) instead of the elements.
template<typename T, typename Enable = void>
struct IsStreamable : std::false_type
{
};

template<typename T>
struct IsStreamable<T, typename MakeVoid<decltype(std::declval<std::ostream &>() << std::declval<const T &>())>::type>
    : std::true_type
{
};

// Is the type a range, i.e. do std::begin/std::end accept it? True for the STL containers,
// for C arrays (which is why IsStreamable above is checked first) and for anything else that
// opted into range-for.
template<typename T, typename Enable = void>
struct IsIterable : std::false_type
{
};

template<typename T>
struct IsIterable<T,
                  typename MakeVoid<decltype(std::begin(std::declval<const T &>())),
                                    decltype(std::end(std::declval<const T &>()))>::type> : std::true_type
{
};

// A key_type without a mapped_type is a set; mapped_type is what turns a range into a map.
// Those two typedefs are how the container layouts are told apart without naming any concrete
// STL type.
template<typename T, typename Enable = void>
struct HasKeyType : std::false_type
{
};

template<typename T>
struct HasKeyType<T, typename MakeVoid<typename T::key_type>::type> : std::true_type
{
};

template<typename T, typename Enable = void>
struct HasMappedType : std::false_type
{
};

template<typename T>
struct HasMappedType<T, typename MakeVoid<typename T::mapped_type>::type> : std::true_type
{
};

// A range with no operator<< of its own: the only types the STL formatters below may see.
// Naming it is what makes the three formatter conditions exact complements of each other,
// instead of three copies of "!IsStreamable && IsIterable" that have to be kept consistent.
template<typename T>
struct IsPlainRange : std::integral_constant<bool, IsIterable<T>::value && !IsStreamable<T>::value>
{
};

}  // namespace detail

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
        if (needSpace) {
            message += " ";
        }
        message += toString(value);
        needSpace = true;
        return *this;
    }

private:
    static std::string toString(const std::string &value) { return value; }
    static std::string toString(const char *value) { return value ? value : ""; }

    // Layout by precedence: anything that is not a bare range streams itself (this covers the
    // qtng containers, String, numbers and enums, so nothing that worked before changes); then
    // maps; then the remaining ranges. The three conditions are exact complements, so exactly
    // one overload matches -- there is no predicate to keep in sync with another.
    template<typename T>
    static typename std::enable_if<!detail::IsPlainRange<T>::value, std::string>::type toString(const T &value)
    {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    // {key: value, ...}
    template<typename T>
    static typename std::enable_if<detail::IsPlainRange<T>::value && detail::HasMappedType<T>::value, std::string>::type
    toString(const T &value)
    {
        std::string out = "{";
        bool first = true;
        for (const auto &entry : value) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += toString(entry.first);
            out += ": ";
            out += toString(entry.second);
        }
        out += "}";
        return out;
    }

    // [a, b] for sequences, {a, b} for sets.
    template<typename T>
    static typename std::enable_if<detail::IsPlainRange<T>::value && !detail::HasMappedType<T>::value,
                                   std::string>::type
    toString(const T &value)
    {
        const bool isSet = detail::HasKeyType<T>::value;
        return toStringRange(value, isSet ? '{' : '[', isSet ? '}' : ']');
    }

    // Each element goes through toString() again, so nesting (map of vectors, vector of
    // maps, ...) formats itself without any special case here.
    template<typename T>
    static std::string toStringRange(const T &value, char open, char close)
    {
        std::string out(1, open);
        bool first = true;
        for (const auto &element : value) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += toString(element);
        }
        out.push_back(close);
        return out;
    }

    static const char *levelName(LogLevel level);
    void flush();

    LogLevel level;
    const char *category;
    std::string message;
    bool active;
    bool needSpace;
};

LogStream ngDebug(const char *category = "qtng");
LogStream ngInfo(const char *category = "qtng");
LogStream ngWarning(const char *category = "qtng");
LogStream ngCritical(const char *category = "qtng");
LogStream ngFatal(const char *category = "qtng");

void logMessage(LogLevel level, const char *category, const std::string &message);

// Optional process-wide sink for ngDebug/ngInfo/ngWarning/ngCritical/ngFatal.
// When set, LogStream::flush() invokes the handler instead of writing to stderr.
// Pass nullptr to restore the default cerr sink. Not thread-safe to swap while
// other threads are flushing; install once at process startup.
typedef void (*LogHandler)(LogLevel level, const char *category, const std::string &message);
void setLogHandler(LogHandler handler);
LogHandler logHandler();

} // namespace utils
QTNG_NAMESPACE_END
// Declare one logger category per translation unit, exactly like Qt's
// Q_LOGGING_CATEGORY. Must be placed at NAMESPACE SCOPE (never inside a
// function body): it emits a file-static category pointer and inline no-arg
// overloads of ngDebug()/ngInfo()/ngWarning()/ngCritical()/ngFatal() that bind
// to it. Subsequent code in the same TU can then write `ngDebug() << ...`.
//
// Use it inside `QTNG_NAMESPACE_BEGIN ... QTNG_NAMESPACE_END` (as the src/ files do) WITHOUT a
// `using namespace QTNG_NAMESPACE::utils;` in scope: the utils overloads carry a default
// category argument, so a using-directive would make the no-arg `ngDebug()`
// ambiguous with the macro-generated overloads. At most one NG_LOGGER per TU.
#define NG_LOGGER(name) \
    static const char *ng_logger_category = name; \
    inline ::QTNG_NAMESPACE::utils::LogStream ngDebug() { return ::QTNG_NAMESPACE::utils::ngDebug(ng_logger_category); } \
    inline ::QTNG_NAMESPACE::utils::LogStream ngInfo() { return ::QTNG_NAMESPACE::utils::ngInfo(ng_logger_category); } \
    inline ::QTNG_NAMESPACE::utils::LogStream ngWarning() { return ::QTNG_NAMESPACE::utils::ngWarning(ng_logger_category); } \
    inline ::QTNG_NAMESPACE::utils::LogStream ngCritical() { return ::QTNG_NAMESPACE::utils::ngCritical(ng_logger_category); } \
    inline ::QTNG_NAMESPACE::utils::LogStream ngFatal() { return ::QTNG_NAMESPACE::utils::ngFatal(ng_logger_category); }

#endif
