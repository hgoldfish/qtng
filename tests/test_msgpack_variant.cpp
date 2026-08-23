#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "qtng/io_utils.h"
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

class OneByteFileLike : public FileLike
{
public:
    explicit OneByteFileLike(string payload)
        : payload(std::move(payload))
    {
    }

    int32_t read(char *data, int32_t size) override
    {
        ++readCalls;
        if (pos >= payload.size()) {
            return 0;
        }
        (void) size;
        data[0] = payload[pos++];
        return 1;
    }

    int32_t write(const char *, int32_t) override { return -1; }
    void close() override { }
    int64_t size() override { return static_cast<int64_t>(payload.size()); }

    int readCalls = 0;

private:
    string payload;
    size_t pos = 0;
};

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

TEST_CASE("variant<int, string> int encodes as native msgpack integer", "[msgpack][variant]")
{
    variant<int, string> v = 42;
    string buf;
    MsgPackStream ds(&buf, true);
    ds << v;
    REQUIRE(ds.isOk());
    REQUIRE(buf == string("\x2a", 1));
}

TEST_CASE("variant<int, string> string encodes as native msgpack string", "[msgpack][variant]")
{
    variant<int, string> v = string("hi");
    string buf;
    MsgPackStream ds(&buf, true);
    ds << v;
    REQUIRE(ds.isOk());
    REQUIRE(buf == string("\xa2hi", 3));
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

TEST_CASE("variant rejects wire type with no matching alternative", "[msgpack][variant]")
{
    const string buf = "\xc0";
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

TEST_CASE("variant rejects unknown leading byte", "[msgpack][variant]")
{
    const string buf = "\xc1";
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

TEST_CASE("peekByte caches one byte from arbitrary FileLike", "[msgpack][variant]")
{
    OneByteFileLike file(string("\x2a", 1));
    MsgPackStream ds(&file);
    uint8_t b = 0;
    REQUIRE(ds.peekByte(&b));
    REQUIRE(b == 0x2a);
    REQUIRE(file.readCalls == 1);
    REQUIRE(ds.peekByte(&b));
    REQUIRE(b == 0x2a);
    REQUIRE(file.readCalls == 1);

    variant<int, string> out;
    ds >> out;
    REQUIRE(ds.isOk());
    REQUIRE(out.index() == 0);
    REQUIRE(get<int>(out) == 42);
    REQUIRE(file.readCalls == 1);
}

TEST_CASE("writeString encodes as msgpack str, readString round-trips", "[msgpack][string]")
{
    string buf;
    {
        MsgPackStream ds(&buf, true);
        ds.writeString("hello", 5);
        REQUIRE(ds.isOk());
    }
    REQUIRE(buf == string("\xa5hello", 6));
    {
        MsgPackStream ds(buf);
        string out;
        REQUIRE(ds.readString(out));
        REQUIRE(ds.isOk());
        REQUIRE(out == "hello");
    }
}

TEST_CASE("writeBytes encodes as msgpack bin, readBytes round-trips", "[msgpack][bin]")
{
    string buf;
    {
        MsgPackStream ds(&buf, true);
        ds.writeBytes(string("\xc4\x00\xff\x10", 4));
        REQUIRE(ds.isOk());
    }
    // BIN8 header (0xc4) followed by the 4-byte payload.
    REQUIRE(buf == string("\xc4\x04\xc4\x00\xff\x10", 6));
    {
        MsgPackStream ds(buf);
        string out;
        REQUIRE(ds.readBytes(out));
        REQUIRE(ds.isOk());
        REQUIRE(out == string("\xc4\x00\xff\x10", 4));
    }
}

TEST_CASE("writeString(const string&) encodes as msgpack str", "[msgpack][string]")
{
    string buf;
    {
        MsgPackStream ds(&buf, true);
        ds.writeString(string("hello"));
        REQUIRE(ds.isOk());
    }
    REQUIRE(buf == string("\xa5hello", 6));
    {
        MsgPackStream ds(buf);
        string out;
        REQUIRE(ds.readString(out));
        REQUIRE(ds.isOk());
        REQUIRE(out == "hello");
    }
}

TEST_CASE("readString rejects bin values", "[msgpack][string]")
{
    string buf;
    {
        MsgPackStream ds(&buf, true);
        ds.writeBytes(string("abc"));
        REQUIRE(ds.isOk());
    }
    MsgPackStream ds(buf);
    string out = "unchanged";
    REQUIRE_FALSE(ds.readString(out));
    REQUIRE(ds.status() == MsgPackStream::ReadCorruptData);
    REQUIRE(out == "unchanged");
}

TEST_CASE("readBytes rejects str values", "[msgpack][bin]")
{
    string buf;
    {
        MsgPackStream ds(&buf, true);
        ds.writeString("abc", 3);
        REQUIRE(ds.isOk());
    }
    MsgPackStream ds(buf);
    string out = "unchanged";
    REQUIRE_FALSE(ds.readBytes(out));
    REQUIRE(ds.status() == MsgPackStream::ReadCorruptData);
    REQUIRE(out == "unchanged");
}

TEST_CASE("str and bin round-trip verbatim, including invalid UTF-8", "[msgpack][bin]")
{
    const string payload("\xff\xfe\x00\xc4", 4);
    string buf;
    {
        MsgPackStream ds(&buf, true);
        ds << payload;              // str
        ds.writeBytes(payload);     // bin
        REQUIRE(ds.isOk());
    }
    MsgPackStream ds(buf);
    string asStr;
    string asBin;
    REQUIRE(ds.readString(asStr));
    REQUIRE(ds.readBytes(asBin));
    REQUIRE(ds.isOk());
    REQUIRE(asStr == payload);
    REQUIRE(asBin == payload);
}

TEST_CASE("readBytes rejects bin32 payload with length >= 2^31", "[msgpack][bin]")
{
    // BIN32 header (0xc6) claiming 0x80000000 bytes must fail cleanly. With the
    // default length limit of UINT32_MAX the old code let it through the
    // limit check and crashed in string::resize via the negative int wrap.
    string buf("\xc6\x80\x00\x00\x00", 5);
    MsgPackStream ds(buf);
    ds.setLengthLimit(numeric_limits<uint32_t>::max());
    string out = "unchanged";
    REQUIRE_FALSE(ds.readBytes(out));
    REQUIRE(ds.status() == MsgPackStream::ReadCorruptData);
    REQUIRE(out == "unchanged");
}
