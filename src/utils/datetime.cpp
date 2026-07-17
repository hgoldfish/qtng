using namespace std;

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "qtng/utils/datetime.h"
#include "qtng/utils/platform.h"
#include "qtng/utils/string_utils.h"

namespace qtng {
namespace utils {

DateTime::DateTime()
    : tp()
    , valid(false)
{
}

DateTime::DateTime(chrono::system_clock::time_point tp)
    : tp(tp)
    , valid(true)
{
}

DateTime DateTime::currentDateTimeUtc()
{
    return DateTime(chrono::system_clock::now());
}

int64_t DateTime::currentMSecsSinceEpoch()
{
    return chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
}

DateTime DateTime::fromSecsSinceEpoch(int64_t secs)
{
    return DateTime(chrono::system_clock::time_point(chrono::seconds(secs)));
}

DateTime DateTime::fromUtc(int year, int month, int day, int hour, int minute, int second, int msec)
{
    tm utc {};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    utc.tm_isdst = 0;
#if defined(NG_OS_WIN)
    time_t secs = _mkgmtime(&utc);
#else
    time_t secs = timegm(&utc);
#endif
    if (secs == static_cast<time_t>(-1)) {
        return DateTime();
    }
    return DateTime(chrono::system_clock::time_point(chrono::seconds(secs) + chrono::milliseconds(msec)));
}

DateTime DateTime::addSecs(int64_t secs) const
{
    if (!valid) {
        return DateTime();
    }
    return DateTime(tp + chrono::seconds(secs));
}

int64_t DateTime::secsTo(const DateTime &other) const
{
    if (!valid || !other.valid) {
        return 0;
    }
    return chrono::duration_cast<chrono::seconds>(other.tp - tp).count();
}

DateTime DateTime::fromMSecsSinceEpoch(int64_t msecs)
{
    return DateTime(chrono::system_clock::time_point(chrono::milliseconds(msecs)));
}

int64_t DateTime::toSecsSinceEpoch() const
{
    if (!valid) {
        return 0;
    }
    return chrono::duration_cast<chrono::seconds>(tp.time_since_epoch()).count();
}

int64_t DateTime::toMSecsSinceEpoch() const
{
    if (!valid) {
        return 0;
    }
    return chrono::duration_cast<chrono::milliseconds>(tp.time_since_epoch()).count();
}

string DateTime::toString(const string &format) const
{
    if (!valid) {
        return string();
    }
    time_t secs = static_cast<time_t>(toSecsSinceEpoch());
    tm utc {};
#if defined(NG_OS_WIN)
    gmtime_s(&utc, &secs);
#else
    gmtime_r(&secs, &utc);
#endif
    char buffer[128];
    const char *fmt = format.empty() ? "%Y-%m-%dT%H:%M:%SZ" : format.c_str();
    if (strftime(buffer, sizeof(buffer), fmt, &utc) == 0) {
        return string();
    }
    return string(buffer);
}

string DateTime::toHttpDate() const
{
    if (!valid) {
        return string();
    }
    time_t secs = static_cast<time_t>(toSecsSinceEpoch());
    tm utc {};
#if defined(NG_OS_WIN)
    gmtime_s(&utc, &secs);
#else
    gmtime_r(&secs, &utc);
#endif
    char buffer[128];
    if (strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc) == 0) {
        return string();
    }
    return string(buffer);
}

ElapsedTimer::ElapsedTimer()
    : start(chrono::steady_clock::now())
{
}

void ElapsedTimer::restart()
{
    start = chrono::steady_clock::now();
}

int64_t ElapsedTimer::elapsed() const
{
    return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count();
}

int64_t ElapsedTimer::elapsedMicroseconds() const
{
    return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - start).count();
}

}  // namespace utils
}  // namespace qtng
