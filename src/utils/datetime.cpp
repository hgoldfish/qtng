#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "qtng/utils/datetime.h"
#include "qtng/utils/platform.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {
namespace utils {

namespace {

#if defined(NG_OS_WIN)
// XP's msvcrt.dll does not export _mkgmtime / _mkgmtime32 / _mkgmtime64.
// Use a portable UTC mktime instead of the MSVCRT helpers.
time_t timegmUtc(tm *utc)
{
    static const int kDaysBeforeMonth[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (!utc || utc->tm_mon < 0 || utc->tm_mon > 11) {
        return static_cast<time_t>(-1);
    }
    const int year = utc->tm_year + 1900;
    int64_t days = 0;
    if (year >= 1970) {
        for (int y = 1970; y < year; ++y) {
            days += Date::isLeapYear(y) ? 366 : 365;
        }
    } else {
        for (int y = year; y < 1970; ++y) {
            days -= Date::isLeapYear(y) ? 366 : 365;
        }
    }
    days += kDaysBeforeMonth[utc->tm_mon];
    if (utc->tm_mon > 1 && Date::isLeapYear(year)) {
        days += 1;
    }
    days += utc->tm_mday - 1;
    const int64_t secs = days * 86400LL + static_cast<int64_t>(utc->tm_hour) * 3600LL
        + static_cast<int64_t>(utc->tm_min) * 60LL + static_cast<int64_t>(utc->tm_sec);
    return static_cast<time_t>(secs);
}
#endif

// Julian Day Number corresponding to 1970-01-01.
const int64_t kJulianDayOfUnixEpoch = 2440588;

// Howard Hinnant's algorithm converting a civil date to days since the epoch.
int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Convert days since the epoch back to a civil date (Howard Hinnant's algorithm).
void civilFromDays(int64_t z, int *year, unsigned *month, unsigned *day)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    *year = static_cast<int>(y + (m <= 2));
    *month = m;
    *day = d;
}

time_t localTimegm(tm *utc)
{
#if defined(NG_OS_WIN)
    return timegmUtc(utc);
#else
    return timegm(utc);
#endif
}

void gmtimeR(const time_t *timep, tm *result)
{
#if defined(NG_OS_WIN)
    gmtime_s(result, timep);
#else
    gmtime_r(timep, result);
#endif
}

void localtimeR(const time_t *timep, tm *result)
{
#if defined(NG_OS_WIN)
    localtime_s(result, timep);
#else
    localtime_r(timep, result);
#endif
}

int msecOf(const chrono::system_clock::time_point &tp)
{
    return static_cast<int>(chrono::duration_cast<chrono::milliseconds>(tp.time_since_epoch()).count() % 1000);
}

int parseFixedDigits(const string &str, size_t *pos, size_t count)
{
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t at = *pos + i;
        if (at >= str.size() || str[at] < '0' || str[at] > '9') {
            return -1;
        }
        value = value * 10 + (str[at] - '0');
    }
    *pos += count;
    return value;
}

// Expand the supported components (yyyy/MM/dd/HH/mm/ss/zzz) of a Qt-style date format string.
// Other characters are emitted verbatim; strftime's %X directive is passed through.
string formatDateTimeTokens(const tm &local, const tm &utc, int msec, bool isLocal, const string &format)
{
    const tm &base = isLocal ? local : utc;
    string out;
    out.reserve(format.size() + 8);
    for (size_t i = 0; i < format.size();) {
        if (format[i] == '%' && i + 1 < format.size()) {
            char buffer[64];
            const char directive[3] = {format[i], format[i + 1], '\0'};
            if (strftime(buffer, sizeof(buffer), directive, &base) > 0) {
                out += buffer;
            }
            i += 2;
            continue;
        }
        if (format.compare(i, 4, "yyyy") == 0) {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%04d", base.tm_year + 1900);
            out += buffer;
            i += 4;
            continue;
        }
        if (format.compare(i, 2, "MM") == 0) {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%02d", base.tm_mon + 1);
            out += buffer;
            i += 2;
            continue;
        }
        if (format.compare(i, 2, "dd") == 0) {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%02d", base.tm_mday);
            out += buffer;
            i += 2;
            continue;
        }
        if (format.compare(i, 2, "HH") == 0) {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%02d", base.tm_hour);
            out += buffer;
            i += 2;
            continue;
        }
        if (format.compare(i, 2, "mm") == 0) {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%02d", base.tm_min);
            out += buffer;
            i += 2;
            continue;
        }
        if (format.compare(i, 2, "ss") == 0) {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%02d", base.tm_sec);
            out += buffer;
            i += 2;
            continue;
        }
        if (format.compare(i, 3, "zzz") == 0) {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%03d", msec);
            out += buffer;
            i += 3;
            continue;
        }
        out += format[i];
        ++i;
    }
    return out;
}

}  // namespace

Date::Date()
    : jd(0)
{
}

Date::Date(int year, int month, int day)
{
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        jd = 0;
        return;
    }
    const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    jd = days + kJulianDayOfUnixEpoch;
}

Date Date::fromJulianDay(int64_t julianDay)
{
    Date d;
    d.jd = julianDay;
    return d;
}

Date Date::fromString(const string &str, const string &format)
{
    if (format == "yyyy-MM-dd") {
        size_t pos = 0;
        const int y = parseFixedDigits(str, &pos, 4);
        if (y < 0 || pos >= str.size() || str[pos] != '-') {
            return Date();
        }
        ++pos;
        const int m = parseFixedDigits(str, &pos, 2);
        if (m < 0 || pos >= str.size() || str[pos] != '-') {
            return Date();
        }
        ++pos;
        const int d = parseFixedDigits(str, &pos, 2);
        if (d < 0) {
            return Date();
        }
        return Date(y, m, d);
    }
    return Date();
}

Date Date::currentDate()
{
    const time_t now = time(nullptr);
    tm local {};
    localtimeR(&now, &local);
    return Date(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

bool Date::isValid() const
{
    return jd != 0;
}

int Date::year() const
{
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civilFromDays(jd - kJulianDayOfUnixEpoch, &y, &m, &d);
    return y;
}

int Date::month() const
{
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civilFromDays(jd - kJulianDayOfUnixEpoch, &y, &m, &d);
    return static_cast<int>(m);
}

int Date::day() const
{
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civilFromDays(jd - kJulianDayOfUnixEpoch, &y, &m, &d);
    return static_cast<int>(d);
}

int Date::dayOfWeek() const
{
    if (!isValid()) {
        return 0;
    }
    return static_cast<int>(jd % 7) + 1;  // jd mod 7: 0 -> Monday, 6 -> Sunday
}

bool Date::isLeapYear(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int Date::daysInMonth(int year, int month)
{
    if (month < 1 || month > 12) {
        return 0;  // mirroring QDate: returns 0 for invalid arguments
    }
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && isLeapYear(year)) {
        return 29;
    }
    return days[month - 1];
}

int Date::daysInMonth() const
{
    if (!isValid()) {
        return 0;
    }
    return daysInMonth(year(), month());
}

int Date::weekNumber(int *yearNumber) const
{
    if (!isValid()) {
        return 0;
    }
    // The ISO-8601 week number and year are determined by the Thursday of that week: use the year of the
    // week's Thursday.
    const Date thursday = addDays(4 - dayOfWeek());
    if (yearNumber) {
        *yearNumber = thursday.year();
    }
    const Date firstDay(thursday.year(), 1, 1);
    return static_cast<int>(firstDay.daysTo(thursday) / 7 + 1);
}

int64_t Date::toJulianDay() const
{
    return jd;
}

string Date::toString(const string &format) const
{
    if (!isValid()) {
        return string();
    }
    tm utc {};
    utc.tm_year = year() - 1900;
    utc.tm_mon = month() - 1;
    utc.tm_mday = day();
    return formatDateTimeTokens(utc, utc, 0, false, format.empty() ? "yyyy-MM-dd" : format);
}

Date Date::addDays(int days) const
{
    if (!isValid()) {
        return Date();
    }
    return Date::fromJulianDay(jd + days);
}

int64_t Date::daysTo(const Date &other) const
{
    return other.jd - jd;
}

DateTime::DateTime()
    : tp()
    , valid(false)
    , local(false)
{
}

DateTime::DateTime(chrono::system_clock::time_point tp)
    : tp(tp)
    , valid(true)
    , local(false)
{
}

DateTime::DateTime(const Date &localDate, int hour, int minute, int second, int msec)
{
    if (!localDate.isValid()) {
        tp = chrono::system_clock::time_point();
        valid = false;
        local = false;
        return;
    }
    tm localTm {};
    localTm.tm_year = localDate.year() - 1900;
    localTm.tm_mon = localDate.month() - 1;
    localTm.tm_mday = localDate.day();
    localTm.tm_hour = hour;
    localTm.tm_min = minute;
    localTm.tm_sec = second;
    localTm.tm_isdst = -1;
    time_t secs = mktime(&localTm);
    if (secs == static_cast<time_t>(-1)) {
        tp = chrono::system_clock::time_point();
        valid = false;
        local = false;
        return;
    }
    tp = chrono::system_clock::time_point(chrono::seconds(secs) + chrono::milliseconds(msec));
    valid = true;
    local = false;
}

DateTime DateTime::currentDateTimeUtc()
{
    return DateTime(chrono::system_clock::now());
}

DateTime DateTime::currentDateTime()
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
    const time_t secs = localTimegm(&utc);
    if (secs == static_cast<time_t>(-1)) {
        return DateTime();
    }
    return DateTime(chrono::system_clock::time_point(chrono::seconds(secs) + chrono::milliseconds(msec)));
}

DateTime DateTime::fromString(const string &str)
{
    if (str.empty()) {
        return DateTime();
    }
    string body = str;
    if (!body.empty() && body[body.size() - 1] == 'Z') {
        body = body.substr(0, body.size() - 1);
    }
    size_t pos = 0;
    const int y = parseFixedDigits(body, &pos, 4);
    if (y < 0 || pos >= body.size() || body[pos] != '-') {
        return DateTime();
    }
    ++pos;
    const int m = parseFixedDigits(body, &pos, 2);
    if (m < 0 || pos >= body.size() || body[pos] != '-') {
        return DateTime();
    }
    ++pos;
    const int d = parseFixedDigits(body, &pos, 2);
    if (d < 0) {
        return DateTime();
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    int msec = 0;
    if (pos < body.size()) {
        if (body[pos] == 'T' || body[pos] == ' ') {
            ++pos;
            hour = parseFixedDigits(body, &pos, 2);
            if (hour < 0 || pos >= body.size() || body[pos] != ':') {
                return DateTime();
            }
            ++pos;
            minute = parseFixedDigits(body, &pos, 2);
            if (minute < 0) {
                return DateTime();
            }
            if (pos < body.size() && body[pos] == ':') {
                ++pos;
                second = parseFixedDigits(body, &pos, 2);
                if (second < 0) {
                    return DateTime();
                }
            }
            if (pos < body.size() && body[pos] == '.') {
                ++pos;
                msec = parseFixedDigits(body, &pos, 3);
                if (msec < 0) {
                    return DateTime();
                }
            }
        }
    }
    const Date date(y, m, d);
    if (!date.isValid()) {
        return DateTime();
    }
    DateTime dt = fromUtc(y, m, d, hour, minute, second, msec);
    return dt.isValid() ? dt : DateTime();
}

DateTime DateTime::addSecs(int64_t secs) const
{
    if (!valid) {
        return DateTime();
    }
    return DateTime(tp + chrono::seconds(secs));
}

DateTime DateTime::addDays(int days) const
{
    if (!valid) {
        return DateTime();
    }
    const Date d = localDate().addDays(days);
    if (!d.isValid()) {
        return DateTime();
    }
    tm local {};
    const time_t secs = static_cast<time_t>(toSecsSinceEpoch());
    localtimeR(&secs, &local);
    return DateTime(d, local.tm_hour, local.tm_min, local.tm_sec, msecOf(tp));
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

DateTime DateTime::toUTC() const
{
    DateTime result = *this;
    result.local = false;
    return result;
}

DateTime DateTime::toLocalTime() const
{
    DateTime result = *this;
    result.local = true;
    return result;
}

Date DateTime::date() const
{
    if (!valid) {
        return Date();
    }
    if (local) {
        return localDate();
    }
    const time_t secs = static_cast<time_t>(toSecsSinceEpoch());
    tm utc {};
    gmtimeR(&secs, &utc);
    return Date(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
}

Date DateTime::localDate() const
{
    if (!valid) {
        return Date();
    }
    const time_t secs = static_cast<time_t>(toSecsSinceEpoch());
    tm local {};
    localtimeR(&secs, &local);
    return Date(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

string DateTime::toString(const string &format) const
{
    if (!valid) {
        return string();
    }
    const time_t secs = static_cast<time_t>(toSecsSinceEpoch());
    tm utc {};
    tm localTm {};
    gmtimeR(&secs, &utc);
    localtimeR(&secs, &localTm);
    const bool isLocal = local;
    if (format.empty()) {
        const string defaultFormat = isLocal ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%dT%H:%M:%SZ";
        return formatDateTimeTokens(localTm, utc, msecOf(tp), isLocal, defaultFormat);
    }
    return formatDateTimeTokens(localTm, utc, msecOf(tp), isLocal, format);
}

string DateTime::toHttpDate() const
{
    if (!valid) {
        return string();
    }
    const time_t secs = static_cast<time_t>(toSecsSinceEpoch());
    tm utc {};
    gmtimeR(&secs, &utc);
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
