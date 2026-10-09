#ifndef QTNG_TYPES_BYTES_H
#define QTNG_TYPES_BYTES_H

// qtng::ByteArray -- an implicitly shared sequence of bytes.
//
// ByteArray shares the storage and layout of qtng::String (see
// types/private/string_data.h) but nothing else: every operation below is defined in terms
// of *bytes*, and belongs here rather than on the shared base precisely because String's
// counterparts must be defined in terms of text (UTF-8 code points). A byte indexOf() is
// not a character indexOf(), so the two must not share an implementation.
//
// There is **no implicit conversion** between the two, and neither binds to the other, so
// mistakes such as treating binary as text (or vice versa) can only happen at an explicit
// conversion -- String::fromUtf8() / String::toUtf8() for the UTF-8 reinterpretation (no
// transcoding), or the hex/base64 members for a real re-encoding.
//
// Conventions:
//   - use String for text (field names, primary keys, strings inside values);
//   - use ByteArray for raw bytes (rows in LMDB, msgpack BIN, serialized buffers).
//
// Like String, it is immutable: there is no append()/resize()/writable operator[], and no
// c_str() (NUL termination is not guaranteed). Build with StringBuilder instead.

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include "qtng/msgpack.h"
#include "qtng/types/private/string_data.h"

namespace qtng {

namespace detail {

// Hex/base64 digit classification. Returning -1 for "not a digit" is what lets
// fromHex()/fromBase64() skip invalid input the way QByteArray does.
inline int hexNibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

inline int base64Digit(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

inline bool isAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Parse base accepted by toLongLong()/toULongLong(): 0 means "auto-detect", otherwise
// 2..36. Anything else passed to strtoll()/strtoull() is undefined behavior by the C
// standard -- glibc happens to set EINVAL and leave `end` untouched, but a different libc
// need not, so reject it before the call instead of relying on errno.
inline bool isValidParseBase(int base)
{
    return base == 0 || (base >= 2 && base <= 36);
}

// Rightmost occurrence of byte `c` at or before the inclusive index `limit` (which must be
// < len), or nullptr when there is none. memrchr() is a GNU extension and is the SIMD kernel
// that makes a backward search of a long buffer cheap; it is used only when the libc actually
// declares it (glibc, and only with _GNU_SOURCE, which libstdc++ defines for every C++
// translation unit). Everywhere else the portable backward loop is used. Both live here so
// the feature test and the fallback are written once, and so no macro has to leak out of this
// public header.
inline const char *rfindByte(const char *p, unsigned char c, std::uint32_t limit)
{
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    return static_cast<const char *>(::memrchr(p, c, static_cast<std::size_t>(limit) + 1));
#else
    for (std::uint32_t i = limit + 1; i-- > 0;) {
        if (static_cast<unsigned char>(p[i]) == c) {
            return p + i;
        }
    }
    return nullptr;
#endif
}

}  // namespace detail

class ByteArray : public detail::BasicCharBlock<detail::ByteArrayTag>
{
    typedef detail::BasicCharBlock<detail::ByteArrayTag> Base;

public:
    enum
    {
        // "Not found", returned by indexOf()/lastIndexOf(). No byte can live at index
        // 0xffffffff: a block's length -- and therefore its largest valid index + 1 -- is
        // bounded by MaxLength. Mirrors std::string::npos, which is also size_type(-1).
        npos = 0xffffffffu,
    };

    // The base owns the storage and the generic construction forms; inheriting its
    // constructors keeps (const char*, len), std::string and const char* available.
    using Base::Base;
    ByteArray() = default;

    // Converting constructors from the base type. They are what let a slice (which the base
    // returns as the base type) stay a ByteArray, so byte-only members such as toHex()
    // remain reachable on `ba.mid(...)`. StringBuilder::takeByteArray() relies on this too.
    ByteArray(const Base &other)
        : Base(other)
    {
    }
    ByteArray(Base &&other)
        : Base(std::move(other))
    {
    }

public:
    // -- read-only element access (byte semantics) --
    //
    // The result is a byte value, not a reference, because the type is immutable: there is
    // no writable operator[] (use StringBuilder to build).
    //
    // Out-of-range does not go out of bounds -- it returns '\0'. QByteArray::at() asserts
    // (and operator[] is UB); a value type has no assert channel, and aborting a process
    // over a bad index is worse than a well-defined sentinel. Callers that need to tell
    // "real NUL" from "out of range" must check the index against size() first.
    char at(std::uint32_t i) const { return i < size() ? data()[i] : '\0'; }
    char operator[](std::uint32_t i) const { return at(i); }

    // Empty content yields '\0' for both, matching the sentinel above.
    char front() const { return size() == 0 ? '\0' : data()[0]; }
    char back() const { return size() == 0 ? '\0' : data()[size() - 1]; }

    // QByteArray::constData() spelling, for callers migrating from Qt.
    const char *constData() const { return data(); }

public:
    // -- slicing --
    //
    // mid(offset) without a length means "to the end", like QByteArray::mid(pos, -1). The
    // base's mid() already clamps a too-long len and returns empty for an out-of-range
    // offset, so npos is safe to pass straight through.
    ByteArray mid(std::uint32_t offset, std::uint32_t len = npos) const;
    ByteArray left(std::uint32_t len) const;
    ByteArray right(std::uint32_t len) const;

public:
    // -- substring search --
    //
    // Plain byte scans: they never detach, allocate, or look past size(). The empty needle
    // matches at `from` (std::string::find semantics), which is what indexOf(empty) == from
    // means; there is no "negative index" in this type.

    // First occurrence at or after `from`, or npos. Tolerates from > size() (returns npos)
    // instead of reading out of bounds.
    std::uint32_t indexOf(const ByteArray &needle, std::uint32_t from = 0) const;
    std::uint32_t indexOf(char c, std::uint32_t from = 0) const;

    // Last occurrence at or before `from`; the default (npos) means "search from the end".
    std::uint32_t lastIndexOf(const ByteArray &needle, std::uint32_t from = npos) const;
    std::uint32_t lastIndexOf(char c, std::uint32_t from = npos) const;

    bool contains(const ByteArray &needle) const { return indexOf(needle) != npos; }
    bool contains(char c) const { return indexOf(c) != npos; }

    bool startsWith(const ByteArray &prefix) const;
    bool endsWith(const ByteArray &suffix) const;

    // Non-overlapping occurrence count. QByteArray's count(ba) is also non-overlapping (it
    // advances past each match), so "aaaa".count("aa") is 2, not 3.
    std::uint32_t count(const ByteArray &needle) const;
    std::uint32_t count(char c) const;

public:
    // -- ordering --
    //
    // ==, != and < are inherited from the shared base (it defines exactly those three).
    // The base has no >/<=/>=, so add them here, derived from < so there is a single
    // ordering definition.
    bool operator>(const ByteArray &other) const { return other < *this; }
    bool operator<=(const ByteArray &other) const { return !(other < *this); }
    bool operator>=(const ByteArray &other) const { return !(*this < other); }

public:
    // -- hex / base64 (byte-only: text has no meaningful encoding) --

    // Lower-case hex, two digits per byte (QByteArray::toHex()).
    ByteArray toHex() const;
    // Decode hex; non-hex bytes are skipped and a trailing odd digit is ignored, matching
    // QByteArray::fromHex().
    static ByteArray fromHex(const ByteArray &hex);

    ByteArray toBase64() const;
    // Decode standard base64; any byte outside the alphabet (including '=' padding) is
    // skipped, so unpadded input is accepted.
    static ByteArray fromBase64(const ByteArray &base64);

    // This content repeated `times` times (QByteArray::repeated()). A result that would
    // exceed MaxLength yields an empty array rather than wrapping and overrunning.
    ByteArray repeated(std::uint32_t times) const;

public:
    // -- ASCII transforms --
    //
    // Deliberately ASCII-only: a ByteArray is opaque bytes, so case folding must not pretend
    // to know an encoding. String (text) is where Unicode-aware folding would belong.

    ByteArray toLower() const;
    ByteArray toUpper() const;
    // Strip leading/trailing ASCII whitespace (QByteArray::trimmed()).
    ByteArray trimmed() const;

public:
    // -- numeric conversion --
    //
    // One template for every integral type, so number(7u)/number(std::int64_t(7)) are exact
    // matches instead of equally-ranked integral conversions (which would be ambiguous).
    // base is 2..36; an out-of-range base yields an empty array.
    template<typename T>
    static typename std::enable_if<std::is_integral<T>::value, ByteArray>::type
    number(T value, int base = 10)
    {
        return numberUnsigned(magnitudeOf(value), isNegative(value), base);
    }

    // Double formatting mirrors QByteArray::number(double, char, int): format is one of
    // 'g'/'G'/'e'/'E'/'f', anything else falls back to 'g'.
    static ByteArray number(double value, char format = 'g', int precision = 6);

    int toInt(bool *ok = nullptr, int base = 10) const;
    unsigned int toUInt(bool *ok = nullptr, int base = 10) const;
    long long toLongLong(bool *ok = nullptr, int base = 10) const;
    unsigned long long toULongLong(bool *ok = nullptr, int base = 10) const;
    double toDouble(bool *ok = nullptr) const;
    float toFloat(bool *ok = nullptr) const;

private:
    // Integral helpers for the number() template. magnitudeOf() works on the unsigned image
    // so that negating INT64_MIN does not overflow.
    template<typename T>
    static std::uint64_t magnitudeOf(T value)
    {
        return isNegative(value) ? (std::uint64_t(0) - static_cast<std::uint64_t>(value))
                                 : static_cast<std::uint64_t>(value);
    }
    // Tag dispatch keeps `value < 0` out of the unsigned instantiation, where it would
    // always be false (and would warn).
    template<typename T>
    static bool isNegativeImpl(T value, std::true_type) { return value < 0; }
    template<typename T>
    static bool isNegativeImpl(T, std::false_type) { return false; }
    template<typename T>
    static bool isNegative(T value)
    {
        return isNegativeImpl(value, std::is_signed<T>());
    }

    static ByteArray numberUnsigned(std::uint64_t magnitude, bool negative, int base);
};

// The derived class adds no data members, so it must keep the base's size and alignment
// (the shared layout is pinned in types/private/string_data.h).
static_assert(sizeof(ByteArray) == 32, "ByteArray must stay 32 bytes");
static_assert(alignof(ByteArray) == 8, "ByteArray alignment must be 8");

// ---------------------------------------------------------------------------
// Out-of-line implementations
// ---------------------------------------------------------------------------

inline ByteArray ByteArray::mid(std::uint32_t offset, std::uint32_t len) const
{
    return ByteArray(Base::mid(offset, len));
}

inline ByteArray ByteArray::left(std::uint32_t len) const
{
    return ByteArray(Base::mid(0, len));
}

inline ByteArray ByteArray::right(std::uint32_t len) const
{
    const std::uint32_t n = size();
    return len >= n ? *this : ByteArray(Base::mid(n - len, len));
}

inline std::uint32_t ByteArray::indexOf(const ByteArray &needle, std::uint32_t from) const
{
    const std::uint32_t n = size();
    if (from > n) {
        return npos;
    }
    const std::uint32_t m = needle.size();
    if (m == 0) {
        return from;
    }
    if (m > n - from) {
        return npos;
    }
    const char *h = data();
    const char *nd = needle.data();
    const std::uint32_t last = n - m;
    // Find candidate first bytes with memchr (SIMD in every libc) and confirm each with
    // memcmp. A hand-written first-byte loop here was ~30-50x slower: it walks one byte at
    // a time and reads as a scalar scan to the vectoriser, whereas memchr is the same
    // kernel std::string::find uses. The window is only `last - from + 1` long, not the
    // whole tail, or the last miss would read up to m-1 bytes past the end.
    const char first = nd[0];
    const char *cur = h + from;
    std::uint32_t remaining = last - from + 1;
    while (remaining > 0) {
        const void *hit = std::memchr(cur, static_cast<unsigned char>(first), remaining);
        if (hit == nullptr) {
            return npos;
        }
        const char *p = static_cast<const char *>(hit);
        if (m == 1 || std::memcmp(p, nd, m) == 0) {
            return static_cast<std::uint32_t>(p - h);
        }
        // A false first byte: resume right after it, staying inside the window.
        const std::uint32_t skipped = static_cast<std::uint32_t>(p - cur) + 1;
        cur += skipped;
        remaining -= skipped;
    }
    return npos;
}

inline std::uint32_t ByteArray::indexOf(char c, std::uint32_t from) const
{
    const std::uint32_t n = size();
    if (from > n) {
        return npos;
    }
    const void *p = std::memchr(data() + from, static_cast<unsigned char>(c), n - from);
    return p == nullptr ? npos : static_cast<std::uint32_t>(static_cast<const char *>(p) - data());
}

inline std::uint32_t ByteArray::lastIndexOf(const ByteArray &needle, std::uint32_t from) const
{
    const std::uint32_t n = size();
    const std::uint32_t m = needle.size();
    if (m > n) {
        return npos;
    }
    // Clamp so the window end never runs past size(); from == npos means "end".
    const std::uint32_t start = (from == npos || from > n - m) ? (n - m) : from;
    if (m == 0) {
        return start;
    }
    const char *h = data();
    const char *nd = needle.data();
    // Probe the needle's *last* byte rather than its first: a backward search visits those
    // positions first, and rfindByte() returns the rightmost one in the window. Confirm each
    // hit with memcmp and, on a false hit, shrink the limit to the bytes before it. A match
    // starting at offset j has its last byte at h[j + m - 1], so the largest last-byte offset
    // a match can use is start + m - 1 -- i.e. relative index `start` into `base`.
    const char lastByte = nd[m - 1];
    const char *base = h + m - 1;
    std::uint32_t limit = start;
    for (;;) {
        const char *hit = detail::rfindByte(base, static_cast<unsigned char>(lastByte), limit);
        if (hit == nullptr) {
            return npos;
        }
        const std::uint32_t candidate = static_cast<std::uint32_t>(hit - base);
        if (std::memcmp(hit - (m - 1), nd, m) == 0) {
            return candidate;
        }
        if (candidate == 0) {
            return npos;  // no room for an earlier candidate
        }
        limit = candidate - 1;
    }
}

inline std::uint32_t ByteArray::lastIndexOf(char c, std::uint32_t from) const
{
    const std::uint32_t n = size();
    if (n == 0) {
        return npos;
    }
    const std::uint32_t start = (from == npos || from >= n) ? n - 1 : from;
    const char *h = data();
    const char *p = detail::rfindByte(h, static_cast<unsigned char>(c), start);
    return p == nullptr ? npos : static_cast<std::uint32_t>(p - h);
}

inline bool ByteArray::startsWith(const ByteArray &prefix) const
{
    const std::uint32_t m = prefix.size();
    return m <= size() && (m == 0 || std::memcmp(data(), prefix.data(), m) == 0);
}

inline bool ByteArray::endsWith(const ByteArray &suffix) const
{
    const std::uint32_t n = size();
    const std::uint32_t m = suffix.size();
    return m <= n && (m == 0 || std::memcmp(data() + (n - m), suffix.data(), m) == 0);
}

inline std::uint32_t ByteArray::count(const ByteArray &needle) const
{
    const std::uint32_t m = needle.size();
    if (m == 0) {
        return size() + 1;
    }
    std::uint32_t occurrences = 0;
    std::uint32_t pos = 0;
    while ((pos = indexOf(needle, pos)) != npos) {
        ++occurrences;
        pos += m;
    }
    return occurrences;
}

inline std::uint32_t ByteArray::count(char c) const
{
    std::uint32_t occurrences = 0;
    const char *h = data();
    const std::uint32_t n = size();
    for (std::uint32_t i = 0; i < n; ++i) {
        if (h[i] == c) {
            ++occurrences;
        }
    }
    return occurrences;
}

inline ByteArray ByteArray::toHex() const
{
    static const char digits[] = "0123456789abcdef";
    const std::uint32_t n = size();
    std::string out(static_cast<std::size_t>(n) * 2, '\0');
    const char *src = data();
    for (std::uint32_t i = 0; i < n; ++i) {
        const unsigned char b = static_cast<unsigned char>(src[i]);
        out[static_cast<std::size_t>(i) * 2] = digits[b >> 4];
        out[static_cast<std::size_t>(i) * 2 + 1] = digits[b & 0x0f];
    }
    return ByteArray(out);
}

inline ByteArray ByteArray::fromHex(const ByteArray &hex)
{
    const std::uint32_t n = hex.size();
    std::string out;
    out.reserve(n / 2);
    const char *src = hex.data();
    int high = -1;
    for (std::uint32_t i = 0; i < n; ++i) {
        const int v = detail::hexNibble(src[i]);
        if (v < 0) {
            continue;
        }
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<char>((high << 4) | v));
            high = -1;
        }
    }
    // A trailing odd digit is silently dropped, like QByteArray::fromHex().
    return ByteArray(out);
}

inline ByteArray ByteArray::toBase64() const
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::uint32_t n = size();
    const unsigned char *src = reinterpret_cast<const unsigned char *>(data());
    std::string out;
    out.reserve((static_cast<std::size_t>(n) + 2) / 3 * 4);

    std::uint32_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(src[i]) << 16)
                | (static_cast<std::uint32_t>(src[i + 1]) << 8) | static_cast<std::uint32_t>(src[i + 2]);
        out.push_back(alphabet[(triple >> 18) & 0x3f]);
        out.push_back(alphabet[(triple >> 12) & 0x3f]);
        out.push_back(alphabet[(triple >> 6) & 0x3f]);
        out.push_back(alphabet[triple & 0x3f]);
    }
    const std::uint32_t remaining = n - i;
    if (remaining == 1) {
        const std::uint32_t triple = static_cast<std::uint32_t>(src[i]) << 16;
        out.push_back(alphabet[(triple >> 18) & 0x3f]);
        out.push_back(alphabet[(triple >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(src[i]) << 16)
                | (static_cast<std::uint32_t>(src[i + 1]) << 8);
        out.push_back(alphabet[(triple >> 18) & 0x3f]);
        out.push_back(alphabet[(triple >> 12) & 0x3f]);
        out.push_back(alphabet[(triple >> 6) & 0x3f]);
        out.push_back('=');
    }
    return ByteArray(out);
}

inline ByteArray ByteArray::fromBase64(const ByteArray &base64)
{
    const std::uint32_t n = base64.size();
    std::string out;
    out.reserve(static_cast<std::size_t>(n) / 4 * 3);
    const char *src = base64.data();
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const int v = detail::base64Digit(src[i]);
        if (v < 0) {
            // '=' padding and any stray byte are skipped. Only the low bits of the
            // accumulator are ever read, so its 32-bit wrap-around is harmless.
            continue;
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    return ByteArray(out);
}

inline ByteArray ByteArray::repeated(std::uint32_t times) const
{
    if (times == 0 || isEmpty()) {
        return ByteArray();
    }
    if (times == 1) {
        return *this;
    }
    const std::uint64_t total = static_cast<std::uint64_t>(size()) * times;
    if (total > static_cast<std::uint64_t>(Base::MaxLength)) {
        // Refuse rather than wrap: an array longer than MaxLength cannot be represented, and
        // a wrapped length would make size() lie while the buffer holds the real bytes.
        return ByteArray();
    }
    const std::uint32_t n = size();
    std::string out(static_cast<std::size_t>(total), '\0');
    char *dst = &out[0];
    for (std::uint32_t i = 0; i < times; ++i) {
        std::memcpy(dst, data(), n);
        dst += n;
    }
    return ByteArray(out);
}

inline ByteArray ByteArray::toLower() const
{
    std::string out = toStdString();
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (c >= 'A' && c <= 'Z') {
            out[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ByteArray(out);
}

inline ByteArray ByteArray::toUpper() const
{
    std::string out = toStdString();
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (c >= 'a' && c <= 'z') {
            out[i] = static_cast<char>(c - 'a' + 'A');
        }
    }
    return ByteArray(out);
}

inline ByteArray ByteArray::trimmed() const
{
    const char *p = data();
    std::uint32_t begin = 0;
    std::uint32_t end = size();
    while (begin < end && detail::isAsciiSpace(p[begin])) {
        ++begin;
    }
    while (end > begin && detail::isAsciiSpace(p[end - 1])) {
        --end;
    }
    // Go through mid() like left()/right(), so a long trimmed result shares the parent block
    // instead of copying it. mid() also handles the all-whitespace case (begin == end == size
    // -> empty).
    return mid(begin, end - begin);
}

inline ByteArray ByteArray::numberUnsigned(std::uint64_t magnitude, bool negative, int base)
{
    if (base < 2 || base > 36) {
        return ByteArray();
    }
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    // 64 binary digits plus a sign. Fill from the back so the result is one contiguous range
    // that the (const char*, len) constructor can copy directly.
    char buf[72];
    char *const end = buf + sizeof(buf);
    char *p = end;
    do {
        *--p = digits[magnitude % static_cast<std::uint64_t>(base)];
        magnitude /= static_cast<std::uint64_t>(base);
    } while (magnitude != 0);
    if (negative) {
        *--p = '-';
    }
    return ByteArray(p, static_cast<std::uint32_t>(end - p));
}

inline ByteArray ByteArray::number(double value, char format, int precision)
{
    if (format != 'g' && format != 'G' && format != 'e' && format != 'E' && format != 'f') {
        format = 'g';
    }
    if (precision < 0) {
        precision = 6;
    }
    char fmt[16];
    std::snprintf(fmt, sizeof(fmt), "%%.%d%c", precision, format);
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), fmt, value);
    if (n <= 0) {
        return ByteArray();
    }
    if (static_cast<std::size_t>(n) < sizeof(buf)) {
        return ByteArray(buf, static_cast<std::uint32_t>(n));
    }
    // Rare: a long %f needs more room. Format again into an exact-size buffer.
    std::string big(static_cast<std::size_t>(n) + 1, '\0');
    std::snprintf(&big[0], big.size(), fmt, value);
    big.resize(static_cast<std::size_t>(n));
    return ByteArray(big);
}

inline int ByteArray::toInt(bool *ok, int base) const
{
    bool parsed = false;
    const long long value = toLongLong(&parsed, base);
    const bool inRange = parsed && value >= INT_MIN && value <= INT_MAX;
    if (ok != nullptr) {
        *ok = inRange;
    }
    return inRange ? static_cast<int>(value) : 0;
}

inline unsigned int ByteArray::toUInt(bool *ok, int base) const
{
    bool parsed = false;
    const unsigned long long value = toULongLong(&parsed, base);
    const bool inRange = parsed && value <= UINT_MAX;
    if (ok != nullptr) {
        *ok = inRange;
    }
    return inRange ? static_cast<unsigned int>(value) : 0;
}

inline long long ByteArray::toLongLong(bool *ok, int base) const
{
    if (!detail::isValidParseBase(base)) {
        if (ok != nullptr) {
            *ok = false;
        }
        return 0;
    }
    const std::string s = toStdString();
    char *end = nullptr;
    errno = 0;
    const long long value = std::strtoll(s.c_str(), &end, base);
    const bool success = end != s.c_str() && errno == 0;
    if (ok != nullptr) {
        *ok = success;
    }
    return success ? value : 0;
}

inline unsigned long long ByteArray::toULongLong(bool *ok, int base) const
{
    if (!detail::isValidParseBase(base)) {
        if (ok != nullptr) {
            *ok = false;
        }
        return 0;
    }
    const std::string s = toStdString();
    // strtoull() wraps a leading '-' into a huge number; a negative value is a failed
    // unsigned conversion, matching QByteArray::toULongLong().
    const char *p = s.c_str();
    while (detail::isAsciiSpace(*p)) {
        ++p;
    }
    if (*p == '-') {
        if (ok != nullptr) {
            *ok = false;
        }
        return 0;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(s.c_str(), &end, base);
    const bool success = end != s.c_str() && errno == 0;
    if (ok != nullptr) {
        *ok = success;
    }
    return success ? value : 0;
}

inline double ByteArray::toDouble(bool *ok) const
{
    const std::string s = toStdString();
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(s.c_str(), &end);
    const bool success = end != s.c_str() && errno == 0;
    if (ok != nullptr) {
        *ok = success;
    }
    return success ? value : 0.0;
}

inline float ByteArray::toFloat(bool *ok) const
{
    return static_cast<float>(toDouble(ok));
}

// ---------------------------------------------------------------------------
// MsgPack: ByteArray is bytes, so it streams as msgpack bin (never str)
// ---------------------------------------------------------------------------
//
// This is the byte-side counterpart of the removed MsgPackStream::operator<<(std::string):
// a bare std::string could mean either str or bin, but ByteArray is unambiguously bytes, so
// the wire type no longer has to be guessed. An empty payload is written as bin of length 0.
//
// Note both writeBytes() overloads emit a BIN header; writeBytes(std::string) is
// used here. Callers that need raw bytes (DER/TLV, NIL) must use writeRawBytes()
// so the payload does not go out without its msgpack header.
inline MsgPackStream &operator<<(MsgPackStream &s, const ByteArray &bytes)
{
    s.writeBytes(bytes.toStdString());
    return s;
}

inline MsgPackStream &operator>>(MsgPackStream &s, ByteArray &bytes)
{
    std::string tmp;
    if (s.readBytes(tmp)) {
        bytes = ByteArray(tmp);
    }
    // On failure readBytes() leaves tmp untouched and sets ReadCorruptData; leave the
    // caller's ByteArray alone rather than clobbering it with a half-read value.
    return s;
}

}  // namespace qtng

#endif  // QTNG_TYPES_BYTES_H
