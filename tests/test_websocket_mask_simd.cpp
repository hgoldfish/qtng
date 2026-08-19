#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

using namespace std;

namespace {

inline void maskToBytes(uint32_t maskkey, uint8_t out[4])
{
    out[0] = static_cast<uint8_t>((maskkey >> 24) & 0xffu);
    out[1] = static_cast<uint8_t>((maskkey >> 16) & 0xffu);
    out[2] = static_cast<uint8_t>((maskkey >> 8) & 0xffu);
    out[3] = static_cast<uint8_t>(maskkey & 0xffu);
}

void applyMaskPlain(char *dst, int offset, int size, const char *src, int payloadSize, uint32_t maskkey)
{
    uint8_t maskbuf[4];
    maskToBytes(maskkey, maskbuf);
    int last = offset + min(size, payloadSize);
    for (int i = offset, j = 0; i < last; ++i, ++j) {
        dst[i] = src[j] ^ static_cast<char>(maskbuf[j % 4]);
    }
}

void applyMaskSimd(char *dst, int offset, int size, const char *src, int payloadSize, uint32_t maskkey)
{
    int i = offset;
    int j = 0;
    int last = offset + min(size, payloadSize);
    int nextOffset16 = (offset + 15) / 16 * 16;
    uint8_t maskbuf[4];
    maskToBytes(maskkey, maskbuf);

    for (; i < nextOffset16 && i < last; ++i, ++j) {
        dst[i] = src[j] ^ static_cast<char>(maskbuf[j % 4]);
    }

    {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        int last128 = last & ~15;
        uint8_t mask16[16];
        for (int k = 0; k < 16; ++k) {
            mask16[k] = maskbuf[(j + k) % 4];
        }
        uint8x16_t mask128 = vld1q_u8(mask16);
        for (; i < last128; i += 16, j += 16) {
            uint8x16_t in128 = vld1q_u8(reinterpret_cast<const uint8_t *>(src + j));
            uint8x16_t out128 = veorq_u8(in128, mask128);
            vst1q_u8(reinterpret_cast<uint8_t *>(dst + i), out128);
        }
#elif defined(__SSE2__)
        int last128 = last & ~15;
        uint8_t mask16[16];
        for (int k = 0; k < 16; ++k) {
            mask16[k] = maskbuf[(j + k) % 4];
        }
        __m128i mask128 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(mask16));
        for (; i < last128; i += 16, j += 16) {
            __m128i in128 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + j));
            __m128i out128 = _mm_xor_si128(in128, mask128);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), out128);
        }
#else
        int last64 = last & ~7;
        for (; i < last64; i += 8, j += 8) {
            for (int k = 0; k < 8; ++k) {
                dst[i + k] = src[j + k] ^ static_cast<char>(maskbuf[(j + k) % 4]);
            }
        }
#endif
    }

    for (; i < last; ++i, ++j) {
        dst[i] = src[j] ^ static_cast<char>(maskbuf[j % 4]);
    }
}

}  // namespace

TEST_CASE("websocket mask simd matches plain edge cases", "[websocket][simd][mask]")
{
    const vector<int> lengths{0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 1023};
    const vector<int> offsets{0, 1, 2, 3, 7, 8, 15, 16, 17};
    const vector<uint32_t> maskkeys{1u, 0x11223344u, 0xffffffffu, 0xa5a5a5a5u};

    mt19937 rng(0x5a17u);
    uniform_int_distribution<int> byteDist(0, 255);

    for (int payloadLen : lengths) {
        vector<char> src(static_cast<size_t>(payloadLen));
        for (int i = 0; i < payloadLen; ++i) {
            src[static_cast<size_t>(i)] = static_cast<char>(byteDist(rng));
        }
        for (int offset : offsets) {
            for (uint32_t maskkey : maskkeys) {
                for (int size : {payloadLen, payloadLen + 9, max(0, payloadLen - 3)}) {
                    vector<char> plain(static_cast<size_t>(offset + size + 32), static_cast<char>(0x7a));
                    vector<char> simd = plain;
                    applyMaskPlain(plain.data(), offset, size, src.data(), payloadLen, maskkey);
                    applyMaskSimd(simd.data(), offset, size, src.data(), payloadLen, maskkey);
                    INFO("payloadLen=" << payloadLen << ", offset=" << offset << ", size=" << size);
                    INFO("maskkey=0x" << hex << maskkey);
                    REQUIRE(simd == plain);
                }
            }
        }
    }
}

TEST_CASE("websocket mask simd matches plain randomized stress", "[websocket][simd][mask][stress]")
{
    mt19937 rng(0xdeadbeefu);
    uniform_int_distribution<int> lenDist(0, 4096);
    uniform_int_distribution<int> offDist(0, 31);
    uniform_int_distribution<int> byteDist(0, 255);
    uniform_int_distribution<uint32_t> maskDist(1u, 0xffffffffu);

    const int rounds = 50000;
    for (int round = 0; round < rounds; ++round) {
        int payloadLen = lenDist(rng);
        int offset = offDist(rng);
        int size = lenDist(rng) % (payloadLen + 33);
        uint32_t maskkey = maskDist(rng);

        vector<char> src(static_cast<size_t>(payloadLen));
        for (int i = 0; i < payloadLen; ++i) {
            src[static_cast<size_t>(i)] = static_cast<char>(byteDist(rng));
        }

        vector<char> plain(static_cast<size_t>(offset + size + 64), static_cast<char>(0x55));
        vector<char> simd = plain;

        applyMaskPlain(plain.data(), offset, size, src.data(), payloadLen, maskkey);
        applyMaskSimd(simd.data(), offset, size, src.data(), payloadLen, maskkey);

        INFO("round=" << round);
        INFO("payloadLen=" << payloadLen << ", offset=" << offset << ", size=" << size);
        INFO("maskkey=0x" << hex << maskkey);
        REQUIRE(simd == plain);
    }
}
