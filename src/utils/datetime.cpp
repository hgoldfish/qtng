#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "qtng/utils/datetime.h"
#include "qtng/utils/platform.h"
#include "qtng/utils/string_utils.h"

using namespace std;

QTNG_NAMESPACE_BEGIN
namespace utils {

namespace {

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

struct CivilDate
{
    int year;
    int month;
    int day;
};

// Convert days since the epoch back to a civil date (Howard Hinnant's algorithm).
CivilDate civilFromDays(int64_t z)
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
    CivilDate out {};
    out.year = static_cast<int>(y + (m <= 2));
    out.month = static_cast<int>(m);
    out.day = static_cast<int>(d);
    return out;
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
    const int64_t msecs = chrono::duration_cast<chrono::milliseconds>(tp.time_since_epoch()).count();
    const int64_t remainder = msecs % 1000;
    return static_cast<int>(remainder < 0 ? remainder + 1000 : remainder);
}

int parseFixedDigits(const char *str, size_t size, size_t *pos, size_t count)
{
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t at = *pos + i;
        if (at >= size || str[at] < '0' || str[at] > '9') {
            return -1;
        }
        value = value * 10 + (str[at] - '0');
    }
    *pos += count;
    return value;
}

enum class Field { Year, Month, Day, Hour, Minute, Second, Millisecond };

struct FormatToken
{
    const char *text;
    size_t length;
    Field field;
    int digits;
};

const FormatToken kFormatTokens[] = {
    {"yyyy", 4, Field::Year, 4},
    {"zzz", 3, Field::Millisecond, 3},
    {"MM", 2, Field::Month, 2},
    {"dd", 2, Field::Day, 2},
    {"HH", 2, Field::Hour, 2},
    {"mm", 2, Field::Minute, 2},
    {"ss", 2, Field::Second, 2},
};

const FormatToken *matchFormatToken(const string &format, size_t at)
{
    for (const FormatToken &token : kFormatTokens) {
        if (format.compare(at, token.length, token.text) == 0) {
            return &token;
        }
    }
    return nullptr;
}

struct ParsedFields
{
    int year = -1;
    int month = -1;
    int day = -1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int msec = 0;
};

// Parse data strictly against a token format: literals must match exactly, every digit group must be that
// wide, the whole input must be consumed, and yyyy/MM/dd must all be present.
bool parseWithFormat(const char *data, size_t size, const string &format, ParsedFields *fields)
{
    size_t pos = 0;
    for (size_t i = 0; i < format.size();) {
        const FormatToken *token = matchFormatToken(format, i);
        if (!token) {
            if (pos >= size || data[pos] != format[i]) {
                return false;
            }
            ++pos;
            ++i;
            continue;
        }
        const int value = parseFixedDigits(data, size, &pos, static_cast<size_t>(token->digits));
        if (value < 0) {
            return false;
        }
        switch (token->field) {
        case Field::Year:
            fields->year = value;
            break;
        case Field::Month:
            fields->month = value;
            break;
        case Field::Day:
            fields->day = value;
            break;
        case Field::Hour:
            fields->hour = value;
            break;
        case Field::Minute:
            fields->minute = value;
            break;
        case Field::Second:
            fields->second = value;
            break;
        case Field::Millisecond:
            fields->msec = value;
            break;
        }
        i += token->length;
    }
    // Both callers need a complete calendar date; a format without yyyy/MM/dd cannot produce one.
    return pos == size && fields->year >= 0 && fields->month >= 0 && fields->day >= 0;
}

bool isValidTimeOfDay(int hour, int minute, int second, int msec)
{
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59 && msec >= 0
        && msec <= 999;
}

// Expand the supported components (yyyy/MM/dd/HH/mm/ss/zzz) of a Qt-style date format string.
// Other characters are emitted verbatim; strftime's %X directive is passed through.
string formatDateTimeTokens(const tm &base, int msec, const string &format)
{
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
        const FormatToken *token = matchFormatToken(format, i);
        if (!token) {
            out += format[i];
            ++i;
            continue;
        }
        int value = 0;
        switch (token->field) {
        case Field::Year:
            value = base.tm_year + 1900;
            break;
        case Field::Month:
            value = base.tm_mon + 1;
            break;
        case Field::Day:
            value = base.tm_mday;
            break;
        case Field::Hour:
            value = base.tm_hour;
            break;
        case Field::Minute:
            value = base.tm_min;
            break;
        case Field::Second:
            value = base.tm_sec;
            break;
        case Field::Millisecond:
            value = msec;
            break;
        }
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%0*d", token->digits, value);
        out += buffer;
        i += token->length;
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
    const int lastDayOfMonth = daysInMonth(year, month);
    if (day < 1 || day > lastDayOfMonth) {
        jd = 0;
        return;
    }
    const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    // jd == 0 is the invalid marker, so the representable calendar starts at jd 1 (-4713-11-25). Without this
    // clamp that single day would be a hole in the middle of an otherwise continuous range, and addDays()
    // stepping onto it would poison every following call.
    const int64_t julianDay = days + kJulianDayOfUnixEpoch;
    jd = julianDay >= 1 ? julianDay : 0;
}

Date Date::fromJulianDay(int64_t julianDay)
{
    Date d;
    d.jd = julianDay >= 1 ? julianDay : 0;
    return d;
}

Date Date::fromString(const char *str, size_t size, const string &format)
{
    if (!str || format.empty()) {
        return Date();
    }
    ParsedFields fields;
    if (!parseWithFormat(str, size, format, &fields)) {
        return Date();
    }
    return Date(fields.year, fields.month, fields.day);
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
    // An invalid Date must not leak the decomposition of the jd == 0 sentinel as if it were a real date.
    return isValid() ? civilFromDays(jd - kJulianDayOfUnixEpoch).year : 0;
}

int Date::month() const
{
    return isValid() ? civilFromDays(jd - kJulianDayOfUnixEpoch).month : 0;
}

int Date::day() const
{
    return isValid() ? civilFromDays(jd - kJulianDayOfUnixEpoch).day : 0;
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
    // The ISO-8601 week number and year are determined by the Thursday of that week.
    const int64_t thursday = jd + 4 - dayOfWeek();
    const int isoYear = civilFromDays(thursday - kJulianDayOfUnixEpoch).year;
    if (yearNumber) {
        *yearNumber = isoYear;
    }
    // Stay in Julian days instead of building Date(isoYear, 1, 1): that date falls before the representable
    // lower bound for the earliest weeks, and clamping it to invalid made daysTo() return 0, which reported
    // every week of 4713 BC as week 1.
    const int64_t jan1 = daysFromCivil(isoYear, 1, 1) + kJulianDayOfUnixEpoch;
    return static_cast<int>((thursday - jan1) / 7 + 1);
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
    return formatDateTimeTokens(utc, 0, format.empty() ? "yyyy-MM-dd" : format);
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
    // Same contract as DateTime::secsTo(): an invalid operand yields 0 instead of arithmetic on the sentinel.
    if (!isValid() || !other.isValid()) {
        return 0;
    }
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
    if (!localDate.isValid() || !isValidTimeOfDay(hour, minute, second, msec)) {
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
    const time_t secs = mktime(&localTm);
    if (secs == static_cast<time_t>(-1)) {
        // -1 is both mktime's error code and the legitimate local time 1969-12-31T23:59:59; a round trip
        // through localtime_r() is the only way to tell them apart.
        tm back {};
        localtimeR(&secs, &back);
        if (back.tm_year != localDate.year() - 1900 || back.tm_mon != localDate.month() - 1
            || back.tm_mday != localDate.day() || back.tm_hour != hour || back.tm_min != minute
            || back.tm_sec != second) {
            tp = chrono::system_clock::time_point();
            valid = false;
            local = false;
            return;
        }
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
    const Date date(year, month, day);
    if (!date.isValid() || !isValidTimeOfDay(hour, minute, second, msec)) {
        return DateTime();
    }
    // Pure arithmetic on the Julian day the Date already holds. Going through timegm() here would be both
    // normalizing and ambiguous: it reports failure with -1, which is also the legitimate instant
    // 1969-12-31T23:59:59Z, and on 32-bit time_t it cannot represent dates past 2038 at all.
    const int64_t secs = (date.toJulianDay() - kJulianDayOfUnixEpoch) * 86400 + hour * 3600 + minute * 60
        + second;
    return DateTime(chrono::system_clock::time_point(chrono::seconds(secs) + chrono::milliseconds(msec)));
}

DateTime DateTime::fromString(const char *str, size_t size, const string &format)
{
    if (!str) {
        return DateTime();
    }
    if (!format.empty()) {
        ParsedFields fields;
        if (!parseWithFormat(str, size, format, &fields)) {
            return DateTime();
        }
        return fromUtc(fields.year, fields.month, fields.day, fields.hour, fields.minute, fields.second,
                       fields.msec);
    }
    // Tolerant ISO-8601: what "ISO but not exactly one format" really means is this fixed list of shapes.
    if (size > 0 && str[size - 1] == 'Z') {
        --size;
    }
    static const char *const kIsoFormats[] = {
        "yyyy-MM-dd",          "yyyy-MM-ddTHH:mm",     "yyyy-MM-dd HH:mm",     "yyyy-MM-ddTHH:mm:ss",
        "yyyy-MM-dd HH:mm:ss", "yyyy-MM-ddTHH:mm:ss.zzz", "yyyy-MM-dd HH:mm:ss.zzz",
    };
    for (const char *isoFormat : kIsoFormats) {
        ParsedFields fields;
        if (parseWithFormat(str, size, isoFormat, &fields)) {
            return fromUtc(fields.year, fields.month, fields.day, fields.hour, fields.minute, fields.second,
                           fields.msec);
        }
    }
    return DateTime();
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
    // Floor, not truncate: 1969-12-31T23:59:59.500 belongs to second -1, and both toString() and
    // toMSecsSinceEpoch() must agree with it. Written without subtraction to stay clear of INT64_MIN.
    const int64_t msecs = toMSecsSinceEpoch();
    const int64_t secs = msecs / 1000;
    return msecs % 1000 < 0 ? secs - 1 : secs;
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
    tm base {};
    if (local) {
        localtimeR(&secs, &base);
    } else {
        gmtimeR(&secs, &base);
    }
    if (format.empty()) {
        const string defaultFormat = local ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%dT%H:%M:%SZ";
        return formatDateTimeTokens(base, msecOf(tp), defaultFormat);
    }
    return formatDateTimeTokens(base, msecOf(tp), format);
}

string DateTime::toHttpDate() const
{
    if (!valid) {
        return string();
    }
    const time_t secs = static_cast<time_t>(toSecsSinceEpoch());
    tm utc {};
    gmtimeR(&secs, &utc);
    // RFC 1123 dates must always use English weekday/month abbreviations regardless
    // of the process locale (strftime %a/%b follow setlocale).
    static const char *const kWeekdayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buffer[128];
    const int len = snprintf(buffer, sizeof(buffer), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                             kWeekdayNames[utc.tm_wday], utc.tm_mday, kMonthNames[utc.tm_mon],
                             utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec);
    if (len <= 0 || len >= static_cast<int>(sizeof(buffer))) {
        return string();
    }
    return string(buffer, static_cast<size_t>(len));
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
QTNG_NAMESPACE_END