#include <catch2/catch_test_macros.hpp>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtng/bencode.h"
#include "qtng/io_utils.h"

using namespace std;
using namespace qtng;

namespace {

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

TEST_CASE("bencode integer round-trip", "[bencode]")
{
    BencodeStream stream;
    stream << static_cast<int64_t>(42);
    REQUIRE(stream.data() == "i42e");

    BencodeStream parsed("i42e");
    int64_t value = 0;
    parsed >> value;
    REQUIRE(parsed.isOk());
    REQUIRE(value == 42);

    BencodeStream neg;
    neg << static_cast<int64_t>(-3);
    REQUIRE(neg.data() == "i-3e");

    BencodeStream zero;
    zero << static_cast<int64_t>(0);
    REQUIRE(zero.data() == "i0e");
}

TEST_CASE("bencode string round-trip", "[bencode]")
{
    BencodeStream stream;
    stream << string("spam");
    REQUIRE(stream.data() == "4:spam");

    BencodeStream parsed("4:spam");
    string value;
    parsed >> value;
    REQUIRE(parsed.isOk());
    REQUIRE(value == "spam");

    string binary("\x00\x01\xff", 3);
    BencodeStream bin;
    bin << binary;
    REQUIRE(bin.data() == string("3:\x00\x01\xff", 5));

    BencodeStream back(bin.data());
    string decoded;
    back >> decoded;
    REQUIRE(back.isOk());
    REQUIRE(decoded == binary);
}

TEST_CASE("bencode list and dict", "[bencode]")
{
    BencodeStream listStream;
    listStream.writeArrayHeader(2);
    listStream << string("spam") << static_cast<int64_t>(42);
    listStream.writeArrayEnd();
    REQUIRE(listStream.data() == "l4:spami42ee");

    map<string, Bencode> dict;
    dict["bar"] = "spam";
    dict["foo"] = Bencode(static_cast<int64_t>(42));
    Bencode dictValue(std::move(dict));
    REQUIRE(dictValue.encode() == "d3:bar4:spam3:fooi42ee");

    Bencode parsed = Bencode::decode(dictValue.encode());
    REQUIRE(parsed.isDict());
    REQUIRE(parsed.toMap().at("foo").toInteger() == 42);
    REQUIRE(parsed.toMap().at("bar").toString() == "spam");
}

TEST_CASE("bencode vector and map templates", "[bencode]")
{
    vector<int64_t> values{1, 2, 3};
    BencodeStream stream;
    stream << values;
    REQUIRE(stream.data() == "li1ei2ei3ee");

    vector<int64_t> back;
    BencodeStream parsed(stream.data());
    parsed >> back;
    REQUIRE(parsed.isOk());
    REQUIRE(back == values);

    map<string, string> dict{{"foo", "bar"}, {"spam", "eggs"}};
    BencodeStream dictStream;
    dictStream << dict;
    REQUIRE(dictStream.data() == "d3:foo3:bar4:spam4:eggse");

    map<string, string> decoded;
    BencodeStream dictParsed(dictStream.data());
    dictParsed >> decoded;
    REQUIRE(dictParsed.isOk());
    REQUIRE(decoded == dict);
}

TEST_CASE("bencode list set unordered containers", "[bencode]")
{
    list<int64_t> linked{1, 2, 3};
    BencodeStream listStream;
    listStream << linked;
    REQUIRE(listStream.data() == "li1ei2ei3ee");

    list<int64_t> linkedBack;
    BencodeStream listParsed(listStream.data());
    listParsed >> linkedBack;
    REQUIRE(listParsed.isOk());
    REQUIRE(linkedBack == linked);

    set<int64_t> ordered{3, 1, 2};
    BencodeStream setStream;
    setStream << ordered;
    REQUIRE(setStream.data() == "li1ei2ei3ee");

    set<int64_t> orderedBack;
    BencodeStream setParsed(setStream.data());
    setParsed >> orderedBack;
    REQUIRE(setParsed.isOk());
    REQUIRE(orderedBack == ordered);

    unordered_set<string> uniq{"a", "b"};
    BencodeStream usetStream;
    usetStream << uniq;
    REQUIRE(usetStream.isOk());

    unordered_set<string> uniqBack;
    BencodeStream usetParsed(usetStream.data());
    usetParsed >> uniqBack;
    REQUIRE(usetParsed.isOk());
    REQUIRE(uniqBack == uniq);

    unordered_map<string, int64_t> umap{{"spam", 42}};
    BencodeStream umapStream;
    umapStream << umap;
    REQUIRE(umapStream.data() == "d4:spami42ee");

    unordered_map<string, int64_t> umapBack;
    BencodeStream umapParsed(umapStream.data());
    umapParsed >> umapBack;
    REQUIRE(umapParsed.isOk());
    REQUIRE(umapBack == umap);
}

TEST_CASE("bencode DHT ping query shape", "[bencode]")
{
    map<string, Bencode> a;
    a["id"] = string(20, '\x01');
    map<string, Bencode> msg;
    msg["a"] = Bencode(std::move(a));
    msg["q"] = "ping";
    msg["t"] = "aa";
    msg["y"] = "q";

    string encoded = Bencode(std::move(msg)).encode();
    Bencode back = Bencode::decode(encoded);
    REQUIRE(back.toMap().at("q").toString() == "ping");
    REQUIRE(back.toMap().at("a").toMap().at("id").toString().size() == 20);
}

TEST_CASE("bencode rejects corrupt input", "[bencode]")
{
    string err;
    REQUIRE_FALSE(Bencode::decode("i01e", &err).isValid());
    REQUIRE_FALSE(Bencode::decode("3:ab", &err).isValid());
    REQUIRE_FALSE(Bencode::decode("l4:spam", &err).isValid());
    REQUIRE_FALSE(Bencode::decode("", &err).isValid());
}

TEST_CASE("bencode dynamic value via stream", "[bencode]")
{
    map<string, Bencode> value;
    value["spam"] = Bencode(static_cast<int64_t>(7));

    BencodeStream stream;
    stream << Bencode(std::move(value));
    REQUIRE(stream.data() == "d4:spami7ee");

    Bencode decoded;
    BencodeStream parsed(stream.data());
    parsed >> decoded;
    REQUIRE(parsed.isOk());
    REQUIRE(decoded.toMap().at("spam").toInteger() == 7);
}

TEST_CASE("bencode toList and toMap wrong type", "[bencode]")
{
    Bencode integer(static_cast<int64_t>(1));
    REQUIRE(integer.toList().empty());
    REQUIRE(integer.toMap().empty());

    vector<Bencode> list;
    list.push_back(Bencode("a"));
    Bencode listValue(list);
    REQUIRE(listValue.isList());
    REQUIRE(listValue.toList().size() == 1);
    REQUIRE(listValue.toMap().empty());
}

TEST_CASE("bencode peekByte caches one byte from arbitrary FileLike", "[bencode]")
{
    OneByteFileLike file("i42e");
    BencodeStream stream(&file);
    uint8_t b = 0;
    REQUIRE(stream.peekByte(&b));
    REQUIRE(b == static_cast<uint8_t>('i'));
    REQUIRE(file.readCalls == 1);
    REQUIRE(stream.peekByte(&b));
    REQUIRE(file.readCalls == 1);

    int64_t value = 0;
    stream >> value;
    REQUIRE(stream.isOk());
    REQUIRE(value == 42);
    REQUIRE(file.readCalls == 4);
}
