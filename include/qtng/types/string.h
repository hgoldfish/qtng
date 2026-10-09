#ifndef QTNG_TYPES_STRING_H
#define QTNG_TYPES_STRING_H

// qtng::String -- an implicitly shared text type.
//
// Design notes live in types/private/string_data.h. Only three things matter at the API
// level:
//
//   1. **Immutable**. There is no detach() and no non-const element access. Concatenate
//      directly with `a + b + c`: `+` returns a lightweight expression and the outermost
//      level allocates once for the total length, so joining N pieces allocates only once
//      (and zero times when the result fits in 28 bytes). Accumulating in a loop
//      (`s = s + piece`) is what StringBuilder is for -- `+` rebuilds the whole content
//      every round, which is O(n^2). Immutability also means a String can be shared
//      across threads as-is.
//   2. **No NUL terminator is guaranteed**, and there is no c_str(). The content may
//      contain '\0'; only size() (or byteSize()) defines the length. Always use
//      toUtf8String() or an explicit length when interacting with external libraries.
//   3. Content of length 0..28 is inlined in the handle (no allocation, and a copy is a
//      32-byte memcpy); content longer than 28 lives in a shared block, and copying only
//      bumps an atomic reference count.
//
// ---------------------------------------------------------------------------
// Text semantics: indices and lengths count UTF-8 code points
// ---------------------------------------------------------------------------
//
// String is *text*, so -- like QString and unlike QByteArray -- its size()/length()/
// count(), its indices and its slicing all work in **UTF-8 code points** (QChar
// equivalents), not bytes. `String("héllo").size()` is 5 even though it occupies 6 bytes.
// This means size() is O(n): it walks the UTF-8 once. The byte count stays available as
// byteSize() (O(1)), which is what data()/toUtf8String()/StringBuilder/serialization must
// use. Do not write `data() + size()` to express "the whole buffer" -- that is a bug now;
// write toUtf8String() or use byteSize().
//
// Indexing a string the caller already knows is ASCII is therefore slightly more
// expensive than before, but the alternative (a byte-indexed "text" type) makes every
// non-ASCII character a landmine.
//
// Malformed UTF-8 is never rejected: an invalid or truncated sequence is treated as a
// single one-byte code point for counting/indexing, so size() <= byteSize() always holds
// and no operation reads past the end.
//
// Note that the type of `a + b` is not String but detail::StringConcat. It converts to
// String implicitly and forwards size()/byteSize()/isEmpty()/toUtf8String() and the
// ==/!=/< comparisons, so the usual code keeps working. The only thing it does not
// provide is data()/c_str(): an expression has no contiguous storage, so returning a
// pointer would dangle -- write `String s = a + b;` or `String(a + b)` when you need one.

#include <algorithm>
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
#include "qtng/types/bytes.h"
#include "qtng/types/containers.h"
#include "qtng/types/private/string_data.h"

namespace qtng {

class String;

// Comparison mode for String::compare() and the case-insensitive StringList operations. A
// plain (unscoped) enum, like Qt's, so `CaseInsensitive` can be written without a
// qualifier after `using namespace qtng`.
enum CaseSensitivity
{
    CaseSensitive,
    CaseInsensitive,
};

// StringList is Vector<String> plus the QStringList vocabulary that is not container
// generic: join / filter / sort / replaceInStrings / removeDuplicates and the
// case-insensitive lookups. It used to be a typedef, which cannot carry members; deriving
// instead is safe because naming Vector<String> does not instantiate the container, and a
// function returning an incomplete type by value (String::split) is a legal declaration.
// The class body comes after String, once Vector<String> is complete.
class StringList;

namespace detail {

// ---------------------------------------------------------------------------
// UTF-8 primitives
// ---------------------------------------------------------------------------
//
// These are deliberately tolerant instead of validating: a String may hold arbitrary
// bytes, so a decoder that returned "error" would leave the caller with nowhere to go.
// Malformed input is consumed one byte at a time (the byte itself becomes the code point),
// which guarantees `1 <= consumed <= 4` and that the sum of consumed bytes equals the byte
// length -- the invariant size() <= byteSize() depends on.

// Decode the sequence starting at p (with `remaining` bytes available). Returns the number
// of bytes consumed (>= 1) and writes the code point to *cp. Overlong forms, surrogate
// code points, out-of-range values, bad continuation bytes and truncation all fall back to
// "one byte, code point == that byte".
inline std::uint32_t utf8Decode(const char *p, std::uint32_t remaining, std::uint32_t *cp)
{
    const unsigned char c0 = static_cast<unsigned char>(p[0]);
    if (c0 < 0x80) {
        *cp = c0;
        return 1;
    }
    std::uint32_t continuation;
    std::uint32_t value;
    if ((c0 & 0xE0) == 0xC0) {
        continuation = 1;
        value = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        continuation = 2;
        value = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        continuation = 3;
        value = c0 & 0x07;
    } else {
        *cp = c0;  // continuation byte or invalid lead byte
        return 1;
    }
    if (continuation >= remaining) {
        *cp = c0;  // truncated
        return 1;
    }
    for (std::uint32_t i = 1; i <= continuation; ++i) {
        const unsigned char cc = static_cast<unsigned char>(p[i]);
        if ((cc & 0xC0) != 0x80) {
            *cp = c0;  // bad continuation byte
            return 1;
        }
        value = (value << 6) | (cc & 0x3F);
    }
    // Reject the same malformed forms the UTF-8 validator rejects, so a String that passed
    // validation is counted exactly as it would be by a validating decoder.
    if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        *cp = c0;
        return 1;
    }
    if (continuation == 1 && value < 0x80) {
        *cp = c0;
        return 1;
    }
    if (continuation == 2 && value < 0x800) {
        *cp = c0;
        return 1;
    }
    if (continuation == 3 && value < 0x10000) {
        *cp = c0;
        return 1;
    }
    *cp = value;
    return continuation + 1;
}

// Encode one code point as UTF-8, appending to `out`.
inline void utf8Encode(std::uint32_t cp, std::string &out)
{
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Number of code points in a byte range, using the tolerant decoder above.
inline std::uint32_t utf8Count(const char *p, std::uint32_t bytes)
{
    std::uint32_t count = 0;
    std::uint32_t i = 0;
    std::uint32_t cp = 0;
    while (i < bytes) {
        i += utf8Decode(p + i, bytes - i, &cp);
        ++count;
    }
    return count;
}

// Byte offset of the k-th code point. Returns `bytes` once k reaches the code point count,
// so callers may use it both as "start of code point k" and as "one past the last code
// point of a slice".
inline std::uint32_t utf8ByteOffset(const char *p, std::uint32_t bytes, std::uint32_t k)
{
    std::uint32_t i = 0;
    std::uint32_t index = 0;
    std::uint32_t cp = 0;
    while (i < bytes && index < k) {
        i += utf8Decode(p + i, bytes - i, &cp);
        ++index;
    }
    return i;
}

// Whitespace as defined by QString::isSpace() for the common ranges (ASCII controls, NEL,
// no-break space, ogham space, the general punctuation spaces, line/paragraph separators,
// narrow no-break space, medium mathematical space and the ideographic space).
inline bool isUnicodeSpace(std::uint32_t cp)
{
    return cp == 0x20 || (cp >= 0x09 && cp <= 0x0D) || cp == 0x85 || cp == 0xA0 || cp == 0x1680
            || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F
            || cp == 0x205F || cp == 0x3000;
}

// Simple (1:1) case folding for the ranges the library actually cares about: ASCII,
// Latin-1 supplement, Greek and Cyrillic. Full Unicode SpecialCasing (the German sharp s,
// final sigma context, Turkish dotted I, ...) is deliberately not attempted -- it needs
// locale and multi-character expansions, which a value type must not guess at.
inline std::uint32_t unicodeToLower(std::uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 32;
    }
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) {  // Latin-1 letters, skip the multiply sign
        return cp + 32;
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) {  // Greek capital letters
        return cp + 32;
    }
    if (cp >= 0x410 && cp <= 0x42F) {  // Cyrillic А..Я
        return cp + 32;
    }
    if (cp >= 0x400 && cp <= 0x40F) {  // Cyrillic Ѐ..Џ
        return cp + 80;
    }
    return cp;
}

inline std::uint32_t unicodeToUpper(std::uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z') {
        return cp - 32;
    }
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) {  // Latin-1 letters, skip the division sign
        return cp - 32;
    }
    if (cp == 0x3C2 || cp == 0x3C3) {  // final/regular sigma -> capital sigma
        return 0x3A3;
    }
    if (cp >= 0x3B1 && cp <= 0x3C1) {  // Greek small alpha..rho
        return cp - 32;
    }
    if (cp >= 0x3C4 && cp <= 0x3C9) {  // Greek small tau..omega
        return cp - 32;
    }
    if (cp >= 0x430 && cp <= 0x44F) {  // Cyrillic а..я
        return cp - 32;
    }
    if (cp >= 0x450 && cp <= 0x45F) {  // Cyrillic ѐ..џ
        return cp - 80;
    }
    return cp;
}

// Base accepted by toLongLong()/toULongLong(): 0 means "auto-detect", otherwise 2..36.
// Reject anything else before calling strtoll()/strtoull(), whose behavior on an
// out-of-range base is undefined by the C standard.
inline bool isStringParseBaseOk(int base)
{
    return base == 0 || (base >= 2 && base <= 36);
}

// Case-folded code point comparison returning -1/0/1. Folding is 1:1 (see unicodeToLower),
// so it neither allocates nor changes any code point's byte length, and a folded match
// consumes exactly the needle's code points. Case-sensitive ordering is deliberately not
// handled here: it belongs to the operators (see String::compare), so there is one
// definition of the ordering rather than two.
inline int compareFolded(const char *a, std::uint32_t ab, const char *b, std::uint32_t bb)
{
    std::uint32_t ia = 0;
    std::uint32_t ib = 0;
    while (ia < ab && ib < bb) {
        std::uint32_t cpa = 0;
        std::uint32_t cpb = 0;
        ia += utf8Decode(a + ia, ab - ia, &cpa);
        ib += utf8Decode(b + ib, bb - ib, &cpb);
        cpa = unicodeToLower(cpa);
        cpb = unicodeToLower(cpb);
        if (cpa != cpb) {
            return cpa < cpb ? -1 : 1;
        }
    }
    if (ia == ab && ib == bb) {
        return 0;
    }
    return ia == ab ? -1 : 1;
}

// Try to match `needle` case-insensitively at hay[at]; on success *endByte is set to one
// past the matched bytes. The empty needle matches trivially (at == *endByte).
inline bool foldedMatchAt(const char *hay, std::uint32_t hayBytes, std::uint32_t at,
                          const char *needle, std::uint32_t needleBytes, std::uint32_t *endByte)
{
    std::uint32_t hi = at;
    std::uint32_t ni = 0;
    while (ni < needleBytes) {
        if (hi >= hayBytes) {
            return false;
        }
        std::uint32_t hcp = 0;
        std::uint32_t ncp = 0;
        hi += utf8Decode(hay + hi, hayBytes - hi, &hcp);
        ni += utf8Decode(needle + ni, needleBytes - ni, &ncp);
        if (unicodeToLower(hcp) != unicodeToLower(ncp)) {
            return false;
        }
    }
    *endByte = hi;
    return true;
}

// Byte offset of the first case-insensitive occurrence of `needle`, or `hayBytes` when it
// is not found. Callers must handle the empty needle before calling (an empty hay would
// otherwise make "not found" indistinguishable from a match at 0).
inline std::uint32_t foldedIndexOf(const char *hay, std::uint32_t hayBytes,
                                   const char *needle, std::uint32_t needleBytes)
{
    std::uint32_t i = 0;
    std::uint32_t end = 0;
    while (i < hayBytes) {
        if (foldedMatchAt(hay, hayBytes, i, needle, needleBytes, &end)) {
            return i;
        }
        std::uint32_t cp = 0;
        i += utf8Decode(hay + i, hayBytes - i, &cp);
    }
    return hayBytes;
}

}  // namespace detail

class String : public detail::BasicCharBlock<detail::StringTag>
{
    typedef detail::BasicCharBlock<detail::StringTag> Base;

public:
    enum
    {
        // "Not found", returned by indexOf()/lastIndexOf(). No code point can live at
        // index 0xffffffff: a block's byte length -- and therefore its code point count
        // and largest valid index + 1 -- is bounded by MaxLength. Mirrors npos in
        // std::string and Qt.
        npos = 0xffffffffu,
    };

    // The base owns the storage and the generic construction forms; inheriting its
    // constructors keeps (const char*, len), std::string and const char* available.
    using Base::Base;
    String() = default;

    // Converting constructors from the base type. They are what let a slice (which the base
    // returns as the base type) stay a String, and what makes String::adoptBlock() and
    // StringBuilder::takeString() produce a String rather than a bare block.
    String(const Base &other)
        : Base(other)
    {
    }
    String(Base &&other)
        : Base(std::move(other))
    {
    }

public:
    // -- length and observation (code point semantics) --
    //
    // size()/length()/count() count UTF-8 code points and deliberately hide the base's
    // byte size(). byteSize() is the byte count for data()/toUtf8String()/serialization.
    std::uint32_t size() const;
    std::uint32_t length() const { return size(); }
    std::uint32_t count() const { return size(); }
    std::uint32_t byteSize() const { return Base::size(); }
    // QString::constData() spelling. Note it is not NUL-terminated.
    const char *constData() const { return data(); }

public:
    // -- read-only element access (code point semantics) --
    //
    // The result is a code point value, not a reference, because the type is immutable:
    // there is no writable operator[] (use StringBuilder to build). Out-of-range does not
    // go out of bounds -- it returns 0, the same "well-defined sentinel" choice ByteArray
    // makes with '\0'. For a single-character substring use mid(i, 1).
    std::uint32_t at(std::uint32_t i) const;
    std::uint32_t operator[](std::uint32_t i) const { return at(i); }
    std::uint32_t front() const { return at(0); }  // at(0) already yields 0 when empty
    std::uint32_t back() const { return isEmpty() ? 0u : at(size() - 1); }

public:
    // -- slicing (offsets and lengths in code points) --
    //
    // The base already clamps a too-long length and returns empty for an out-of-range
    // offset, and long slices still share the parent block (see string_data.h).
    String mid(std::uint32_t offset, std::uint32_t len = npos) const;
    String left(std::uint32_t len) const { return mid(0, len); }
    String right(std::uint32_t len) const;
    String sliced(std::uint32_t pos, std::uint32_t len = npos) const { return mid(pos, len); }
    String chopped(std::uint32_t n) const;

public:
    // -- substring search (indices in code points) --
    //
    // These never detach or allocate: they scan the UTF-8 bytes and translate the byte
    // position back into a code point index. Because UTF-8 is self-synchronizing, a valid
    // needle can only match on a code point boundary, so a byte scan is equivalent to a
    // code point scan. The empty needle matches at `from` (std::string::find semantics).
    std::uint32_t indexOf(const String &needle, std::uint32_t from = 0) const;
    std::uint32_t indexOf(char c, std::uint32_t from = 0) const;
    // Last occurrence at or before `from`; the default (npos) means "search from the end".
    std::uint32_t lastIndexOf(const String &needle, std::uint32_t from = npos) const;
    std::uint32_t lastIndexOf(char c, std::uint32_t from = npos) const;

    bool contains(const String &needle) const { return indexOf(needle) != npos; }
    bool contains(char c) const { return indexOf(c) != npos; }
    // Case-insensitive substring test; the case-sensitive form is the overload above.
    bool contains(const String &needle, CaseSensitivity cs) const;
    bool startsWith(const String &prefix) const;
    bool endsWith(const String &suffix) const;

    // Non-overlapping occurrence count, like QString::count(): "aaaa".count("aa") is 2.
    std::uint32_t count(const String &needle) const;
    std::uint32_t count(char c) const;

public:
    // -- ordering --
    //
    // ==, != and < are inherited from the shared base (byte comparison is also code point
    // order, because UTF-8 preserves code point ordering). Add the rest, derived from < so
    // there is a single ordering definition.
    bool operator>(const String &other) const { return other < *this; }
    bool operator<=(const String &other) const { return !(other < *this); }
    bool operator>=(const String &other) const { return !(*this < other); }

    // Lexicographic comparison returning -1/0/1, like QString::compare(). With
    // CaseInsensitive, ASCII/Latin-1/Greek/Cyrillic letters are folded to lower case first.
    int compare(const String &other, CaseSensitivity cs = CaseSensitive) const;

public:
    // -- transforms (each returns a new String) --
    String trimmed() const;
    String simplified() const;
    String toLower() const;
    String toUpper() const;
    String replaced(const String &before, const String &after) const;
    String replaced(const String &before, const String &after, CaseSensitivity cs) const;
    // This content repeated `times` times. A result longer than MaxLength yields empty
    // rather than wrapping and overrunning.
    String repeated(std::uint32_t times) const;

public:
    // -- split / join / numeric conversion --
    StringList split(char separator) const;
    StringList split(const String &separator) const;

    // One template for every integral type, so number(7u)/number(std::int64_t(7)) are exact
    // matches instead of equally-ranked integral conversions (which would be ambiguous).
    // base is 2..36; an out-of-range base yields an empty string.
    template<typename T>
    static typename std::enable_if<std::is_integral<T>::value, String>::type
    number(T value, int base = 10)
    {
        return numberUnsigned(magnitudeOf(value), isNegative(value), base);
    }

    // Double formatting mirrors QString::number(double, char, int): format is one of
    // 'g'/'G'/'e'/'E'/'f', anything else falls back to 'g'.
    static String number(double value, char format = 'g', int precision = 6);

    int toInt(bool *ok = nullptr, int base = 10) const;
    unsigned int toUInt(bool *ok = nullptr, int base = 10) const;
    long long toLongLong(bool *ok = nullptr, int base = 10) const;
    unsigned long long toULongLong(bool *ok = nullptr, int base = 10) const;
    double toDouble(bool *ok = nullptr) const;
    float toFloat(bool *ok = nullptr) const;

public:
    // -- UTF-8 <-> ByteArray (explicit, zero-copy) --
    //
    // String is stored as UTF-8, so neither of these transcodes: fromUtf8() reinterprets
    // the bytes as text and toUtf8() hands the text back as bytes, both byte-for-byte.
    // No validation is done -- String tolerates malformed UTF-8 (see the top of this
    // file), and an invalid sequence is carried through unchanged. Neither type gains an
    // implicit conversion to the other; callers must spell the conversion out.
    //
    // The conversion is O(1) for content longer than InlineCapacity: the result reuses
    // the same shared block (no bytes copied, both handles report useCount() == 2). Short
    // content is copied inline (no allocation). Sharing is safe because the block is
    // immutable.
    static String fromUtf8(const ByteArray &utf8);
    ByteArray toUtf8() const;

    // The UTF-8 bytes as std::string. String's storage is already UTF-8, so this is a
    // byte-for-byte copy -- never a transcode, and never through size() (which counts
    // code points). byteSize() is the byte length.
    std::string toUtf8String() const { return std::string(data(), byteSize()); }

    // String names the conversion explicitly, so callers say what they mean instead of
    // borrowing the byte block's toStdString() name. Deleting the inherited one also makes
    // every stale call fail to compile (a compiler-enforced rename), and blocks a future
    // String::toStdString() whose size() would silently resolve to code points. ByteArray
    // keeps the inherited toStdString() -- it is bytes already.
    std::string toStdString() const = delete;

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

    static String numberUnsigned(std::uint64_t magnitude, bool negative, int base);
};

// The derived class adds no data members, so it must keep the base's size and alignment
// (the shared layout is pinned in types/private/string_data.h).
static_assert(sizeof(String) == 32, "String must stay 32 bytes");
static_assert(alignof(String) == 8, "String alignment must be 8");

// ---------------------------------------------------------------------------
// StringList: Vector<String> plus the QStringList-specific vocabulary
// ---------------------------------------------------------------------------
//
// The container-generic members (isEmpty / first / last / contains / count / indexOf /
// lastIndexOf / value / takeX / removeX / prepend / insert(i, v) / removeAt / fill / move /
// ...) are inherited from Vector<String>; only what Qt adds on top of QList lives here.
// "Not found" is npos (SizeType(-1)), matching Vector and String/ByteArray, not Qt's int
// -1. Like every other method of a COW handle, the read-only members never detach and the
// mutating ones detach first (through the inherited non-const get()).
class StringList : public Vector<String>
{
    typedef Vector<String> Base;

public:
    typedef Base::SizeType SizeType;

    using Base::Base;
    StringList() = default;
    // QStringList's single-element constructor. Implicit, like Qt's, so
    // `StringList l = someString;` builds a one-element list.
    StringList(const String &value) { push_back(value); }

    // The case-insensitive overloads below add a parameter, which would otherwise hide the
    // inherited case-sensitive overloads of the same name; pull them back in.
    using Base::contains;
    using Base::indexOf;
    using Base::lastIndexOf;

    // Separator between every pair; both forms return empty for an empty list.
    String join(const String &separator) const;
    String join(char separator) const;

    bool contains(const String &value, CaseSensitivity cs) const;
    SizeType indexOf(const String &value, SizeType from, CaseSensitivity cs) const;
    SizeType lastIndexOf(const String &value, SizeType from, CaseSensitivity cs) const;

    // The elements containing `needle`. Read-only, so it does not detach.
    StringList filter(const String &needle, CaseSensitivity cs = CaseSensitive) const;
    // Qt semantics: sorts (case-sensitively) and then removes adjacent duplicates, so the
    // order of the surviving elements is not preserved. Returns the number removed, as a
    // SizeType like the other removal counts in this library (not Qt's int).
    SizeType removeDuplicates();
    void sort(CaseSensitivity cs = CaseSensitive);
    StringList &replaceInStrings(const String &before, const String &after,
                                 CaseSensitivity cs = CaseSensitive);
};

// ---------------------------------------------------------------------------
// Out-of-line implementations
// ---------------------------------------------------------------------------

inline std::uint32_t String::size() const
{
    return detail::utf8Count(data(), Base::size());
}

inline std::uint32_t String::at(std::uint32_t i) const
{
    const std::uint32_t bytes = Base::size();
    // utf8ByteOffset() returns `bytes` once i reaches the code point count, so the bound
    // check falls out of the walked offset -- no separate (and twice as expensive) size().
    const std::uint32_t offset = detail::utf8ByteOffset(data(), bytes, i);
    if (offset >= bytes) {
        return 0;
    }
    std::uint32_t cp = 0;
    detail::utf8Decode(data() + offset, bytes - offset, &cp);
    return cp;
}

inline String String::mid(std::uint32_t offset, std::uint32_t len) const
{
    const std::uint32_t bytes = Base::size();
    const char *p = data();
    // Walk straight to the code point boundaries: utf8ByteOffset() clamps to the byte
    // length by itself, so a too-large offset yields an empty range and a too-long len
    // runs to the end. Passing npos as len is therefore the same as "to the end", and the
    // min/npos arithmetic the old version needed is gone.
    const std::uint32_t beginByte = detail::utf8ByteOffset(p, bytes, offset);
    if (beginByte >= bytes) {
        return String();
    }
    const std::uint32_t spanBytes = detail::utf8ByteOffset(p + beginByte, bytes - beginByte, len);
    return String(Base::mid(beginByte, spanBytes));
}

inline String String::right(std::uint32_t len) const
{
    const std::uint32_t n = size();
    return len >= n ? *this : mid(n - len, len);
}

inline String String::chopped(std::uint32_t n) const
{
    const std::uint32_t total = size();
    return n >= total ? String() : mid(0, total - n);
}

inline std::uint32_t String::indexOf(const String &needle, std::uint32_t from) const
{
    const std::uint32_t n = size();
    if (from > n) {
        return npos;
    }
    const std::uint32_t m = needle.size();
    if (m == 0) {
        return from;
    }
    const std::uint32_t bytes = Base::size();
    const std::uint32_t nb = needle.byteSize();
    const std::uint32_t fromByte = detail::utf8ByteOffset(data(), bytes, from);
    if (nb > bytes - fromByte) {
        return npos;
    }
    const char *h = data();
    const char *nd = needle.data();
    const std::uint32_t last = bytes - nb;
    for (std::uint32_t i = fromByte; i <= last; ++i) {
        // Compare the first byte inline; memcmp only when it matches, so a scan over
        // non-matching data stays one byte of work per position.
        if (h[i] == nd[0] && (nb == 1 || std::memcmp(h + i, nd, nb) == 0)) {
            return detail::utf8Count(h, i);
        }
    }
    return npos;
}

inline std::uint32_t String::indexOf(char c, std::uint32_t from) const
{
    const std::uint32_t n = size();
    if (from > n) {
        return npos;
    }
    const std::uint32_t bytes = Base::size();
    const std::uint32_t fromByte = detail::utf8ByteOffset(data(), bytes, from);
    const void *p = std::memchr(data() + fromByte, static_cast<unsigned char>(c), bytes - fromByte);
    return p == nullptr ? npos
                        : detail::utf8Count(data(), static_cast<std::uint32_t>(static_cast<const char *>(p) - data()));
}

inline std::uint32_t String::lastIndexOf(const String &needle, std::uint32_t from) const
{
    const std::uint32_t n = size();
    const std::uint32_t m = needle.size();
    if (m > n) {
        return npos;
    }
    // Clamp the code point `from` into [0, n - m]; npos means "start from the end".
    const std::uint32_t count = (from == npos || from > n - m) ? (n - m) : from;
    if (m == 0) {
        return count;
    }
    const std::uint32_t bytes = Base::size();
    const std::uint32_t nb = needle.byteSize();
    const std::uint32_t lastByte = detail::utf8ByteOffset(data(), bytes, count);
    const char *h = data();
    const char *nd = needle.data();
    // Down-count loop: byte 0 must be tested, so "i-- > 0" rather than "i >= 0" (which
    // would be an infinite loop on unsigned).
    for (std::uint32_t i = lastByte + 1; i-- > 0;) {
        if (i + nb <= bytes && h[i] == nd[0] && (nb == 1 || std::memcmp(h + i, nd, nb) == 0)) {
            return detail::utf8Count(h, i);
        }
    }
    return npos;
}

inline std::uint32_t String::lastIndexOf(char c, std::uint32_t from) const
{
    const std::uint32_t n = size();
    if (n == 0) {
        return npos;
    }
    const std::uint32_t count = (from == npos || from >= n) ? n - 1 : from;
    const std::uint32_t lastByte = detail::utf8ByteOffset(data(), Base::size(), count);
    const char *h = data();
    for (std::uint32_t i = lastByte + 1; i-- > 0;) {
        if (h[i] == c) {
            return detail::utf8Count(h, i);
        }
    }
    return npos;
}

inline bool String::startsWith(const String &prefix) const
{
    const std::uint32_t m = prefix.byteSize();
    return m <= Base::size() && (m == 0 || std::memcmp(data(), prefix.data(), m) == 0);
}

inline bool String::endsWith(const String &suffix) const
{
    const std::uint32_t n = Base::size();
    const std::uint32_t m = suffix.byteSize();
    return m <= n && (m == 0 || std::memcmp(data() + (n - m), suffix.data(), m) == 0);
}

inline int String::compare(const String &other, CaseSensitivity cs) const
{
    if (cs == CaseSensitive) {
        // Byte comparison is code point order (UTF-8 preserves it), and deriving from the
        // existing operators keeps a single definition of the ordering.
        if (*this < other) {
            return -1;
        }
        return other < *this ? 1 : 0;
    }
    return detail::compareFolded(data(), byteSize(), other.data(), other.byteSize());
}

inline bool String::contains(const String &needle, CaseSensitivity cs) const
{
    if (cs == CaseSensitive) {
        return contains(needle);
    }
    // An empty needle is always contained; handling it up front also keeps the
    // "not found == byteSize()" sentinel from being mistaken for a match at 0 in an empty
    // haystack.
    if (needle.isEmpty()) {
        return true;
    }
    const std::uint32_t bytes = byteSize();
    return detail::foldedIndexOf(data(), bytes, needle.data(), needle.byteSize()) != bytes;
}

inline std::uint32_t String::count(const String &needle) const
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

inline std::uint32_t String::count(char c) const
{
    const char *h = data();
    const std::uint32_t bytes = Base::size();
    std::uint32_t occurrences = 0;
    for (std::uint32_t i = 0; i < bytes; ++i) {
        if (h[i] == c) {
            ++occurrences;
        }
    }
    // A byte < 0x80 cannot appear inside a multi-byte sequence, so this equals the code
    // point count. A separator byte >= 0x80 is ambiguous and left as a raw byte match.
    return occurrences;
}

inline String String::trimmed() const
{
    const char *p = data();
    const std::uint32_t bytes = Base::size();
    std::uint32_t firstNonSpace = npos;
    std::uint32_t lastNonSpaceEnd = 0;  // code point index one past the last non-space
    std::uint32_t i = 0;
    std::uint32_t index = 0;
    std::uint32_t cp = 0;
    while (i < bytes) {
        const std::uint32_t adv = detail::utf8Decode(p + i, bytes - i, &cp);
        if (!detail::isUnicodeSpace(cp)) {
            if (firstNonSpace == npos) {
                firstNonSpace = index;
            }
            lastNonSpaceEnd = index + 1;
        }
        i += adv;
        ++index;
    }
    if (firstNonSpace == npos) {
        return String();
    }
    // Go through mid() so a long trimmed result shares the parent block instead of copying.
    return mid(firstNonSpace, lastNonSpaceEnd - firstNonSpace);
}

inline String String::simplified() const
{
    const char *p = data();
    const std::uint32_t bytes = Base::size();
    std::string out;
    out.reserve(bytes);
    bool wroteAny = false;
    bool pendingSpace = false;
    std::uint32_t i = 0;
    std::uint32_t cp = 0;
    while (i < bytes) {
        const std::uint32_t adv = detail::utf8Decode(p + i, bytes - i, &cp);
        if (detail::isUnicodeSpace(cp)) {
            // Leading whitespace is dropped; an internal run collapses to one space.
            if (wroteAny) {
                pendingSpace = true;
            }
        } else {
            if (pendingSpace) {
                out.push_back(' ');
                pendingSpace = false;
            }
            out.append(p + i, adv);
            wroteAny = true;
        }
        i += adv;
    }
    return String(out);
}

inline String String::toLower() const
{
    const char *p = data();
    const std::uint32_t bytes = Base::size();
    std::string out;
    out.reserve(bytes);
    std::uint32_t i = 0;
    std::uint32_t cp = 0;
    while (i < bytes) {
        const std::uint32_t adv = detail::utf8Decode(p + i, bytes - i, &cp);
        const std::uint32_t mapped = detail::unicodeToLower(cp);
        if (mapped == cp) {
            out.append(p + i, adv);  // unchanged bytes, no re-encode
        } else {
            detail::utf8Encode(mapped, out);
        }
        i += adv;
    }
    return String(out);
}

inline String String::toUpper() const
{
    const char *p = data();
    const std::uint32_t bytes = Base::size();
    std::string out;
    out.reserve(bytes);
    std::uint32_t i = 0;
    std::uint32_t cp = 0;
    while (i < bytes) {
        const std::uint32_t adv = detail::utf8Decode(p + i, bytes - i, &cp);
        const std::uint32_t mapped = detail::unicodeToUpper(cp);
        if (mapped == cp) {
            out.append(p + i, adv);
        } else {
            detail::utf8Encode(mapped, out);
        }
        i += adv;
    }
    return String(out);
}

inline String String::replaced(const String &before, const String &after) const
{
    if (before.isEmpty()) {
        return *this;  // QString also treats an empty needle as "nothing to replace"
    }
    const std::uint32_t bytes = Base::size();
    const char *p = data();
    std::string out;
    out.reserve(bytes);
    std::uint32_t pos = 0;  // code point cursor in the source
    while (true) {
        const std::uint32_t found = indexOf(before, pos);
        if (found == npos) {
            break;
        }
        const std::uint32_t startByte = detail::utf8ByteOffset(p, bytes, pos);
        const std::uint32_t foundByte = detail::utf8ByteOffset(p, bytes, found);
        out.append(p + startByte, foundByte - startByte);
        out.append(after.data(), after.byteSize());
        pos = found + before.size();
    }
    const std::uint32_t tailByte = detail::utf8ByteOffset(p, bytes, pos);
    out.append(p + tailByte, bytes - tailByte);
    return String(out);
}

inline String String::replaced(const String &before, const String &after, CaseSensitivity cs) const
{
    if (cs == CaseSensitive) {
        return replaced(before, after);
    }
    if (before.isEmpty()) {
        return *this;
    }
    const char *p = data();
    const std::uint32_t bytes = byteSize();
    const char *np = before.data();
    const std::uint32_t nb = before.byteSize();
    std::string out;
    out.reserve(bytes);
    std::uint32_t last = 0;
    std::uint32_t i = 0;
    while (i < bytes) {
        std::uint32_t end = 0;
        if (detail::foldedMatchAt(p, bytes, i, np, nb, &end)) {
            out.append(p + last, i - last);
            out.append(after.data(), after.byteSize());
            i = end;
            last = end;
        } else {
            std::uint32_t cp = 0;
            i += detail::utf8Decode(p + i, bytes - i, &cp);
        }
    }
    out.append(p + last, bytes - last);
    return String(out);
}

inline String String::repeated(std::uint32_t times) const
{
    if (times == 0 || isEmpty()) {
        return String();
    }
    if (times == 1) {
        return *this;
    }
    const std::uint32_t n = Base::size();
    const std::uint64_t total = static_cast<std::uint64_t>(n) * times;
    if (total > static_cast<std::uint64_t>(Base::MaxLength)) {
        // Refuse rather than wrap: a string longer than MaxLength cannot be represented, and
        // a wrapped length would make size() lie while the buffer holds the real bytes.
        return String();
    }
    std::string out(static_cast<std::size_t>(total), '\0');
    char *dst = &out[0];
    for (std::uint32_t i = 0; i < times; ++i) {
        std::memcpy(dst, data(), n);
        dst += n;
    }
    return String(out);
}

inline StringList String::split(char separator) const
{
    // Splitting on an ASCII byte cannot land inside a multi-byte sequence (continuation
    // bytes are >= 0x80), so every part is a valid code point slice. An empty string yields
    // a single empty part, as QString does.
    StringList parts;
    const char *h = data();
    const std::uint32_t bytes = Base::size();
    std::uint32_t start = 0;
    for (std::uint32_t i = 0; i < bytes; ++i) {
        if (h[i] == separator) {
            parts.push_back(String(h + start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(String(h + start, bytes - start));
    return parts;
}

inline StringList String::split(const String &separator) const
{
    StringList parts;
    if (separator.isEmpty()) {
        parts.push_back(*this);
        return parts;
    }
    const char *h = data();
    const char *sep = separator.data();
    const std::uint32_t bytes = Base::size();
    const std::uint32_t sb = separator.byteSize();
    std::uint32_t start = 0;
    std::uint32_t i = 0;
    while (i + sb <= bytes) {
        if (h[i] == sep[0] && (sb == 1 || std::memcmp(h + i, sep, sb) == 0)) {
            parts.push_back(String(h + start, i - start));
            i += sb;
            start = i;
        } else {
            ++i;
        }
    }
    parts.push_back(String(h + start, bytes - start));
    return parts;
}

inline String StringList::join(const String &separator) const
{
    const StlType &c = get();
    if (c.empty()) {
        return String();
    }
    std::string out;
    for (SizeType i = 0; i < c.size(); ++i) {
        if (i > 0) {
            out.append(separator.data(), separator.byteSize());
        }
        out.append(c[i].data(), c[i].byteSize());
    }
    return String(out);
}

inline String StringList::join(char separator) const
{
    // A one-character separator is just a one-byte String; delegating keeps a single join
    // loop instead of two copy-paste twins.
    return join(String(&separator, 1));
}

inline bool StringList::contains(const String &value, CaseSensitivity cs) const
{
    if (cs == CaseSensitive) {
        return Base::contains(value);
    }
    const StlType &c = get();
    for (SizeType i = 0; i < c.size(); ++i) {
        if (c[i].compare(value, cs) == 0) {
            return true;
        }
    }
    return false;
}

inline StringList::SizeType StringList::indexOf(const String &value, SizeType from, CaseSensitivity cs) const
{
    const StlType &c = get();
    if (cs == CaseSensitive) {
        return detail::seqIndexOf(c, from, value);
    }
    for (SizeType i = from; i < c.size(); ++i) {
        if (c[i].compare(value, cs) == 0) {
            return i;
        }
    }
    return npos;
}

inline StringList::SizeType StringList::lastIndexOf(const String &value, SizeType from, CaseSensitivity cs) const
{
    const StlType &c = get();
    if (cs == CaseSensitive) {
        return detail::seqLastIndexOf(c, from, value);
    }
    if (c.empty()) {
        return npos;
    }
    // from == npos (>= size) means "start from the end"; anything past the end clamps the
    // same way, so callers can pass npos or size() without special-casing.
    const SizeType start = (from >= c.size()) ? c.size() - 1 : from;
    for (SizeType i = start + 1; i-- > 0;) {
        if (c[i].compare(value, cs) == 0) {
            return i;
        }
    }
    return npos;
}

inline StringList StringList::filter(const String &needle, CaseSensitivity cs) const
{
    StringList out;
    const StlType &c = get();
    for (SizeType i = 0; i < c.size(); ++i) {
        if (c[i].contains(needle, cs)) {
            out.push_back(c[i]);
        }
    }
    return out;
}

inline StringList::SizeType StringList::removeDuplicates()
{
    StlType &c = Base::get();
    std::sort(c.begin(), c.end());
    const SizeType before = c.size();
    c.erase(std::unique(c.begin(), c.end()), c.end());
    return before - c.size();
}

inline void StringList::sort(CaseSensitivity cs)
{
    StlType &c = Base::get();
    if (cs == CaseSensitive) {
        std::sort(c.begin(), c.end());
    } else {
        std::sort(c.begin(), c.end(), [cs](const String &a, const String &b) {
            return a.compare(b, cs) < 0;
        });
    }
}

inline StringList &StringList::replaceInStrings(const String &before, const String &after, CaseSensitivity cs)
{
    StlType &c = Base::get();
    for (SizeType i = 0; i < c.size(); ++i) {
        c[i] = c[i].replaced(before, after, cs);
    }
    return *this;
}

inline String String::numberUnsigned(std::uint64_t magnitude, bool negative, int base)
{
    if (base < 2 || base > 36) {
        return String();
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
    return String(p, static_cast<std::uint32_t>(end - p));
}

inline String String::number(double value, char format, int precision)
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
        return String();
    }
    if (static_cast<std::size_t>(n) < sizeof(buf)) {
        return String(buf, static_cast<std::uint32_t>(n));
    }
    // Rare: a long %f needs more room. Format again into an exact-size buffer.
    std::string big(static_cast<std::size_t>(n) + 1, '\0');
    std::snprintf(&big[0], big.size(), fmt, value);
    big.resize(static_cast<std::size_t>(n));
    return String(big);
}

inline int String::toInt(bool *ok, int base) const
{
    bool parsed = false;
    const long long value = toLongLong(&parsed, base);
    const bool inRange = parsed && value >= INT_MIN && value <= INT_MAX;
    if (ok != nullptr) {
        *ok = inRange;
    }
    return inRange ? static_cast<int>(value) : 0;
}

inline unsigned int String::toUInt(bool *ok, int base) const
{
    bool parsed = false;
    const unsigned long long value = toULongLong(&parsed, base);
    const bool inRange = parsed && value <= UINT_MAX;
    if (ok != nullptr) {
        *ok = inRange;
    }
    return inRange ? static_cast<unsigned int>(value) : 0;
}

inline long long String::toLongLong(bool *ok, int base) const
{
    if (!detail::isStringParseBaseOk(base)) {
        if (ok != nullptr) {
            *ok = false;
        }
        return 0;
    }
    const std::string s = toUtf8String();
    char *end = nullptr;
    errno = 0;
    const long long value = std::strtoll(s.c_str(), &end, base);
    const bool success = end != s.c_str() && errno == 0;
    if (ok != nullptr) {
        *ok = success;
    }
    return success ? value : 0;
}

inline unsigned long long String::toULongLong(bool *ok, int base) const
{
    if (!detail::isStringParseBaseOk(base)) {
        if (ok != nullptr) {
            *ok = false;
        }
        return 0;
    }
    const std::string s = toUtf8String();
    // strtoull() wraps a leading '-' into a huge number; a negative value is a failed
    // unsigned conversion, matching QString::toULongLong().
    const char *p = s.c_str();
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') {
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

inline double String::toDouble(bool *ok) const
{
    const std::string s = toUtf8String();
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(s.c_str(), &end);
    const bool success = end != s.c_str() && errno == 0;
    if (ok != nullptr) {
        *ok = success;
    }
    return success ? value : 0.0;
}

inline float String::toFloat(bool *ok) const
{
    return static_cast<float>(toDouble(ok));
}

inline String String::fromUtf8(const ByteArray &utf8)
{
    return String(utf8.shareStorageAs<detail::StringTag>());
}

inline ByteArray String::toUtf8() const
{
    return ByteArray(shareStorageAs<detail::ByteArrayTag>());
}

namespace detail {

// ---------------------------------------------------------------------------
// Concatenation expression: let `a + b + c + d` allocate only once
// ---------------------------------------------------------------------------
//
// `a + b` does not materialize into a String right away; it returns a lightweight node
// that records what is to be concatenated. The outermost level computes the total length
// once, allocates once, and fills the pieces in turn. So joining N pieces takes one
// allocation (zero for a short result, which lands in the inline area). The price is that
// the type of `a + b` is no longer String -- hence the forwarding members below, which
// keep code such as `(a + b).toUtf8String()` working (Qt's QStringBuilder does the same).
//
// Nodes hold their operands by value. Copying a String is cheap (an inline 32-byte memcpy,
// or one atomic incref), so `auto x = a + b;` does not produce a dangling reference.

template<typename A, typename B>
class StringConcat;

template<typename T>
struct IsStringConcat : std::false_type
{
};
template<typename A, typename B>
struct IsStringConcat<StringConcat<A, B> > : std::true_type
{
};

// The expression keeps two running totals: the byte length (for allocation and the
// MaxLength guard) and the code point count (because String::size() counts code points).
// Tracking bytes separately also means the overflow guard can short-circuit before any
// code point count is attempted -- which matters for the adversarial "block with a huge
// nominal length" case in the tests, where decoding would read past the payload.
inline std::uint64_t concatByteSizeOf(const String &s)
{
    return s.byteSize();
}
inline std::uint64_t concatCharCountOf(const String &s)
{
    return s.size();
}

inline char *concatCopyTo(const String &s, char *dst)
{
    if (s.byteSize() != 0) {
        std::memcpy(dst, s.data(), s.byteSize());
    }
    return dst + s.byteSize();
}

// An operand is either a String (or anything implicitly convertible to String, such as a
// literal or std::string) or another expression layer. The former is normalized to String;
// the latter is kept as-is.
template<typename T>
struct IsConcatOperand
    : std::integral_constant<bool, IsStringConcat<T>::value
                                           || std::is_convertible<typename std::decay<const T &>::type, String>::value>
{
};

template<typename T, bool = IsStringConcat<T>::value>
struct ConcatOperand
{
    typedef String Type;
    // A leaf operand (String / literal / std::string) has to be stored in the node anyway.
    // Take the parameter by value and move it into String: operator+'s parameters are
    // already copies, so writing `String(t)` here would copy **again** -- measured at two
    // cowIncrefs per String leaf. By-value plus move makes a String leaf use the move
    // constructor instead, paying only once.
    static Type normalize(T t) { return String(std::move(t)); }
};
template<typename T>
struct ConcatOperand<T, true>
{
    typedef T Type;
    // Take the parameter by value and return by value: the caller passes a temporary
    // expression (an rvalue), so the whole path is moves and nothing is copied. Writing
    // normalize(const T &) earlier made every layer copy the entire subtree; measured at
    // 10 node copies for a+b+c+d+e+f (a triangular number, O(N^2)), and 0 after switching
    // to by-value.
    static Type normalize(T t) { return t; }
};

template<typename A, typename B>
class StringConcat
{
public:
    StringConcat(A a, B b)
        : a_(std::move(a))
        , b_(std::move(b))
    {
    }

    // -- Forwarding members: keep (a + b).xxx() working --
    //
    // Only forward the ones with a definite answer: size()/byteSize()/isEmpty() are O(1)
    // (well, size() scans the operands, not the result) and agree with the materialized
    // result, and toUtf8String() plus the comparison operators are "materialize, then
    // convert/compare".
    //
    // Deliberately **not** forwarded:
    //   - data()/c_str(): an expression has no contiguous storage, so the returned pointer
    //     would necessarily point at a temporary -- i.e. dangle.
    //   - useCount(): an expression has no shared block at all, and the temporary
    //     materialized from it always has a reference count of 1. That number carries no
    //     information, so forwarding it would hand the caller a false answer. To ask about
    //     the reference count, first do `String s = a + b;` and ask s.
    //   - also no `operator std::string()`: it would make `std::string s = a + b;` compile,
    //     but it would turn mixed overload sets such as `void f(const String&)` /
    //     `void f(const std::string&)` into **ambiguous calls** (one conversion each, same
    //     rank). Better to omit one convenience conversion than to plant ambiguity on the
    //     caller. Use `(a + b).toUtf8String()` when you need a std::string.
    std::uint32_t size() const
    {
        const std::uint64_t bytes = byteSize64();
        if (bytes > String::MaxLength) {
            return 0;  // materialization yields an empty string; report the same
        }
        return static_cast<std::uint32_t>(charCount64());
    }
    std::uint32_t byteSize() const
    {
        const std::uint64_t bytes = byteSize64();
        return bytes > String::MaxLength ? 0u : static_cast<std::uint32_t>(bytes);
    }
    bool isEmpty() const
    {
        // Empty both when there is nothing to copy and when the result is too long to
        // represent (materialization yields an empty string). Checking bytes first avoids
        // decoding operands that must not be decoded.
        const std::uint64_t bytes = byteSize64();
        return bytes == 0 || bytes > String::MaxLength;
    }
    std::string toUtf8String() const { return static_cast<String>(*this).toUtf8String(); }

    // Materialize with a single allocation. When the total length exceeds what String can
    // represent it returns an empty string -- the same handling as the unreachable branch
    // in operator+: there is no error channel here, and an empty string beats going out of
    // bounds.
    operator String() const
    {
        const std::uint64_t total = byteSize64();
        if (total > String::MaxLength) {
            return String();
        }
        const std::uint32_t n = static_cast<std::uint32_t>(total);
        if (n <= static_cast<std::uint32_t>(String::InlineCapacity)) {
            char buf[String::InlineCapacity];
            copyTo(buf);
            return String(buf, n);
        }
        String::Data *d = String::allocateBlock(n);
        copyTo(d->buf);
        return String::adoptBlock(d, n);
    }

    bool operator==(const String &other) const { return static_cast<String>(*this) == other; }
    bool operator!=(const String &other) const { return !(*this == other); }
    bool operator<(const String &other) const { return static_cast<String>(*this) < other; }

    // -- detail-internal interface --
    //
    // These exist for detail's concatByteSizeOf / concatCopyTo (non-member functions, which
    // cannot reach private members). They serve one purpose, so they sit together and share
    // this note instead of each carrying a scattered comment. Callers are not supposed to
    // invoke them directly.
    std::uint64_t byteSize64() const { return concatByteSizeOf(a_) + concatByteSizeOf(b_); }
    std::uint64_t charCount64() const { return concatCharCountOf(a_) + concatCharCountOf(b_); }

    char *copyTo(char *dst) const
    {
        dst = concatCopyTo(a_, dst);
        return concatCopyTo(b_, dst);
    }

private:
    A a_;
    B b_;
};

template<typename A, typename B>
inline std::uint64_t concatByteSizeOf(const StringConcat<A, B> &c)
{
    return c.byteSize64();
}

template<typename A, typename B>
inline std::uint64_t concatCharCountOf(const StringConcat<A, B> &c)
{
    return c.charCount64();
}

template<typename A, typename B>
inline char *concatCopyTo(const StringConcat<A, B> &c, char *dst)
{
    return c.copyTo(dst);
}

// Two pieces concatenate into an expression (which can take part in further +), and
// materialize in one shot when a String is needed.
//
// Operands are taken by value: a leaf (String/literal) has to be stored anyway, and it is
// copied at the call site; an intermediate result is a temporary expression, so taking it
// by value lets it move all the way down without copying the subtree at every layer (see
// the notes on ConcatOperand).
//
// It is defined in namespace detail, next to the type it operates on, so argument-dependent
// lookup finds it for a plain `a + b` even without `using namespace qtng`. Defining it in
// namespace qtng instead left it unreachable there: the operands are detail::BasicCharBlock,
// whose associated namespace is detail, not qtng.
template<typename A, typename B>
inline typename std::enable_if<IsConcatOperand<A>::value && IsConcatOperand<B>::value,
                               StringConcat<typename ConcatOperand<A>::Type,
                                            typename ConcatOperand<B>::Type> >::type
operator+(A a, B b)
{
    typedef StringConcat<typename ConcatOperand<A>::Type, typename ConcatOperand<B>::Type> Result;
    return Result(ConcatOperand<A>::normalize(std::move(a)), ConcatOperand<B>::normalize(std::move(b)));
}

}  // namespace detail

// ---------------------------------------------------------------------------
// MsgPack: String is text, so it streams as msgpack str (never bin)
// ---------------------------------------------------------------------------
//
// This is the text-side counterpart of the removed MsgPackStream::operator<<(std::string):
// a bare std::string could mean either str or bin, but String is unambiguously text. The
// bytes on the wire are byteSize(), not size() -- size() counts UTF-8 code points, and the
// content may contain '\0', so passing (data(), byteSize()) is what keeps embedded NULs and
// non-ASCII intact.
inline MsgPackStream &operator<<(MsgPackStream &s, const String &str)
{
    s.writeString(str.data(), str.byteSize());
    return s;
}

inline MsgPackStream &operator>>(MsgPackStream &s, String &str)
{
    std::string tmp;
    if (s.readString(tmp)) {
        str = String(tmp);
    }
    // On failure readString() leaves tmp untouched and sets ReadCorruptData; leave the
    // caller's String alone rather than clobbering it with a half-read value.
    return s;
}

}  // namespace qtng

#endif  // QTNG_TYPES_STRING_H
