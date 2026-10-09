#ifndef QTNG_TYPES_PRIVATE_STRING_DATA_H
#define QTNG_TYPES_PRIVATE_STRING_DATA_H

// Shared implementation of qtng::String and qtng::ByteArray.
//
// Their layout and algorithms are identical; only the "type identity" differs: String is
// text, ByteArray is bytes, and there is no implicit conversion between them. A tag
// template parameter instantiates the same implementation as two unrelated types, avoiding
// a second copy of the 250-line algorithm.
//
// ---------------------------------------------------------------------------
// Layout (pinned by static_assert; changing it means changing the assertions too)
// ---------------------------------------------------------------------------
//
//   union {
//       struct { uint32_t len; char sso[28]; } inl;                          // 32B
//       struct { uint32_t len; uint32_t offset; Data *d; } heap;             // 16B -> 24B
//   }
//
//   sizeof(Block) == 32, alignof(Block) == 8
//
//   Data { CowRef ref; char buf[1]; }   // payload shares the block with the header; one allocation
//
// There is only one discriminator: **heap <=> len > InlineCapacity(28)**.
// There is no extra tag field; len itself is the discriminator bit.
//
//   - len <= 28: the content is in inl.sso, copied byte by byte;
//   - len >  28: the content is at heap.d->buf + heap.offset, and a copy only bumps the
//     reference count.
//
// A corollary of this invariant: **"short content + heap block" must never exist**. Every
// path that could produce that state (mid slicing out a short string, StringBuilder
// finalizing a short result) must copy back inline and free the block, or else data() will
// dereference the first 8 bytes of sso as a pointer.
//
// ---------------------------------------------------------------------------
// No NUL terminator is stored
// ---------------------------------------------------------------------------
//
// All 28 bytes of sso are available for content; not one byte is reserved for '\0'. Hence
// there is **no c_str()**: the content may legitimately contain '\0', and only size()
// defines the length. Every conversion to or from external code (LMDB / nlohmann / qtng
// msgpack) must pass the length explicitly.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "qtng/types/cow.h"

namespace qtng {

namespace detail {

struct StringTag
{
};
struct ByteArrayTag
{
};

// String and ByteArray share exactly the same block, so it is not templated on Tag: only
// the handle (the type's "identity") is templated, while the block itself is one thing.
struct CharBlockData
{
    CowRef ref;
    char buf[1];

    // The payload starts at offset 4 (sizeof(ref)), not at sizeof(CharBlockData) (8,
    // including tail padding). The allocation size must be computed from offsetof; see
    // BasicCharBlock::allocateBlock().
};

// Shared implementation of String and ByteArray. Tag only mints two unrelated type
// identities and takes no part in runtime behavior.
template<typename Tag>
class BasicCharBlock
{
public:
    typedef CharBlockData Data;

    // String and ByteArray are the two instantiations of this template, and they must be
    // able to share one block (see shareStorageAs), which means reaching the otherwise
    // private union of the other instantiation.
    template<typename>
    friend class BasicCharBlock;

    enum
    {
        InlineCapacity = 28,

        // The length field is a uint32: this is both the maximum length a single String
        // can express and the legal bound for "adding two lengths". Every length addition
        // must use it as a limit, or the sum wraps around to a small number in uint32. It
        // lives here instead of being hard-coded as 0xffffffff everywhere because it belongs
        // to "what type is the length field"; if that field ever widens, this must change
        // with it, and two separate literals would drift apart.
        MaxLength = 0xffffffffu,
    };

public:
    BasicCharBlock()
    {
        u.inl.len = 0;
    }

    BasicCharBlock(const char *src, std::uint32_t len)
    {
        init(src, len);
    }

    BasicCharBlock(const std::string &s)
    {
        init(s.data(), static_cast<std::uint32_t>(s.size()));
    }

    BasicCharBlock(const char *cstr)
    {
        init(cstr, static_cast<std::uint32_t>(std::strlen(cstr)));
    }

    BasicCharBlock(const BasicCharBlock &other)
    {
        std::memcpy(&u, &other.u, sizeof(u));
        if (isHeap()) {
            cowIncref(u.heap.d->ref);
        }
    }

    BasicCharBlock(BasicCharBlock &&other)
    {
        u = other.u;
        // Empty the source, or both sides will decref at destruction.
        other.u.inl.len = 0;
    }

    ~BasicCharBlock()
    {
        release();
    }

    BasicCharBlock &operator=(const BasicCharBlock &other)
    {
        if (this == &other) {
            return *this;
        }
        release();
        std::memcpy(&u, &other.u, sizeof(u));
        if (isHeap()) {
            cowIncref(u.heap.d->ref);
        }
        return *this;
    }

    BasicCharBlock &operator=(BasicCharBlock &&other)
    {
        if (this == &other) {
            return *this;
        }
        release();
        u = other.u;
        other.u.inl.len = 0;
        return *this;
    }

public:
    // First byte of the content. For an empty string this returns a valid, non-null pointer
    // (the handle's own inline area, whose bytes are never read because size() is 0), so the
    // caller can express an empty range as data() + size().
    const char *data() const
    {
        if (!isHeap()) {
            return u.inl.sso;
        }
        return u.heap.d->buf + u.heap.offset;
    }

    std::uint32_t size() const { return length(); }

    bool isEmpty() const { return length() == 0; }

    bool isDetached() const { return !isHeap() || !cowIsShared(u.heap.d->ref); }

    long useCount() const { return isHeap() ? cowUseCount(u.heap.d->ref) : 0; }

    // The only safe conversion when interacting with external libraries (LMDB / nlohmann /
    // msgpack).
    std::string toStdString() const { return std::string(data(), size()); }

public:
    // Shared slice: when the result is long it reuses the parent's Data, changing only
    // offset/len; when the result is short it copies back inline (a shallow copy, not
    // shared).
    //
    // This is the guard for the "heap <=> len > 28" invariant: a short slice never creates
    // a heap block.
    BasicCharBlock mid(std::uint32_t offset, std::uint32_t len) const
    {
        const std::uint32_t n = length();
        if (offset >= n) {
            return BasicCharBlock();
        }
        if (len > n - offset) {
            len = n - offset;
        }
        if (len <= InlineCapacity) {
            // A short result is always inline; never reuse the parent's heap block.
            return BasicCharBlock(data() + offset, len);
        }
        if (!isHeap()) {
            // Impossible: an inline string has length <= 28 < len. Return an empty string
            // defensively instead of reading out of bounds.
            return BasicCharBlock();
        }
        BasicCharBlock result;
        result.u.heap.len = len;
        result.u.heap.offset = u.heap.offset + offset;
        result.u.heap.d = u.heap.d;
        cowIncref(result.u.heap.d->ref);
        return result;
    }

    bool operator==(const BasicCharBlock &other) const
    {
        return size() == other.size() && std::memcmp(data(), other.data(), size()) == 0;
    }

    bool operator!=(const BasicCharBlock &other) const { return !(*this == other); }

    bool operator<(const BasicCharBlock &other) const
    {
        const std::uint32_t n = size() < other.size() ? size() : other.size();
        const int c = n == 0 ? 0 : std::memcmp(data(), other.data(), n);
        if (c != 0) {
            return c < 0;
        }
        return size() < other.size();
    }

public:
    // Internal interface: StringBuilder needs to fill a shared block directly so finalizing
    // does not copy once more.
    static Data *allocateBlock(std::uint32_t capacity)
    {
        if (capacity == 0) {
            capacity = 1;
        }
        void *raw = ::operator new(offsetof(Data, buf) + capacity);
        Data *d = static_cast<Data *>(raw);
        cowInitRef(d->ref);
        return d;
    }

    static void releaseBlock(Data *d)
    {
        if (d == nullptr) {
            return;
        }
        ::operator delete(static_cast<void *>(d));
    }

    // Take over a shared block whose content is already filled in and whose reference count
    // is 1; ownership transfers to the return value. Preconditions: d came from
    // allocateBlock, its ref is 1, and the first len bytes are filled in. When
    // len <= InlineCapacity it is corrected to inline on the spot and the block is freed --
    // so even if the caller passes a wrong length, it cannot leave behind the "short content
    // + heap block" state that would make data() read sso as a pointer.
    static BasicCharBlock adoptBlock(Data *d, std::uint32_t len)
    {
        if (len <= static_cast<std::uint32_t>(InlineCapacity)) {
            BasicCharBlock result(d->buf, len);
            releaseBlock(d);
            return result;
        }
        BasicCharBlock result;
        result.u.heap.len = len;
        result.u.heap.offset = 0;
        result.u.heap.d = d;
        return result;
    }

    // Cross-tag zero-copy share: reinterpret this handle's storage as a handle of the
    // other type (String <-> ByteArray). Both types are immutable and are built on the
    // same CharBlockData layout, so aliasing one block between them is sound -- nothing
    // can mutate it afterwards. A heap block is shared with a single incref (O(1),
    // independent of length); inline content is copied in place (32 bytes, no
    // allocation). The whole union is copied, which preserves the "heap <=> len >
    // InlineCapacity" discriminator for free. This backs String::fromUtf8()/toUtf8().
    template<typename OtherTag>
    BasicCharBlock<OtherTag> shareStorageAs() const
    {
        BasicCharBlock<OtherTag> result;
        std::memcpy(&result.u, &u, sizeof(u));
        if (isHeap()) {
            cowIncref(result.u.heap.d->ref);
        }
        return result;
    }

private:
    // Read len with memcpy rather than reading inl.len directly: that way, if someone
    // changes the union member layout later, we do not depend on the easily forgotten
    // "common initial sequence" rule.
    std::uint32_t length() const
    {
        std::uint32_t n = 0;
        std::memcpy(&n, &u, sizeof(n));
        return n;
    }

    bool isHeap() const { return length() > static_cast<std::uint32_t>(InlineCapacity); }

    // The parameter is deliberately named src rather than data: this class has a data()
    // method, and a same-named parameter would shadow it.
    void init(const char *src, std::uint32_t len)
    {
        if (len <= static_cast<std::uint32_t>(InlineCapacity)) {
            u.inl.len = len;
            if (len > 0) {
                std::memcpy(u.inl.sso, src, len);
            }
            return;
        }
        Data *d = allocateBlock(len);
        if (len > 0) {
            std::memcpy(d->buf, src, len);
        }
        u.heap.len = len;
        u.heap.offset = 0;
        u.heap.d = d;
    }

    void release()
    {
        if (isHeap()) {
            if (cowDecref(u.heap.d->ref)) {
                releaseBlock(u.heap.d);
            }
        }
        u.inl.len = 0;
    }

private:
    struct Inl
    {
        std::uint32_t len;
        char sso[InlineCapacity];
    };
    struct Heap
    {
        std::uint32_t len;
        std::uint32_t offset;
        Data *d;
    };
    union U {
        Inl inl;
        Heap heap;
    };
    U u;
};

// Layout assertions. C++11's static_assert requires a message.
// The handle size is determined by the inl branch (32B): the heap branch is 16B, and any
// attempt to grow past 32 would fail the sizeof(String) assertion below -- no extra
// mechanism is needed to watch it.
static_assert(sizeof(CharBlockData) == 8, "String::Data must be 8 bytes (4 ref + 1 buf + 3 tail padding)");
static_assert(offsetof(CharBlockData, buf) == 4, "String::Data payload must start at offset 4");
static_assert(sizeof(BasicCharBlock<StringTag>) == 32, "String must be 32 bytes");
static_assert(alignof(BasicCharBlock<StringTag>) == 8, "String alignment must be 8");
static_assert(sizeof(BasicCharBlock<ByteArrayTag>) == 32, "ByteArray must be 32 bytes");
static_assert(alignof(BasicCharBlock<ByteArrayTag>) == 8, "ByteArray alignment must be 8");

}  // namespace detail

}  // namespace qtng

#endif  // QTNG_TYPES_PRIVATE_STRING_DATA_H
