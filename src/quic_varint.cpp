#include "qtng/private/quic_p.h"

using namespace std;

namespace qtng {

size_t quicVarintSize(uint64_t value)
{
    if (value <= 63) {
        return 1;
    }
    if (value <= 16383) {
        return 2;
    }
    if (value <= 1073741823ull) {
        return 4;
    }
    return 8;
}

bool quicEncodeVarint(uint64_t value, string *out)
{
    if (!out) {
        return false;
    }
    if (value <= 63) {
        out->push_back(static_cast<char>(value));
        return true;
    }
    if (value <= 16383) {
        out->push_back(static_cast<char>(0x40 | ((value >> 8) & 0x3f)));
        out->push_back(static_cast<char>(value & 0xff));
        return true;
    }
    if (value <= 1073741823ull) {
        out->push_back(static_cast<char>(0x80 | ((value >> 24) & 0x3f)));
        out->push_back(static_cast<char>((value >> 16) & 0xff));
        out->push_back(static_cast<char>((value >> 8) & 0xff));
        out->push_back(static_cast<char>(value & 0xff));
        return true;
    }
    if (value > 4611686018427387903ull) {
        return false;
    }
    out->push_back(static_cast<char>(0xc0 | ((value >> 56) & 0x3f)));
    out->push_back(static_cast<char>((value >> 48) & 0xff));
    out->push_back(static_cast<char>((value >> 40) & 0xff));
    out->push_back(static_cast<char>((value >> 32) & 0xff));
    out->push_back(static_cast<char>((value >> 24) & 0xff));
    out->push_back(static_cast<char>((value >> 16) & 0xff));
    out->push_back(static_cast<char>((value >> 8) & 0xff));
    out->push_back(static_cast<char>(value & 0xff));
    return true;
}

bool quicDecodeVarint(const char *data, size_t size, size_t *consumed, uint64_t *value)
{
    if (!data || !consumed || !value || size == 0) {
        return false;
    }
    const unsigned char prefix = static_cast<unsigned char>(data[0]) >> 6;
    size_t len = 1u << prefix;
    if (size < len) {
        return false;
    }
    uint64_t v = static_cast<unsigned char>(data[0]) & 0x3full;
    for (size_t i = 1; i < len; ++i) {
        v = (v << 8) | static_cast<unsigned char>(data[i]);
    }
    *value = v;
    *consumed = len;
    return true;
}

}  // namespace qtng
