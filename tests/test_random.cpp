#include <catch2/catch_test_macros.hpp>
#include <set>
#include <vector>

#include "qtng/utils/random.h"

using namespace std;

using namespace qtng::utils;

TEST_CASE("RandomGenerator bounded", "[random]")
{
    RandomGenerator &rng = RandomGenerator::global();
    for (int i = 0; i < 100; ++i) {
        uint32_t value = rng.bounded(10);
        REQUIRE(value < 10);
    }
    REQUIRE(rng.bounded(0) == 0);
    REQUIRE(rng.bounded(1) == 0);
}

TEST_CASE("RandomGenerator produces varied output", "[random]")
{
    RandomGenerator &rng = RandomGenerator::global();
    set<uint32_t> seen;
    for (int i = 0; i < 64; ++i) {
        seen.insert(rng.generate());
    }
    REQUIRE(seen.size() > 32);
}

TEST_CASE("RandomGenerator generate64", "[random]")
{
    RandomGenerator &rng = RandomGenerator::global();
    uint64_t a = rng.generate64();
    uint64_t b = rng.generate64();
    const bool hasNonZero = (a != 0) || (b != 0);
    REQUIRE(hasNonZero);
}

TEST_CASE("RandomGenerator generate buffer", "[random]")
{
    RandomGenerator &rng = RandomGenerator::global();
    char buffer[32] = {};
    rng.generate(buffer, 32);
    bool allZero = true;
    for (int i = 0; i < 32; ++i) {
        if (buffer[i] != 0) {
            allZero = false;
            break;
        }
    }
    REQUIRE_FALSE(allZero);
}

TEST_CASE("RandomGenerator generateHex", "[random]")
{
    RandomGenerator &rng = RandomGenerator::global();
    string hex = rng.generateHex(16);
    REQUIRE(hex.size() == 32);
    for (char ch : hex) {
        REQUIRE(((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')));
    }
}

TEST_CASE("RandomGenerator global is singleton", "[random]")
{
    REQUIRE(&RandomGenerator::global() == &RandomGenerator::global());
}

TEST_CASE("RandomGenerator bounded distribution", "[random]")
{
    RandomGenerator &rng = RandomGenerator::global();
    vector<int> counts(5, 0);
    for (int i = 0; i < 500; ++i) {
        ++counts[rng.bounded(5)];
    }
    for (int count : counts) {
        REQUIRE(count > 0);
    }
}

TEST_CASE("RandomGenerator generateHex sizes", "[random]")
{
    RandomGenerator &rng = RandomGenerator::global();
    REQUIRE(rng.generateHex(0).empty());
    REQUIRE(rng.generateHex(8).size() == 16);
}

TEST_CASE("RandomGenerator generate zero length", "[random]")
{
    RandomGenerator &rng = RandomGenerator::global();
    char buffer[4] = {1, 2, 3, 4};
    rng.generate(buffer, 0);
    REQUIRE(buffer[0] == 1);
}
