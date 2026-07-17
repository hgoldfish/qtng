#include <catch2/catch_test_macros.hpp>
#include <string>
#include <variant>
#include <vector>

#include "qtng/msgpack.h"

using namespace std;

using namespace qtng;

namespace {

template<typename V>
void roundTrip(const V &input, V &output)
{
    string buf;
    {
        MsgPackStream ds(&buf, true);
        ds << input;
        REQUIRE(ds.isOk());
    }
    {
        MsgPackStream ds(buf);
        ds >> output;
    }
}

}  // namespace

TEST_CASE("variant<int, string> round-trips int", "[msgpack][variant]")
{
    variant<int, string> v = 42;
    variant<int, string> out;
    roundTrip(v, out);
    REQUIRE(out.index() == 0);
    REQUIRE(get<int>(out) == 42);
}

TEST_CASE("variant<int, string> round-trips string", "[msgpack][variant]")
{
    variant<int, string> v = string("hello");
    variant<int, string> out;
    roundTrip(v, out);
    REQUIRE(out.index() == 1);
    REQUIRE(get<string>(out) == "hello");
}

TEST_CASE("variant<int, string> default-constructed holds int zero", "[msgpack][variant]")
{
    variant<int, string> v;
    variant<int, string> out;
    roundTrip(v, out);
    REQUIRE(out.index() == 0);
    REQUIRE(get<int>(out) == 0);
}

TEST_CASE("variant<monostate, int, string> round-trips monostate", "[msgpack][variant]")
{
    variant<monostate, int, string> v;
    variant<monostate, int, string> out;
    out = 7;
    roundTrip(v, out);
    REQUIRE(out.index() == 0);
    REQUIRE(holds_alternative<monostate>(out));
}

TEST_CASE("variant<monostate, int, string> round-trips string at index 2", "[msgpack][variant]")
{
    variant<monostate, int, string> v = string("gamma");
    variant<monostate, int, string> out;
    roundTrip(v, out);
    REQUIRE(out.index() == 2);
    REQUIRE(get<string>(out) == "gamma");
}

TEST_CASE("variant<double, vector<int>> round-trips nested container", "[msgpack][variant]")
{
    variant<double, vector<int>> v = vector<int>{1, 2, 3};
    variant<double, vector<int>> out;
    out = 1.5;
    roundTrip(v, out);
    REQUIRE(out.index() == 1);
    REQUIRE(get<vector<int>>(out) == vector<int>{1, 2, 3});
}

TEST_CASE("variant<uint64_t, string> round-trips large integer", "[msgpack][variant]")
{
    variant<uint64_t, string> v = static_cast<uint64_t>(0x123456789abcdefULL);
    variant<uint64_t, string> out;
    roundTrip(v, out);
    REQUIRE(out.index() == 0);
    REQUIRE(get<uint64_t>(out) == 0x123456789abcdefULL);
}

TEST_CASE("variant rejects array header of wrong length", "[msgpack][variant]")
{
    // fixarray(3) without valid payload; readArrayHeader succeeds, len != 2 fails fast.
    const string buf = "\x93";
    MsgPackStream ds(buf);
    variant<int, string> out;
    ds >> out;
    REQUIRE(ds.status() == MsgPackStream::ReadCorruptData);
}

TEST_CASE("variant rejects out-of-range index", "[msgpack][variant]")
{
    // fixarray(2), uint8 5 (index), value placeholder 0.
    const string buf = "\x92\xcc\x05\x00";
    MsgPackStream ds(buf);
    variant<int, string> out;
    ds >> out;
    REQUIRE(ds.status() == MsgPackStream::ReadCorruptData);
}

TEST_CASE("variant rejects truncated stream", "[msgpack][variant]")
{
    const string buf;
    MsgPackStream ds(buf);
    variant<int, string> out;
    ds >> out;
    REQUIRE(ds.status() == MsgPackStream::ReadPastEnd);
}

TEST_CASE("variant rejects non-array leading byte", "[msgpack][variant]")
{
    // nil where an array header is expected.
    const string buf = "\xc0";
    MsgPackStream ds(buf);
    variant<int, string> out;
    ds >> out;
    REQUIRE(ds.status() == MsgPackStream::ReadCorruptData);
}

TEST_CASE("monostate serializes as nil byte", "[msgpack][variant]")
{
    string buf;
    {
        MsgPackStream ds(&buf, true);
        ds << monostate{};
        REQUIRE(ds.isOk());
    }
    REQUIRE(buf.size() == 1);
    REQUIRE(static_cast<uint8_t>(buf[0]) == FirstByte::NIL);
    {
        MsgPackStream ds(buf);
        monostate m;
        ds >> m;
        REQUIRE(ds.isOk());
    }
}
