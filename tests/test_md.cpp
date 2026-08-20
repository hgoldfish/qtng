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

TEST_CASE("MessageDigest SHA-224 known vectors", "[md][sha224]")
{
    REQUIRE(digestHex(MessageDigest::Sha224, "")
            == "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f");
    REQUIRE(digestHex(MessageDigest::Sha224, "abc")
            == "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");
    REQUIRE(digestHex(MessageDigest::Sha224, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
            == "75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525");
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
         {MessageDigest::Md5, MessageDigest::Sha1, MessageDigest::Sha224, MessageDigest::Sha256}) {
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
             {MessageDigest::Md5, MessageDigest::Sha1, MessageDigest::Sha224, MessageDigest::Sha256}) {
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
    REQUIRE(MessageDigest::digest("abc", MessageDigest::Sha3_256).empty());
    REQUIRE(MessageDigest::digest("abc", MessageDigest::Blake2s_256).empty());
    REQUIRE(MessageDigest::digest("abc", MessageDigest::Ripemd160).empty());
    REQUIRE(PBKDF2_HMAC(16, "password", "salt", MessageDigest::Sha256, 10).empty());
}
#else
TEST_CASE("MessageDigest SHA-384 known vectors", "[md][sha384]")
{
    REQUIRE(digestHex(MessageDigest::Sha384, "")
            == "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da"
               "274edebfe76f65fbd51ad2f14898b95b");
    REQUIRE(digestHex(MessageDigest::Sha384, "abc")
            == "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
               "8086072ba1e7cc2358baeca134c825a7");
}

TEST_CASE("MessageDigest SHA-512 known vectors", "[md][sha512]")
{
    REQUIRE(digestHex(MessageDigest::Sha512, "")
            == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
               "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    REQUIRE(digestHex(MessageDigest::Sha512, "abc")
            == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
               "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

TEST_CASE("MessageDigest SHA-3-256 known vectors", "[md][sha3]")
{
    const string empty = digestHex(MessageDigest::Sha3_256, "");
    if (empty.empty()) {
        SKIP("SHA-3 unavailable in this crypto backend");
    }
    REQUIRE(empty == "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");
    REQUIRE(digestHex(MessageDigest::Sha3_256, "abc")
            == "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");
    REQUIRE(digestHex(MessageDigest::Sha3_224, "abc")
            == "e642824c3f8cf24ad09234ee7d3c766fc9a3a5168d0c94ad73b46fdf");
    REQUIRE(digestHex(MessageDigest::Sha3_384, "abc")
            == "ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c2596da7cf0e49be4b2"
               "98d88cea927ac7f539f1edf228376d25");
    REQUIRE(digestHex(MessageDigest::Sha3_512, "abc")
            == "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
               "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0");
}

TEST_CASE("MessageDigest SHA-512/224 and SHA-512/256 known vectors", "[md][sha512t]")
{
    const string sha512_256 = digestHex(MessageDigest::Sha512_256, "abc");
    if (sha512_256.empty()) {
        SKIP("SHA-512/224 and SHA-512/256 unavailable in this crypto backend");
    }
    REQUIRE(digestHex(MessageDigest::Sha512_224, "abc")
            == "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa");
    REQUIRE(sha512_256 == "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23");
}

TEST_CASE("MessageDigest BLAKE2 known vectors", "[md][blake2]")
{
    const string s = digestHex(MessageDigest::Blake2s_256, "abc");
    if (s.empty()) {
        SKIP("BLAKE2 unavailable in this crypto backend");
    }
    REQUIRE(s == "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982");
    REQUIRE(digestHex(MessageDigest::Blake2b_512, "abc")
            == "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
               "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
}

TEST_CASE("MessageDigest SM3 known vectors", "[md][sm3]")
{
    const string abc = digestHex(MessageDigest::Sm3, "abc");
    if (abc.empty()) {
        SKIP("SM3 unavailable in this crypto backend");
    }
    REQUIRE(digestHex(MessageDigest::Sm3, "")
            == "1ab21d8355cfa17f8e61194831e81a8f22bec8c728fefb747ed035eb5082aa2b");
    REQUIRE(abc == "66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0");
}

TEST_CASE("MessageDigest RIPEMD-160 known vectors", "[md][ripemd160]")
{
    const string abc = digestHex(MessageDigest::Ripemd160, "abc");
    if (abc.empty()) {
        SKIP("RIPEMD-160 unavailable in this crypto backend");
    }
    REQUIRE(digestHex(MessageDigest::Ripemd160, "") == "9c1185a5c5e9fc54612808977ee8f548b2258d31");
    REQUIRE(abc == "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");
    // Bitcoin HASH160 = RIPEMD160(SHA256(data))
    const string sha256abc = MessageDigest::digest("abc", MessageDigest::Sha256);
    REQUIRE(digestHex(MessageDigest::Ripemd160, sha256abc)
            == "bb1be98c142444d7a56aa3981c3942a978e4dc33");
}

TEST_CASE("MessageDigest OpenSSL backend streaming SHA-512", "[md][streaming]")
{
    const string data(1000, 'x');
    const string once = digestHex(MessageDigest::Sha512, data);
    REQUIRE_FALSE(once.empty());
    REQUIRE(digestHexChunked(MessageDigest::Sha512, data, 63) == once);
    REQUIRE(digestHexChunked(MessageDigest::Sha512, data, 128) == once);
}
#endif
