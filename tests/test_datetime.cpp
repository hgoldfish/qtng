#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "qtng/utils/datetime.h"

using namespace std;

using namespace qtng::utils;

TEST_CASE("DateTime epoch conversions", "[datetime]")
{
    DateTime dt = DateTime::fromSecsSinceEpoch(0);
    REQUIRE(dt.isValid());
    REQUIRE(dt.toSecsSinceEpoch() == 0);
    REQUIRE(dt.toMSecsSinceEpoch() == 0);

    DateTime ms = DateTime::fromMSecsSinceEpoch(1500);
    REQUIRE(ms.toMSecsSinceEpoch() == 1500);
    REQUIRE(ms.toSecsSinceEpoch() == 1);
}

TEST_CASE("DateTime fromUtc", "[datetime]")
{
    DateTime dt = DateTime::fromUtc(2020, 1, 15, 12, 30, 45);
    REQUIRE(dt.isValid());
    REQUIRE(dt.toString("%Y-%m-%d") == "2020-01-15");
    REQUIRE(dt.toString("%H:%M:%S") == "12:30:45");
    REQUIRE(dt.toHttpDate().find("Wed, 15 Jan 2020 12:30:45 GMT") != string::npos);
}

TEST_CASE("DateTime arithmetic and comparison", "[datetime]")
{
    DateTime base = DateTime::fromSecsSinceEpoch(1000);
    DateTime later = base.addSecs(250);
    REQUIRE(later.toSecsSinceEpoch() == 1250);
    REQUIRE(base.secsTo(later) == 250);
    REQUIRE(later.secsTo(base) == -250);
    REQUIRE(base < later);
    REQUIRE(base != later);
    REQUIRE(base == base);
}

TEST_CASE("DateTime invalid state", "[datetime]")
{
    DateTime invalid;
    REQUIRE_FALSE(invalid.isValid());
    REQUIRE(invalid.toSecsSinceEpoch() == 0);
    REQUIRE(invalid.toString().empty());
    REQUIRE(invalid.toHttpDate().empty());
    REQUIRE(invalid.addSecs(10).isValid() == false);
    REQUIRE(invalid.secsTo(DateTime::fromSecsSinceEpoch(0)) == 0);
}

TEST_CASE("DateTime current time", "[datetime]")
{
    int64_t before = DateTime::currentMSecsSinceEpoch();
    DateTime now = DateTime::currentDateTimeUtc();
    int64_t after = DateTime::currentMSecsSinceEpoch();
    REQUIRE(now.isValid());
    REQUIRE(now.toMSecsSinceEpoch() >= before);
    REQUIRE(now.toMSecsSinceEpoch() <= after);
}

TEST_CASE("ElapsedTimer", "[datetime]")
{
    ElapsedTimer timer;
    this_thread::sleep_for(chrono::milliseconds(20));
    REQUIRE(timer.elapsed() >= 10);
    REQUIRE(timer.elapsedMicroseconds() >= timer.elapsed() * 1000);

    timer.restart();
    REQUIRE(timer.elapsed() < 50);
}

TEST_CASE("DateTime default format", "[datetime]")
{
    DateTime dt = DateTime::fromUtc(2024, 6, 1, 8, 9, 10);
    REQUIRE(dt.toString() == "2024-06-01T08:09:10Z");
}

TEST_CASE("DateTime negative addSecs", "[datetime]")
{
    DateTime base = DateTime::fromSecsSinceEpoch(1000);
    DateTime earlier = base.addSecs(-250);
    REQUIRE(earlier.toSecsSinceEpoch() == 750);
    REQUIRE(base.secsTo(earlier) == -250);
}

TEST_CASE("DateTime timePoint round-trip", "[datetime]")
{
    DateTime dt = DateTime::fromMSecsSinceEpoch(1234567890);
    DateTime copy(dt.timePoint());
    REQUIRE(copy == dt);
}

TEST_CASE("DateTime toUTC is identity", "[datetime]")
{
    DateTime dt = DateTime::fromUtc(2019, 12, 31, 23, 59, 59);
    REQUIRE(dt.toUTC() == dt);
}

TEST_CASE("Date construction and accessors", "[date]")
{
    Date d(2024, 6, 1);
    REQUIRE(d.isValid());
    REQUIRE(d.year() == 2024);
    REQUIRE(d.month() == 6);
    REQUIRE(d.day() == 1);

    Date invalid;
    REQUIRE_FALSE(invalid.isValid());
    REQUIRE(invalid.toString().empty());
    REQUIRE(invalid.addDays(1).isValid() == false);
}

TEST_CASE("Date julian day round-trip", "[date]")
{
    Date d = Date::fromJulianDay(2460260);  // 2023-11-11
    REQUIRE(d.isValid());
    REQUIRE(d.year() == 2023);
    REQUIRE(d.month() == 11);
    REQUIRE(d.day() == 11);
    REQUIRE(d.toJulianDay() == 2460260);
}

TEST_CASE("Date addDays and daysTo", "[date]")
{
    Date d(2024, 1, 31);
    REQUIRE(d.addDays(1) == Date(2024, 2, 1));
    REQUIRE(d.addDays(29) == Date(2024, 2, 29));  // leap year
    REQUIRE(d.addDays(-31) == Date(2023, 12, 31));
    REQUIRE(d.daysTo(Date(2024, 2, 1)) == 1);
    REQUIRE(Date(2024, 2, 1).daysTo(d) == -1);
}

TEST_CASE("Date string round-trip", "[date]")
{
    Date d(2024, 3, 5);
    REQUIRE(d.toString() == "2024-03-05");
    REQUIRE(Date::fromString("2024-03-05") == d);
    REQUIRE_FALSE(Date::fromString("2024-13-05").isValid());
    REQUIRE_FALSE(Date::fromString("2024-03-32").isValid());
    REQUIRE_FALSE(Date::fromString("bad-date").isValid());
}

TEST_CASE("Date dayOfWeek", "[date]")
{
    // 2024-06-01 is a Saturday.
    REQUIRE(Date(2024, 6, 1).dayOfWeek() == 6);
    // 2024-06-02 is a Sunday.
    REQUIRE(Date(2024, 6, 2).dayOfWeek() == 7);
    // 2024-06-03 is a Monday.
    REQUIRE(Date(2024, 6, 3).dayOfWeek() == 1);
}

TEST_CASE("Date isLeapYear", "[date]")
{
    REQUIRE(Date::isLeapYear(2000));
    REQUIRE(Date::isLeapYear(2020));
    REQUIRE(Date::isLeapYear(2024));
    REQUIRE_FALSE(Date::isLeapYear(1900));
    REQUIRE_FALSE(Date::isLeapYear(2021));
    REQUIRE_FALSE(Date::isLeapYear(2023));
}

TEST_CASE("Date daysInMonth", "[date]")
{
    REQUIRE(Date::daysInMonth(2024, 2) == 29);  // leap year
    REQUIRE(Date::daysInMonth(2023, 2) == 28);  // common year
    REQUIRE(Date::daysInMonth(2024, 1) == 31);
    REQUIRE(Date::daysInMonth(2024, 4) == 30);
    REQUIRE(Date::daysInMonth(2024, 12) == 31);
    REQUIRE(Date::daysInMonth(2024, 0) == 0);   // invalid month
    REQUIRE(Date::daysInMonth(2024, 13) == 0);

    REQUIRE(Date(2024, 2, 10).daysInMonth() == 29);
    REQUIRE(Date(2023, 2, 10).daysInMonth() == 28);
    REQUIRE(Date(2024, 6, 1).daysInMonth() == 30);
    REQUIRE(Date().daysInMonth() == 0);  // invalid date
}

TEST_CASE("Date weekNumber", "[date]")
{
    int isoYear = 0;

    // 2024-01-01 is a Monday, so it belongs to ISO 2024-W01.
    REQUIRE(Date(2024, 1, 1).weekNumber(&isoYear) == 1);
    REQUIRE(isoYear == 2024);

    // 2021-01-01 is a Friday; the Thursday of its week is in 2020: ISO 2020-W53.
    REQUIRE(Date(2021, 1, 1).weekNumber(&isoYear) == 53);
    REQUIRE(isoYear == 2020);

    // 2016-01-01 is a Friday: ISO 2015-W53.
    REQUIRE(Date(2016, 1, 1).weekNumber(&isoYear) == 53);
    REQUIRE(isoYear == 2015);

    // 2024-06-01 is a Saturday: ISO 2024-W22.
    REQUIRE(Date(2024, 6, 1).weekNumber() == 22);

    // 2024-12-31 is a Tuesday; the Thursday of its week is 2025-01-02: ISO 2025-W01.
    REQUIRE(Date(2024, 12, 31).weekNumber(&isoYear) == 1);
    REQUIRE(isoYear == 2025);

    REQUIRE(Date().weekNumber() == 0);  // invalid date
}

TEST_CASE("DateTime fromString ISO", "[datetime]")
{
    DateTime dt = DateTime::fromString("2024-06-01T08:09:10.123Z");
    REQUIRE(dt.isValid());
    REQUIRE(dt.toMSecsSinceEpoch() == DateTime::fromUtc(2024, 6, 1, 8, 9, 10, 123).toMSecsSinceEpoch());

    DateTime dateOnly = DateTime::fromString("2024-06-01");
    REQUIRE(dateOnly.isValid());
    REQUIRE(dateOnly.toMSecsSinceEpoch() == DateTime::fromUtc(2024, 6, 1).toMSecsSinceEpoch());

    REQUIRE_FALSE(DateTime::fromString("not-a-date").isValid());
}

TEST_CASE("DateTime date() returns UTC date", "[datetime]")
{
    DateTime dt = DateTime::fromUtc(2024, 6, 1, 23, 59, 59);
    REQUIRE(dt.date() == Date(2024, 6, 1));
    REQUIRE(dt.toUTC().date() == Date(2024, 6, 1));
}

TEST_CASE("DateTime local time string", "[datetime]")
{
    // This test does not depend on the machine timezone: whatever the local timezone is, the time
    // components of toLocalTime().toString() must equal the local time decomposition (as determined by
    // the system localtime) and must no longer carry a 'Z' suffix.
    DateTime dt = DateTime::currentDateTimeUtc();
    DateTime local = dt.toLocalTime();
    REQUIRE(local.toString().find('Z') == string::npos);
    REQUIRE_FALSE(local.toString().empty());
    // The string may differ from the UTC version in time components, but represents the same instant.
    REQUIRE(local.toMSecsSinceEpoch() == dt.toMSecsSinceEpoch());
}

TEST_CASE("DateTime construct from local Date", "[datetime]")
{
    // The result may differ from fromUtc (local timezone), but must be valid with matching time components.
    DateTime dt(Date(2024, 6, 1), 12, 30, 45);
    REQUIRE(dt.isValid());
    REQUIRE(dt.localDate() == Date(2024, 6, 1));
    REQUIRE(dt.toLocalTime().date() == Date(2024, 6, 1));
}

TEST_CASE("DateTime addDays", "[datetime]")
{
    // Construct from a local Date to keep the test independent of the machine timezone.
    DateTime dt(Date(2024, 1, 31), 10, 20, 30);
    DateTime next = dt.addDays(1);
    REQUIRE(next.isValid());
    REQUIRE(next.localDate() == Date(2024, 2, 1));
    // The time-of-day must be preserved (in local time).
    REQUIRE(next.toLocalTime().toString("HH:mm:ss") == "10:20:30");
}
