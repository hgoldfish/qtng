using namespace std;

#include <cstdint>
#include <string>
#include <vector>

#include "qtng/utils/punycode.h"

namespace qtng {
namespace utils {

namespace {

const uint32_t kBase = 36;
const uint32_t kTMin = 1;
const uint32_t kTMax = 26;
const uint32_t kSkew = 38;
const uint32_t kDamp = 700;
const uint32_t kInitialBias = 72;
const uint32_t kInitialN = 128;
const uint32_t kMaxCodepoint = 0x10FFFF;
const uint32_t kInvalidDigit = kBase;  // invalid sentinel for decode_digit

char encodeDigit(uint32_t d)
{
    // 0..25 -> 'a'..'z', 26..35 -> '0'..'9' (always lowercase output, matching RFC 3492)
    if (d < 26) {
        return static_cast<char>('a' + d);
    }
    return static_cast<char>('0' + (d - 26));
}

uint32_t decodeDigit(char ch)
{
    unsigned char c = static_cast<unsigned char>(ch);
    if (c >= '0' && c <= '9') {
        return static_cast<uint32_t>(c - '0') + 26;
    }
    if (c >= 'A' && c <= 'Z') {
        return static_cast<uint32_t>(c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return static_cast<uint32_t>(c - 'a');
    }
    return kInvalidDigit;
}

// RFC 3492 section 6.1: bias adaptation function.
uint32_t adapt(uint32_t delta, uint32_t numpoints, bool firsttime)
{
    delta = firsttime ? delta / kDamp : delta / 2;
    delta += delta / numpoints;
    uint32_t k = 0;
    while (delta > ((kBase - kTMin) * kTMax) / 2) {
        delta /= (kBase - kTMin);
        k += kBase;
    }
    return k + ((kBase - kTMin + 1) * delta) / (delta + kSkew);
}

}  // namespace

bool punycodeEncode(const vector<uint32_t> &input, string &output)
{
    output.clear();
    // Output all basic code points (< 0x80) first, preserving original order
    size_t h = 0;
    for (uint32_t cp : input) {
        if (cp > kMaxCodepoint) {
            return false;
        }
        if (cp < 0x80) {
            output.push_back(static_cast<char>(cp));
            ++h;
        }
    }
    const size_t b = h;  // number of basic code points, used by adapt for firsttime
    if (h == input.size()) {
        return true;  // all basic code points, no extended encoding needed
    }
    if (h > 0) {
        output.push_back('-');  // separator between basic code points and extended part
    }

    uint32_t n = kInitialN;
    uint32_t delta = 0;
    uint32_t bias = kInitialBias;
    const size_t total = input.size();
    while (h < total) {
        // Find the smallest code point >= n
        uint32_t m = 0xFFFFFFFFu;
        for (uint32_t cp : input) {
            if (cp >= n && cp < m) {
                m = cp;
            }
        }
        // delta += (m - n) * (h + 1), guard against overflow
        if (m - n > (0xFFFFFFFFu - delta) / (h + 1)) {
            return false;
        }
        delta += (m - n) * static_cast<uint32_t>(h + 1);
        n = m;
        for (size_t idx = 0; idx < total; ++idx) {
            uint32_t cp = input[idx];
            if (cp < n) {
                if (++delta == 0) {
                    return false;  // delta overflow
                }
            }
            if (cp == n) {
                // Encode delta as a generalized variable-length integer
                uint32_t q = delta;
                for (uint32_t k = kBase;; k += kBase) {
                    uint32_t t = (k <= bias) ? kTMin
                              : (k >= bias + kTMax) ? kTMax
                                                    : k - bias;
                    if (q < t) {
                        break;
                    }
                    output.push_back(encodeDigit(t + (q - t) % (kBase - t)));
                    q = (q - t) / (kBase - t);
                }
                output.push_back(encodeDigit(q));
                bias = adapt(delta, static_cast<uint32_t>(h + 1), h == b);
                delta = 0;
                ++h;
            }
        }
        ++delta;
        ++n;
    }
    return true;
}

bool punycodeDecode(const string &input, vector<uint32_t> &output)
{
    output.clear();
    // Basic code points are everything before the last '-'
    size_t lastDash = input.rfind('-');
    size_t basicEnd = (lastDash == string::npos) ? 0 : lastDash;
    for (size_t i = 0; i < basicEnd; ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c >= 0x80) {
            return false;  // basic code points must be ASCII
        }
        output.push_back(static_cast<uint32_t>(c));
    }
    // Skip the separator (if present)
    size_t pos = (basicEnd > 0) ? basicEnd + 1 : 0;

    uint32_t i = 0;
    uint32_t n = kInitialN;
    uint32_t bias = kInitialBias;
    while (pos < input.size()) {
        uint32_t oldi = i;
        uint32_t w = 1;
        uint32_t k = kBase;
        for (;;) {
            if (pos >= input.size()) {
                return false;  // truncated input
            }
            uint32_t digit = decodeDigit(input[pos++]);
            if (digit >= kBase) {
                return false;  // illegal character
            }
            if (digit > (0xFFFFFFFFu - i) / w) {
                return false;  // i overflow
            }
            i += digit * w;
            uint32_t t = (k <= bias) ? kTMin
                      : (k >= bias + kTMax) ? kTMax
                                            : k - bias;
            if (digit < t) {
                break;
            }
            if (w > 0xFFFFFFFFu / (kBase - t)) {
                return false;  // w overflow
            }
            w *= (kBase - t);
            k += kBase;
        }
        const uint32_t outPlus1 = static_cast<uint32_t>(output.size() + 1);
        bias = adapt(i - oldi, outPlus1, oldi == 0);
        n += i / outPlus1;
        i %= outPlus1;
        if (n > kMaxCodepoint) {
            return false;
        }
        output.insert(output.begin() + static_cast<ptrdiff_t>(i), n);
        ++i;
    }
    return true;
}

}  // namespace utils
}  // namespace qtng
