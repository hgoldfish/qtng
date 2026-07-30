#ifndef QTNG_UTILS_SHARED_MUTEX_COMPAT_H
#define QTNG_UTILS_SHARED_MUTEX_COMPAT_H

#include "qtng/utils/platform.h"

#if __cplusplus >= 201703L

#  include <shared_mutex>

namespace qtng {
namespace utils {

using SharedMutex = std::shared_mutex;

}  // namespace utils
}  // namespace qtng

#elif __cplusplus >= 201402L

#  include <shared_mutex>

namespace qtng {
namespace utils {

using SharedMutex = std::shared_timed_mutex;

}  // namespace utils
}  // namespace qtng

#else

#  ifdef NG_OS_WIN

#    if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0600)

#      ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#      endif
#      include <windows.h>

namespace qtng {
namespace utils {

class SharedMutex
{
public:
    SharedMutex() { InitializeSRWLock(&lock_); }
    void lock_shared() { AcquireSRWLockShared(&lock_); }
    void unlock_shared() { ReleaseSRWLockShared(&lock_); }
    void lock() { AcquireSRWLockExclusive(&lock_); }
    void unlock() { ReleaseSRWLockExclusive(&lock_); }

private:
    NG_DISABLE_COPY(SharedMutex);
    SRWLOCK lock_;
};

}  // namespace utils
}  // namespace qtng

#    else

// SRWLOCK requires Vista+. On XP, fall back to exclusive std::mutex for both
// shared and exclusive locking (correct, less concurrent).
#      include <mutex>

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

#    endif

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
