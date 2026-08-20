// Software MessageDigest when OpenSSL/LibreSSL is unavailable (QTNG_NO_CRYPTO).
// Md5/Sha1/Sha224/Sha256. Sha1 is required by HTTP WebSocket, BitTorrent, and Kademlia.
// Streaming addData() must match the OpenSSL-backed MessageDigest contract.

#include "qtng/md.h"

#include <cstring>

using namespace std;

namespace qtng {

namespace {

uint32_t rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

uint32_t rotr32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

void storeBe32(unsigned char *out, uint32_t v)
{
    out[0] = static_cast<unsigned char>((v >> 24) & 0xff);
    out[1] = static_cast<unsigned char>((v >> 16) & 0xff);
    out[2] = static_cast<unsigned char>((v >> 8) & 0xff);
    out[3] = static_cast<unsigned char>(v & 0xff);
}

void storeBe64(unsigned char *out, uint64_t v)
{
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<unsigned char>(v & 0xff);
        v >>= 8;
    }
}

uint32_t loadBe32(const unsigned char *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
            | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void storeLe32(unsigned char *out, uint32_t v)
{
    out[0] = static_cast<unsigned char>(v & 0xff);
    out[1] = static_cast<unsigned char>((v >> 8) & 0xff);
    out[2] = static_cast<unsigned char>((v >> 16) & 0xff);
    out[3] = static_cast<unsigned char>((v >> 24) & 0xff);
}

uint32_t loadLe32(const unsigned char *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16)
            | (static_cast<uint32_t>(p[3]) << 24);
}

void md5Transform(uint32_t state[4], const unsigned char block[64])
{
    static const uint32_t k[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
            0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
            0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
            0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
            0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
            0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };
    static const int s[64] = {7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
                              14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
                              4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};

    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = loadLe32(block + i * 4);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        uint32_t g;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = static_cast<uint32_t>(i);
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = static_cast<uint32_t>((5 * i + 1) % 16);
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = static_cast<uint32_t>((3 * i + 5) % 16);
        } else {
            f = c ^ (b | (~d));
            g = static_cast<uint32_t>((7 * i) % 16);
        }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + k[i] + m[g], s[i]);
        a = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void sha1Transform(uint32_t state[5], const unsigned char block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = loadBe32(block + i * 4);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f;
        uint32_t k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        uint32_t t = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha256Transform(uint32_t state[8], const unsigned char block[64])
{
    static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = loadBe32(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

enum DigestKind { DigestMd5, DigestSha1, DigestSha224, DigestSha256 };

bool algoToKind(MessageDigest::Algorithm algo, DigestKind *kind)
{
    switch (algo) {
    case MessageDigest::Md5:
        *kind = DigestMd5;
        return true;
    case MessageDigest::Sha1:
        *kind = DigestSha1;
        return true;
    case MessageDigest::Sha224:
        *kind = DigestSha224;
        return true;
    case MessageDigest::Sha256:
        *kind = DigestSha256;
        return true;
    default:
        return false;
    }
}

void transformBlock(DigestKind kind, uint32_t *state, const unsigned char *block)
{
    if (kind == DigestMd5) {
        md5Transform(state, block);
    } else if (kind == DigestSha1) {
        sha1Transform(state, block);
    } else {
        sha256Transform(state, block);
    }
}

}  // namespace

class MessageDigestPrivate
{
public:
    explicit MessageDigestPrivate(MessageDigest::Algorithm algo);
    void addData(const char *buf, int len);
    string result();

    DigestKind kind;
    int digestBytes;
    uint32_t state[8];
    unsigned char block[64];
    size_t blockLen;
    uint64_t totalBytes;
    string finalData;
    bool hasError;
    bool finished;
};

MessageDigestPrivate::MessageDigestPrivate(MessageDigest::Algorithm algo)
    : kind(DigestSha256)
    , digestBytes(32)
    , blockLen(0)
    , totalBytes(0)
    , hasError(false)
    , finished(false)
{
    memset(state, 0, sizeof(state));
    memset(block, 0, sizeof(block));
    if (!algoToKind(algo, &kind)) {
        hasError = true;
        return;
    }
    if (kind == DigestMd5) {
        state[0] = 0x67452301u;
        state[1] = 0xefcdab89u;
        state[2] = 0x98badcfeu;
        state[3] = 0x10325476u;
        digestBytes = 16;
    } else if (kind == DigestSha1) {
        state[0] = 0x67452301u;
        state[1] = 0xefcdab89u;
        state[2] = 0x98badcfeu;
        state[3] = 0x10325476u;
        state[4] = 0xc3d2e1f0u;
        digestBytes = 20;
    } else if (kind == DigestSha224) {
        state[0] = 0xc1059ed8u;
        state[1] = 0x367cd507u;
        state[2] = 0x3070dd17u;
        state[3] = 0xf70e5939u;
        state[4] = 0xffc00b31u;
        state[5] = 0x68581511u;
        state[6] = 0x64f98fa7u;
        state[7] = 0xbefa4fa4u;
        digestBytes = 28;
    } else {
        state[0] = 0x6a09e667u;
        state[1] = 0xbb67ae85u;
        state[2] = 0x3c6ef372u;
        state[3] = 0xa54ff53au;
        state[4] = 0x510e527fu;
        state[5] = 0x9b05688cu;
        state[6] = 0x1f83d9abu;
        state[7] = 0x5be0cd19u;
        digestBytes = 32;
    }
}

void MessageDigestPrivate::addData(const char *buf, int len)
{
    // After result(), further updates are ignored; cached digest stays valid
    // (matches OpenSSL/LibreSSL MessageDigest in this tree).
    if (hasError || finished || len <= 0) {
        return;
    }
    const unsigned char *p = reinterpret_cast<const unsigned char *>(buf);
    size_t remaining = static_cast<size_t>(len);
    totalBytes += remaining;

    if (blockLen > 0) {
        size_t n = 64 - blockLen;
        if (n > remaining) {
            n = remaining;
        }
        memcpy(block + blockLen, p, n);
        blockLen += n;
        p += n;
        remaining -= n;
        if (blockLen == 64) {
            transformBlock(kind, state, block);
            blockLen = 0;
        }
    }

    while (remaining >= 64) {
        transformBlock(kind, state, p);
        p += 64;
        remaining -= 64;
    }

    if (remaining > 0) {
        memcpy(block, p, remaining);
        blockLen = remaining;
    }
}

string MessageDigestPrivate::result()
{
    if (hasError) {
        return string();
    }
    if (finished) {
        return finalData;
    }
    finished = true;

    // Finalize a copy so repeated result() stays idempotent; live state is unused after this.
    uint32_t st[8];
    memcpy(st, state, sizeof(st));
    unsigned char pad[64];
    memcpy(pad, block, blockLen);
    size_t padLen = blockLen;
    const uint64_t totalBits = totalBytes * 8;

    pad[padLen++] = 0x80;
    if (padLen > 56) {
        while (padLen < 64) {
            pad[padLen++] = 0;
        }
        transformBlock(kind, st, pad);
        padLen = 0;
    }
    while (padLen < 56) {
        pad[padLen++] = 0;
    }
    if (kind == DigestMd5) {
        storeLe32(pad + 56, static_cast<uint32_t>(totalBits));
        storeLe32(pad + 60, static_cast<uint32_t>(totalBits >> 32));
        transformBlock(kind, st, pad);
        finalData.resize(static_cast<size_t>(digestBytes));
        for (int i = 0; i < digestBytes / 4; ++i) {
            storeLe32(reinterpret_cast<unsigned char *>(&finalData[0]) + i * 4, st[i]);
        }
    } else {
        storeBe64(pad + 56, totalBits);
        transformBlock(kind, st, pad);
        finalData.resize(static_cast<size_t>(digestBytes));
        for (int i = 0; i < digestBytes / 4; ++i) {
            storeBe32(reinterpret_cast<unsigned char *>(&finalData[0]) + i * 4, st[i]);
        }
    }
    return finalData;
}

MessageDigest::MessageDigest(Algorithm algo)
    : d_ptr(new MessageDigestPrivate(algo))
{
}

MessageDigest::~MessageDigest()
{
    delete d_ptr;
}

void MessageDigest::addData(const char *data, int len)
{
    NG_D(MessageDigest);
    d->addData(data, len);
}

string MessageDigest::result()
{
    NG_D(MessageDigest);
    return d->result();
}

string PBKDF2_HMAC(int, const string &, const string &, const MessageDigest::Algorithm, int)
{
    return string();
}

}  // namespace qtng
