#ifndef QTNG_UTILS_DATETIME_H
#define QTNG_UTILS_DATETIME_H

#include <chrono>
#include <cstdint>
#include <string>

namespace qtng {
namespace utils {

class DateTime;

// Day-resolution calendar type with semantics aligned with QDate: internally stores a Julian Day Number,
// with an invalid date represented by jd == 0. All operations are pure calendar arithmetic, timezone-free.
class Date
{
public:
    Date();
    Date(int year, int month, int day);

    static Date fromJulianDay(std::int64_t jd);
    static Date fromString(const std::string &str, const std::string &format = "yyyy-MM-dd");
    static Date currentDate();  // local calendar day

    bool isValid() const;
    int year() const;
    int month() const;
    int day() const;
    int dayOfWeek() const;  // Monday == 1 ... Sunday == 7

    // The following three methods mirror QDate.
    static bool isLeapYear(int year);                 // mirrors QDate::isLeapYear(int)
    static int daysInMonth(int year, int month);      // mirrors QDate::daysInMonth(int, int); returns 0 for invalid arguments
    int daysInMonth() const;                          // mirrors QDate::daysInMonth(); returns 0 for an invalid date
    // Returns the ISO-8601 week number (1..53); if yearNumber is non-null, stores the ISO year of that week
    // (which may differ from the calendar year at the start/end of the year). Mirrors QDate::weekNumber(int *yearNumber).
    int weekNumber(int *yearNumber = nullptr) const;

    std::int64_t toJulianDay() const;
    std::string toString(const std::string &format = "yyyy-MM-dd") const;
    Date addDays(int days) const;
    std::int64_t daysTo(const Date &other) const;

    bool operator==(const Date &other) const { return jd == other.jd; }
    bool operator!=(const Date &other) const { return jd != other.jd; }
    bool operator<(const Date &other) const { return jd < other.jd; }
    bool operator<=(const Date &other) const { return jd <= other.jd; }
    bool operator>(const Date &other) const { return jd > other.jd; }
    bool operator>=(const Date &other) const { return jd >= other.jd; }

private:
    std::int64_t jd;  // Julian Day Number; 0 means invalid.
};

class DateTime
{
public:
    DateTime();
    explicit DateTime(std::chrono::system_clock::time_point tp);
    // Interpret date as a local calendar day and combine it into the corresponding UTC instant.
    DateTime(const Date &localDate, int hour = 0, int minute = 0, int second = 0, int msec = 0);

    static DateTime currentDateTimeUtc();
    static DateTime currentDateTime();  // current instant (same instant as currentDateTimeUtc)
    static std::int64_t currentMSecsSinceEpoch();
    static DateTime fromSecsSinceEpoch(std::int64_t secs);
    static DateTime fromMSecsSinceEpoch(std::int64_t msecs);
    static DateTime fromUtc(int year, int month, int day, int hour = 0, int minute = 0, int second = 0,
                            int msec = 0);
    // Parse an ISO-8601 / "yyyy-MM-dd" string. Without a timezone marker, treat it as UTC.
    static DateTime fromString(const std::string &str);

    std::int64_t toSecsSinceEpoch() const;
    DateTime addSecs(std::int64_t secs) const;
    DateTime addDays(int days) const;  // add/subtract local calendar days, preserving the time of day
    std::int64_t secsTo(const DateTime &other) const;
    DateTime toUTC() const;
    DateTime toLocalTime() const;
    Date date() const;         // returns the local date when the local flag is set, otherwise the UTC date
    Date localDate() const;    // always returns the local date
    std::int64_t toMSecsSinceEpoch() const;
    std::string toString(const std::string &format = std::string()) const;
    std::string toHttpDate() const;

    bool isValid() const { return valid; }
    bool isLocalTime() const { return local; }
    std::chrono::system_clock::time_point timePoint() const { return tp; }

    bool operator==(const DateTime &other) const { return valid == other.valid && tp == other.tp; }
    bool operator!=(const DateTime &other) const { return !(*this == other); }
    bool operator<(const DateTime &other) const { return tp < other.tp; }

private:
    std::chrono::system_clock::time_point tp;
    bool valid;
    bool local;  // whether toString/date are interpreted in the local timezone
};

class ElapsedTimer
{
public:
    ElapsedTimer();
    void restart();
    std::int64_t elapsed() const;
    std::int64_t elapsedMicroseconds() const;
private:
    std::chrono::steady_clock::time_point start;
};

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_DATETIME_H
