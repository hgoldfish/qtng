#ifndef QTNG_UTILS_SHARED_MUTEX_COMPAT_H
#define QTNG_UTILS_SHARED_MUTEX_COMPAT_H

#include "qtng/utils/platform.h"

#if __cplusplus >= 201703L

#  include <shared_mutex>

namespace qtng {
namespace utils {

// WARNING: std::shared_mutex::unlock() releases an exclusive lock only. The
// mode-agnostic SharedReadWriteLock::unlock() therefore cannot be used with
// this type on the lockForRead() path. qtng core compiles as C++11 (the
// pthread_rwlock branch below), so this branch is dead today; do not switch
// the core standard to C++14/17 without giving SharedMutex a mode-aware
// unlock() first.
using SharedMutex = std::shared_mutex;

}  // namespace utils
}  // namespace qtng

#elif __cplusplus >= 201402L

#  include <shared_mutex>

namespace qtng {
namespace utils {

// WARNING: same mode-agnostic unlock() mismatch as the C++17 branch above.
using SharedMutex = std::shared_timed_mutex;

}  // namespace utils
}  // namespace qtng

#else

#  ifdef NG_OS_WIN

// Pre-C++14 Windows cannot rely on SRWLOCK: its release API must match the
// acquired mode, and tracking that with a per-lock field is shared non-atomic
// state that breaks under concurrent readers. Fall back to exclusive
// std::mutex for both shared and exclusive locking (correct, less concurrent).
#    include <mutex>

namespace qtng {
namespace utils {

class SharedMutex
{
public:
    SharedMutex() = default;
    void lock_shared() { mutex_.lock(); }
    void unlock_shared() { mutex_.unlock(); }
    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }

private:
    NG_DISABLE_COPY(SharedMutex);
    std::mutex mutex_;
};

}  // namespace utils
}  // namespace qtng

#  else

#    include <pthread.h>

namespace qtng {
namespace utils {

class SharedMutex
{
public:
    SharedMutex() { pthread_rwlock_init(&lock_, nullptr); }
    ~SharedMutex() { pthread_rwlock_destroy(&lock_); }
    void lock_shared() { pthread_rwlock_rdlock(&lock_); }
    void unlock_shared() { pthread_rwlock_unlock(&lock_); }
    void lock() { pthread_rwlock_wrlock(&lock_); }
    void unlock() { pthread_rwlock_unlock(&lock_); }

private:
    NG_DISABLE_COPY(SharedMutex);
    pthread_rwlock_t lock_;
};

}  // namespace utils
}  // namespace qtng

#  endif

#endif

#endif  // QTNG_UTILS_SHARED_MUTEX_COMPAT_H
