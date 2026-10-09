#ifndef QTNG_TYPES_STRING_BUILDER_H
#define QTNG_TYPES_STRING_BUILDER_H

// qtng::StringBuilder -- for building strings.
//
// String is immutable, so building one needs a mutable staging area. The builder
// accumulates directly in a shared block (growing by capacity when needed), so
// takeString() is **zero-copy** for a long result: it hands the already-filled shared block
// to String without copying it again.
//
// When the result length is <= 28 it is copied back into String's inline storage and the
// block is freed, preserving the "heap <=> len > 28" invariant (see
// types/private/string_data.h).

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

#include "qtng/types/bytes.h"
#include "qtng/types/string.h"

namespace qtng {

class StringBuilder
{
public:
    StringBuilder();
    ~StringBuilder();

    StringBuilder &append(const char *bytes, std::uint32_t len);
    StringBuilder &append(const String &s);
    StringBuilder &append(const ByteArray &b);
    StringBuilder &append(const std::string &s);
    StringBuilder &append(const char *cstr);
    StringBuilder &append(char c);

    // Every integer goes through this single template. Writing one overload per width
    // (char / int64_t / uint64_t) would make `sb.append(7)` ambiguous, because all three
    // would need an equal-rank "integer conversion" -- and a string builder that cannot
    // append an int is indefensible. The template is an exact match for any integer, so the
    // special case disappears, and is_signed dispatches unsigned / signed without another
    // overload each.
    //
    // Excluding char is redundant for overload resolution (the non-template append(char) is
    // itself an exact match and wins), but it is a **deliberate safety net**: if someone
    // deletes append(char) later, this exclusion makes it "does not compile" rather than
    // "'A' quietly becomes 65". Better a compile error than a silent wrong digit.
    template<typename T>
    typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, char>::value, StringBuilder &>::type
    append(T v)
    {
        // snprintf into a stack buffer: C++11 has no std::to_chars (that is C++17), and an
        // integer does not deserve another allocation just to be written out.
        char buf[32];
        const int n = std::is_signed<T>::value
                             ? std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v))
                             : std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
        return n > 0 ? append(buf, static_cast<std::uint32_t>(n)) : *this;
    }

    std::uint32_t size() const { return len; }
    bool isEmpty() const { return len == 0; }
    const char *data() const { return d->buf; }

    void clear() { len = 0; }

    // Produce a copy (the builder stays usable). A short result lands inline; a long result
    // is copied into a new shared block.
    String toString() const;
    ByteArray toByteArray() const;

    // finalize: hand the buffer straight to the result type (zero-copy for a long result),
    // then the builder is cleared.
    String takeString() { return takeAs<String>(); }
    ByteArray takeByteArray() { return takeAs<ByteArray>(); }

private:
    StringBuilder(const StringBuilder &);
    StringBuilder &operator=(const StringBuilder &);

    void ensure(std::uint32_t needed);

    // String and ByteArray share the same layout, so the hand-off logic is written once.
    template<typename Block>
    Block takeAs()
    {
        if (len <= static_cast<std::uint32_t>(String::InlineCapacity)) {
            // A short result must be copied back inline, or "short content + heap block"
            // would break the discriminator invariant.
            Block shortResult(d->buf, len);
            len = 0;
            return shortResult;
        }
        String::Data *handed = d;
        const std::uint32_t handedLen = len;
        // The handed-off block already has reference count 1; the result type takes it over.
        d = String::allocateBlock(0);
        len = 0;
        capacity = 1;
        return Block::adoptBlock(handed, handedLen);
    }

private:
    String::Data *d;
    std::uint32_t len;
    std::uint32_t capacity;
};

}  // namespace qtng

#endif  // QTNG_TYPES_STRING_BUILDER_H
