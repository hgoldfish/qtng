#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "qtng/deferred.h"

using namespace std;
using namespace qtng;

TEST_CASE("Deferred::remove skips unfired callback", "[deferred]")
{
    Deferred<int> df;
    vector<int> seen;

    df.addCallback([&](const int &v) { seen.push_back(v); });
    int id = df.addCallback([&](const int &v) { seen.push_back(v * 10); });
    df.addCallback([&](const int &v) { seen.push_back(v * 100); });

    df.remove(id);
    df.remove(id);  // unknown / already removed: no-op
    df.callback(7);

    REQUIRE(seen == vector<int>{7, 700});
}

TEST_CASE("Deferred<void>::remove skips unfired callback", "[deferred]")
{
    Deferred<void> df;
    int count = 0;

    df.addCallback([&]() { ++count; });
    int id = df.addCallback([&]() { count += 10; });
    df.addCallback([&]() { count += 100; });

    df.remove(id);
    df.callback();

    REQUIRE(count == 101);
}

TEST_CASE("Deferred::callback survives remove of current id", "[deferred]")
{
    Deferred<int> df;
    vector<int> seen;

    int id1 = 0;
    id1 = df.addCallback([&](const int &v) {
        seen.push_back(v);
        df.remove(id1);
    });
    int id2 = 0;
    id2 = df.addCallback([&](const int &v) {
        seen.push_back(v * 10);
        df.remove(id2);
    });

    df.callback(7);

    REQUIRE(seen == vector<int>{7, 70});
}

TEST_CASE("Deferred<void>::callback survives clear during fire", "[deferred]")
{
    Deferred<void> df;
    int count = 0;

    df.addCallback([&]() {
        ++count;
        df.clear();
    });
    df.addCallback([&]() { ++count; });

    df.callback();

    REQUIRE(count == 2);
}
