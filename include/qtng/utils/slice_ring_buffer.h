#ifndef QTNG_UTILS_SLICE_RING_BUFFER_H
#define QTNG_UTILS_SLICE_RING_BUFFER_H

#include <cstddef>

#include "qtng/utils/platform.h"

namespace qtng {
namespace utils {

// SliceRingBuffer: a byte-level double-ended queue backed by a virtual-memory
// double mapping. Two adjacent virtual regions ([base, base+N) and
// [base+N, base+2N)) map the same N-byte physical page, so the live data
// window [head, tail) is always CONTIGUOUS in virtual address space no matter
// how far the ring has wrapped in physical memory.
//
// This is the complement of the cursor-based ByteBuffer (see string_utils.h):
// ByteBuffer is a one-way stream decoder, whereas SliceRingBuffer keeps a
// deque of bytes and hands out a single contiguous slice via data(). That
// makes it a natural fit for zero-copy feeding of an fd via writev/readv, and
// for decoders that want a whole frame as one memory block.
//
// Semantics:
//   - data(): pointer to the head of the live window. The first size() bytes
//     form ONE contiguous region regardless of physical wraparound.
//   - size(): number of live bytes (== tail - head).
//   - capacity(): the mapped physical capacity (rounded down to a page).
//   - push_back(p, n) / push_front(p, n): append/prepend, growing the mapping
//     automatically. Writing is contiguous per call only after an internal
//     recenter; the source p must NOT point into this buffer's own storage
//     (caller should copy to a temp first), because inside the double-mapping
//     a virtual-address-aliased self-memcpy would corrupt the window.
//   - pop_front(n) / pop_back(n): drop from either end.
//
// Growth strategy: when a push needs more room than the remaining capacity,
// the buffer allocates a larger double mapping, memcpy's the live window into
// the middle of it, and releases the old mapping. The same path is used on
// POSIX and Windows so behaviour is identical on both.
//
// Thread-safety: not thread-safe; single consumer/single producer (or a single
// thread) as with ByteBuffer. This is a value type: movable, non-copyable.

class SliceRingBufferPrivate;
class SliceRingBuffer
{
public:
    explicit SliceRingBuffer(std::size_t capacity = 0);
    ~SliceRingBuffer();

    SliceRingBuffer(SliceRingBuffer &&other) noexcept;
    SliceRingBuffer &operator=(SliceRingBuffer &&other) noexcept;

    std::size_t size() const;
    bool empty() const;
    std::size_t capacity() const;

    // Single contiguous view of all live bytes. Valid until the next
    // mutation. The pointer is stable across pop_front/pop_back while the
    // buffer is non-empty (they only move head/tail); the pop that empties the
    // buffer immediately re-centers the empty window (resetWindow), moving
    // head, so do not rely on the pointer across the empty boundary. It is NOT
    // stable across a push that triggers growth.
    const char *data() const;
    char *data();

    char front() const;
    char back() const;

    // Append/prepend n bytes. Growth is automatic.
    void push_back(const char *data, std::size_t n);
    inline void push_back(char c) { push_back(&c, 1); }
    void push_front(const char *data, std::size_t n);
    inline void push_front(char c) { push_front(&c, 1); }

    // Drop n bytes from either end (clamped to size()).
    void pop_front(std::size_t n);
    void pop_back(std::size_t n);

    // Discard all bytes; the mapping is retained (size() becomes 0).
    void clear();

    // Aliases matching ByteBuffer's stream-decoder vocabulary, so a decoder
    // written against ByteBuffer can switch to this class with minimal edits.
    inline void append(const char *data, std::size_t n) { push_back(data, n); }
    inline void prepend(const char *data, std::size_t n) { push_front(data, n); }
    inline void consume(std::size_t n) { pop_front(n); }

private:
    SliceRingBufferPrivate *d_ptr;
    NG_DECLARE_PRIVATE(SliceRingBuffer)
    NG_DISABLE_COPY(SliceRingBuffer)
};

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_SLICE_RING_BUFFER_H
