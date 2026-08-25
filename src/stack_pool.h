#ifndef QTNG_STACK_POOL_H
#define QTNG_STACK_POOL_H

#include <cstddef>

#include "qtng/utils/platform.h"

#ifdef NG_OS_UNIX

namespace qtng {
namespace stack_pool {

// Caches coroutine stacks so that short-lived coroutines reuse the same VMAs
// instead of paying for a mmap()/munmap() pair on every spawn/destroy. Each
// cached mapping is a [PROT_NONE guard page][PROT_READ|WRITE usable area] pair;
// acquire()/release() exchange the usable-area base pointer.
//
// The pool knows nothing about the event loop. The event loop is expected to
// call sweep() periodically (the interval is kStackIdleTimeoutMs); without an
// event loop, stacks simply stay cached until the owning thread exits or a
// capacity limit is reached.

// Idle stacks cached longer than this are released by sweep().
constexpr std::size_t kStackIdleTimeoutMs = 10000;

void *acquire(std::size_t size);
void release(void *stack, std::size_t size);
void sweep();

}  // namespace stack_pool
}  // namespace qtng

#endif  // NG_OS_UNIX

#endif  // QTNG_STACK_POOL_H
