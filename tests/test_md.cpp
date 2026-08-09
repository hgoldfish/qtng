#include <catch2/catch_test_macros.hpp>

#include <string>

#include "qtng/md.h"

using namespace std;
using namespace qtng;

namespace {

string toHex(const string &bytes)
{
    static const char *digits = "0123456789abcdef";
    string out;
    out.resize(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        out[i * 2] = digits[c >> 4];
        out[i * 2 + 1] = digits[c & 0xf];
    }
    return out;
}

string digestHex(MessageDigest::Algorithm algo, const string &data)
{
    return toHex(MessageDigest::digest(data, algo));
}

string digestHexChunked(MessageDigest::Algorithm algo, const string &data, size_t chunk)
{
    if (chunk == 0) {
        return digestHex(algo, data);
    }
    MessageDigest md(algo);
    for (size_t off = 0; off < data.size();) {
        size_t n = chunk;
        if (off + n > data.size()) {
            n = data.size() - off;
        }
        md.addData(data.data() + off, static_cast<int>(n));
        off += n;
    }
    return toHex(md.result());
}

}  // namespace

TEST_CASE("MessageDigest MD5 known vectors", "[md][md5]")
{
    REQUIRE(digestHex(MessageDigest::Md5, "") == "d41d8cd98f00b204e9800998ecf8427e");
    REQUIRE(digestHex(MessageDigest::Md5, "a") == "0cc175b9c0f1b6a831c399e269772661");
    REQUIRE(digestHex(MessageDigest::Md5, "abc") == "900150983cd24fb0d6963f7d28e17f72");
    REQUIRE(digestHex(MessageDigest::Md5, "message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    REQUIRE(digestHex(MessageDigest::Md5, "abcdefghijklmnopqrstuvwxyz")
            == "c3fcd3d76192e4007dfb496cca67e13b");
}

TEST_CASE("MessageDigest SHA-1 known vectors", "[md][sha1]")
{
    REQUIRE(digestHex(MessageDigest::Sha1, "") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    REQUIRE(digestHex(MessageDigest::Sha1, "abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    REQUIRE(digestHex(MessageDigest::Sha1, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
            == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST_CASE("MessageDigest SHA-256 known vectors", "[md][sha256]")
{
    REQUIRE(digestHex(MessageDigest::Sha256, "")
            == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(digestHex(MessageDigest::Sha256, "abc")
            == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(digestHex(MessageDigest::Sha256, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
            == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("MessageDigest streaming matches one-shot", "[md][streaming]")
{
    const string data(1000, 'x');
    for (MessageDigest::Algorithm algo :
         {MessageDigest::Md5, MessageDigest::Sha1, MessageDigest::Sha256}) {
        const string once = digestHex(algo, data);
        REQUIRE(digestHexChunked(algo, data, 1) == once);
        REQUIRE(digestHexChunked(algo, data, 17) == once);
        REQUIRE(digestHexChunked(algo, data, 63) == once);
        REQUIRE(digestHexChunked(algo, data, 64) == once);
        REQUIRE(digestHexChunked(algo, data, 65) == once);
        REQUIRE(digestHexChunked(algo, data, 128) == once);
    }
}

TEST_CASE("MessageDigest padding boundary lengths", "[md][padding]")
{
    // 55/56/63/64/65 hit MD-padding edge cases around the 56-byte length field.
    for (size_t len : {size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(119), size_t(128)}) {
        const string data(len, 'A');
        for (MessageDigest::Algorithm algo :
             {MessageDigest::Md5, MessageDigest::Sha1, MessageDigest::Sha256}) {
            REQUIRE(digestHexChunked(algo, data, 7) == digestHex(algo, data));
        }
    }
}

TEST_CASE("MessageDigest result is idempotent", "[md][contract]")
{
    MessageDigest md(MessageDigest::Sha1);
    md.addData("abc");
    const string first = md.result();
    REQUIRE(toHex(first) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    REQUIRE(md.result() == first);

    // Extra updates after finalize must not change the cached digest.
    md.addData("x");
    REQUIRE(md.result() == first);
}

#ifdef QTNG_NO_CRYPTO
TEST_CASE("MessageDigest software backend rejects unsupported algorithms", "[md]")
{
    MessageDigest md(MessageDigest::Sha512);
    md.addData("abc");
    REQUIRE(md.result().empty());
    REQUIRE(MessageDigest::digest("abc", MessageDigest::Sha384).empty());
    REQUIRE(PBKDF2_HMAC(16, "password", "salt", MessageDigest::Sha256, 10).empty());
}
#endif
