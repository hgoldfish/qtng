#ifndef QTNG_UTILS_PUNYCODE_H
#define QTNG_UTILS_PUNYCODE_H

#include <cstdint>
#include <string>
#include <vector>

namespace qtng {
namespace utils {

// RFC 3492 Punycode encode/decode.
//
// This module performs only pure Punycode conversion; it does not include IDNA
// Nameprep (NFKC normalization, case folding, Bidi/Joining checks, or excluded
// code point checks). Callers must split labels, handle the "xn--" prefix
// themselves, and normalize ASCII case where required.
//
// Code point range: [0, 0x10FFFF]; surrogate code points [0xD800, 0xDFFF] are
// not allowed.

// Encodes a sequence of Unicode code points into an ASCII Punycode string.
// Returns false on encoding failure (out-of-range code point or delta overflow)
// and leaves output unmodified.
// Empty input yields an empty output.
bool punycodeEncode(const std::vector<std::uint32_t> &input, std::string &output);

// Decodes an ASCII Punycode string into a sequence of Unicode code points.
// The input must contain only ASCII; returns false on decoding failure
// (illegal character, delta overflow, or truncated input).
bool punycodeDecode(const std::string &input, std::vector<std::uint32_t> &output);

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_PUNYCODE_H
