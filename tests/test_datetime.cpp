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
