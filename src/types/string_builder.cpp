#include <cstring>

#include "qtng/types/string_builder.h"

namespace qtng {

namespace {

std::uint32_t grownCapacity(std::uint32_t current, std::uint32_t needed)
{
    std::uint32_t next = current == 0 ? 64 : current;
    while (next < needed) {
        const std::uint32_t doubled = next + next / 2;
        // Overflow guard: if doubling no longer grows the value, take needed directly.
        next = doubled > next ? doubled : needed;
    }
    return next;
}

}  // namespace

StringBuilder::StringBuilder()
    : d(String::allocateBlock(64))
    , len(0)
    , capacity(64)
{
}

StringBuilder::~StringBuilder()
{
    String::releaseBlock(d);
}

StringBuilder &StringBuilder::append(const char *bytes, std::uint32_t n)
{
    if (n == 0) {
        return *this;
    }
    // len + n must be checked in uint64. Adding in uint32 would wrap around, so ensure()
    // would not grow the buffer while memcpy still writes n bytes and corrupts the heap;
    // len would wrap too, making size() lie from then on. The limit is String::MaxLength:
    // the builder should never produce anything a String cannot hold.
    if (static_cast<std::uint64_t>(len) + static_cast<std::uint64_t>(n) > String::MaxLength) {
        return *this;
    }
    ensure(len + n);
    std::memcpy(d->buf + len, bytes, n);
    len += n;
    return *this;
}

StringBuilder &StringBuilder::append(const String &s)
{
    // String::size() counts UTF-8 code points, so the byte length must come from byteSize()
    // -- using size() here would truncate every multi-byte character.
    return append(s.data(), s.byteSize());
}

StringBuilder &StringBuilder::append(const ByteArray &b)
{
    return append(b.data(), b.size());
}

StringBuilder &StringBuilder::append(const std::string &s)
{
    return append(s.data(), static_cast<std::uint32_t>(s.size()));
}

StringBuilder &StringBuilder::append(const char *cstr)
{
    return append(cstr, static_cast<std::uint32_t>(std::strlen(cstr)));
}

StringBuilder &StringBuilder::append(char c)
{
    return append(&c, 1);
}

void StringBuilder::ensure(std::uint32_t needed)
{
    if (needed <= capacity) {
        return;
    }
    const std::uint32_t next = grownCapacity(capacity, needed);
    String::Data *grown = String::allocateBlock(next);
    if (len > 0) {
        std::memcpy(grown->buf, d->buf, len);
    }
    String::releaseBlock(d);
    d = grown;
    capacity = next;
}

String StringBuilder::toString() const
{
    return String(d->buf, len);
}

ByteArray StringBuilder::toByteArray() const
{
    return ByteArray(d->buf, len);
}

}  // namespace qtng
