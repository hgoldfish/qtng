#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "qtng/utils/platform.h"

using namespace std;

TEST_CASE("ngFromBigEndian and ngToBigEndian", "[platform]")
{
    SECTION("uint16")
    {
        const uint8_t bytes[] = {0x12, 0x34};
        REQUIRE(ngFromBigEndian<uint16_t>(bytes) == 0x1234);

        uint16_t value = 0xabcd;
        uint16_t encoded = ngToBigEndian(value);
        uint8_t out[2];
        ngToBigEndian(value, out);
        REQUIRE(out[0] == 0xab);
        REQUIRE(out[1] == 0xcd);
        REQUIRE(ngFromBigEndian<uint16_t>(out) == value);
        (void)encoded;
    }

    SECTION("uint32")
    {
        const uint8_t bytes[] = {0x00, 0x00, 0x01, 0x02};
        REQUIRE(ngFromBigEndian<uint32_t>(bytes) == 0x102);

        uint32_t value = 0xdeadbeef;
        uint8_t out[4];
        ngToBigEndian(value, out);
        REQUIRE(out[0] == 0xde);
        REQUIRE(out[1] == 0xad);
        REQUIRE(out[2] == 0xbe);
        REQUIRE(out[3] == 0xef);
        REQUIRE(ngFromBigEndian<uint32_t>(out) == value);
    }

    SECTION("uint64")
    {
        uint64_t value = 0x0102030405060708ULL;
        uint8_t out[8];
        ngToBigEndian(value, out);
        REQUIRE(ngFromBigEndian<uint64_t>(out) == value);
    }
}

TEST_CASE("std integer types have expected sizes", "[platform]")
{
    REQUIRE(sizeof(int8_t) == 1);
    REQUIRE(sizeof(uint8_t) == 1);
    REQUIRE(sizeof(int16_t) == 2);
    REQUIRE(sizeof(uint16_t) == 2);
    REQUIRE(sizeof(int32_t) == 4);
    REQUIRE(sizeof(uint32_t) == 4);
    REQUIRE(sizeof(int64_t) == 8);
    REQUIRE(sizeof(uint64_t) == 8);
}

enum TestFlags { FlagA = 1, FlagB = 2, FlagC = 4 };
NG_DECLARE_OPERATORS_FOR_FLAGS(TestFlags)

TEST_CASE("NG_DECLARE_OPERATORS_FOR_FLAGS", "[platform]")
{
    TestFlags combined = FlagA | FlagB;
    REQUIRE(int(combined) == 3);
    combined &= ~FlagA;
    REQUIRE(int(combined) == 2);
    combined ^= FlagC;
    REQUIRE(int(combined) == 6);
    REQUIRE(int(~FlagA) != 0);
}

TEST_CASE("endian round-trip all integer widths", "[platform]")
{
    SECTION("int8 and uint8")
    {
        const int8_t src = -1;
        uint8_t out = 0;
        ngToBigEndian(src, &out);
        REQUIRE(ngFromBigEndian<int8_t>(&out) == src);
    }

    SECTION("zero values")
    {
        uint32_t zero = 0;
        uint8_t out[4] = {0xff, 0xff, 0xff, 0xff};
        ngToBigEndian(zero, out);
        REQUIRE(out[0] == 0);
        REQUIRE(out[1] == 0);
        REQUIRE(out[2] == 0);
        REQUIRE(out[3] == 0);
        REQUIRE(ngFromBigEndian<uint32_t>(out) == 0);
    }

    SECTION("ngToBigEndian writes big-endian bytes")
    {
        uint16_t value = 0x00ff;
        uint8_t out[2] = {};
        ngToBigEndian(value, out);
        REQUIRE(out[0] == 0x00);
        REQUIRE(out[1] == 0xff);
    }
}
