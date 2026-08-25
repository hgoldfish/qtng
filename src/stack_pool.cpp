#include <chrono>
#include <map>
#include <mutex>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "qtng/utils/platform.h"
#include "stack_pool.h"

using namespace std;

namespace qtng {
namespace stack_pool {

#ifdef NG_OS_UNIX

namespace {

constexpr size_t kThreadLocalMaxStacks = 32;    // per-thread cache cap (count)
constexpr size_t kThreadLocalMaxBytes = 64 * 1024 * 1024;  // ~256KB x 256 stacks
constexpr size_t kGlobalMaxStacksPerSize = 256; // per-size global cap (count)
constexpr size_t kGlobalMaxStacksTotal = 2048;  // global cap (count)
constexpr size_t kGlobalMaxBytes = 64 * 1024 * 1024;  // ~256KB x 256 stacks

size_t pageSize()
{
    static const size_t size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return size;
}

size_t roundUpToPage(size_t size)
{
    const size_t ps = pageSize();
    return (size + ps - 1) / ps * ps;
}

struct Stack
{
    void *mapping;                           // mmap() return value, includes the leading guard page
    size_t usableSize;                       // page-aligned usable area size
    chrono::steady_clock::time_point idleSince;
};

void *usableBase(const Stack &stack)
{
    return static_cast<char *>(stack.mapping) + pageSize();
}

void freeStack(Stack *stack)
{
    if (stack->mapping) {
        munmap(stack->mapping, pageSize() + stack->usableSize);
        stack->mapping = nullptr;
    }
}

void discardPages(Stack *stack)
{
#ifdef MADV_DONTNEED
    madvise(usableBase(*stack), stack->usableSize, MADV_DONTNEED);
#else
    (void)stack;
#endif
}

void *allocateStack(size_t usableSize)
{
    const size_t ps = pageSize();
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_STACK
    flags |= MAP_STACK;
#endif
    void *mapping = mmap(nullptr, ps + usableSize, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mapping == MAP_FAILED) {
        return nullptr;
    }
    if (mprotect(mapping, ps, PROT_NONE) != 0) {
        munmap(mapping, ps + usableSize);
        return nullptr;
    }
    return static_cast<char *>(mapping) + ps;
}

// The cached-stack inventory of one pool. Every push/pop/sweep keeps
// totalStacks and totalBytes in sync, so both the count and the byte caps are
// honest regardless of the stack size being cached.
struct PoolEntries
{
    map<size_t, vector<Stack>> freeList;
    size_t totalStacks = 0;
    size_t totalBytes = 0;
};

// Global fallback pool shared by all threads, guarded by a mutex. It receives
// stacks that overflow the thread-local cap and stacks flushed on thread exit.
struct GlobalPool
{
    ~GlobalPool()
    {
        for (auto &entry : entries.freeList) {
            for (Stack &stack : entry.second) {
                freeStack(&stack);
            }
        }
    }
    PoolEntries entries;
    std::mutex poolMutex;
};

GlobalPool &globalPool()
{
    static GlobalPool pool;
    return pool;
}

void pushToGlobal(Stack stack)
{
    GlobalPool &global = globalPool();
    bool accepted = false;
    {
        lock_guard<mutex> lock(global.poolMutex);
        PoolEntries &entries = global.entries;
        if (entries.totalStacks < kGlobalMaxStacksTotal && entries.totalBytes + stack.usableSize <= kGlobalMaxBytes) {
            vector<Stack> &bucket = entries.freeList[stack.usableSize];
            if (bucket.size() < kGlobalMaxStacksPerSize) {
                stack.idleSince = chrono::steady_clock::now();
                bucket.push_back(stack);
                ++entries.totalStacks;
                entries.totalBytes += stack.usableSize;
                accepted = true;
            }
        }
    }
    if (!accepted) {
        freeStack(&stack);
    }
}

// Per-thread fast path: no locking. Coroutines are created and destroyed on the
// same event-loop thread, so this path serves almost every release/acquire.
struct ThreadLocalPool
{
    ~ThreadLocalPool() { flushToGlobal(); }
    void flushToGlobal()
    {
        for (auto &entry : entries.freeList) {
            for (Stack &stack : entry.second) {
                pushToGlobal(stack);
            }
        }
        entries = PoolEntries();
    }
    PoolEntries entries;
};

ThreadLocalPool &threadLocalPool()
{
    thread_local ThreadLocalPool pool;
    return pool;
}

// Pop the newest stack from the bucket for key; null when the bucket is empty.
// Buckets are keyed by the exact page-aligned usable size, so every stack in a
// bucket matches every acquire of that key.
void *popFrom(PoolEntries &entries, size_t key)
{
    map<size_t, vector<Stack>>::iterator found = entries.freeList.find(key);
    if (found == entries.freeList.end() || found->second.empty()) {
        return nullptr;
    }
    Stack stack = found->second.back();
    found->second.pop_back();
    if (found->second.empty()) {
        entries.freeList.erase(found);
    }
    --entries.totalStacks;
    entries.totalBytes -= stack.usableSize;
    return usableBase(stack);
}

// Reclaim every cached stack idle for longer than timeout.
void sweepBucket(PoolEntries &entries, const chrono::steady_clock::time_point &now, const chrono::milliseconds &timeout)
{
    for (map<size_t, vector<Stack>>::iterator it = entries.freeList.begin(); it != entries.freeList.end();) {
        vector<Stack> &bucket = it->second;
        for (size_t i = bucket.size(); i > 0; --i) {
            if (now - bucket[i - 1].idleSince >= timeout) {
                entries.totalBytes -= bucket[i - 1].usableSize;
                freeStack(&bucket[i - 1]);
                bucket.erase(bucket.begin() + static_cast<ptrdiff_t>(i - 1));
                --entries.totalStacks;
            }
        }
        if (bucket.empty()) {
            it = entries.freeList.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

void *acquire(size_t size)
{
    if (!size) {
        return nullptr;
    }
    const size_t usable = roundUpToPage(size);

    ThreadLocalPool &local = threadLocalPool();
    if (void *stack = popFrom(local.entries, usable)) {
        return stack;
    }

    GlobalPool &global = globalPool();
    lock_guard<mutex> lock(global.poolMutex);
    if (void *stack = popFrom(global.entries, usable)) {
        return stack;
    }

    return allocateStack(usable);
}

void release(void *stack, size_t size)
{
    if (!stack || !size) {
        return;
    }
    Stack s;
    s.mapping = static_cast<char *>(stack) - pageSize();
    s.usableSize = roundUpToPage(size);
    s.idleSince = chrono::steady_clock::now();
    discardPages(&s);

    ThreadLocalPool &local = threadLocalPool();
    PoolEntries &entries = local.entries;
    if (entries.totalStacks < kThreadLocalMaxStacks && entries.totalBytes + s.usableSize <= kThreadLocalMaxBytes) {
        entries.freeList[s.usableSize].push_back(s);
        ++entries.totalStacks;
        entries.totalBytes += s.usableSize;
        return;
    }
    pushToGlobal(s);
}

void sweep()
{
    const chrono::steady_clock::time_point now = chrono::steady_clock::now();
    const chrono::milliseconds timeout(static_cast<long long>(kStackIdleTimeoutMs));

    ThreadLocalPool &local = threadLocalPool();
    sweepBucket(local.entries, now, timeout);

    GlobalPool &global = globalPool();
    lock_guard<mutex> lock(global.poolMutex);
    sweepBucket(global.entries, now, timeout);
}

#else

void *acquire(size_t)
{
    return nullptr;
}

void release(void *, size_t)
{
}

void sweep()
{
}

#endif  // NG_OS_UNIX

}  // namespace stack_pool
}  // namespace qtng
