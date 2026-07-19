#ifndef QTNG_COROUTINE_UTILS_H
#define QTNG_COROUTINE_UTILS_H

#include <atomic>
#include <cassert>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "qtng/locks.h"
#include "qtng/private/eventloop_p.h"
#include "qtng/utils/platform.h"

// std::thread is required by the library internally (NgThread is built on top
// of it). waitThread() is only exposed when std::thread is actually available;
// on toolchains without <thread> the helper is simply not provided.
#if defined(__has_include)
#  if __has_include(<thread>)
#    include <thread>
#    define QTNG_HAS_STD_THREAD 1
#  endif
#elif defined(__cplusplus) && __cplusplus >= 201103L
#  include <thread>
#  define QTNG_HAS_STD_THREAD 1
#endif

namespace qtng {

template<typename T>
T callInEventLoop(std::function<T()> func)
{
    assert(static_cast<BaseCoroutine *>(EventLoopCoroutine::get()) != BaseCoroutine::current());

    std::shared_ptr<T> result = std::make_shared<T>();
    std::shared_ptr<Event> done = std::make_shared<Event>();

    std::function<void()> wrapper = [result, done, func]() mutable {
        *result = func();
        done->set();
    };

    int callbackId = EventLoopCoroutine::get()->callLater(0, new LambdaFunctor(wrapper));
    try {
        done->tryWait();
        EventLoopCoroutine::get()->cancelCall(callbackId);
    } catch (...) {
        EventLoopCoroutine::get()->cancelCall(callbackId);
        throw;
    }
    return *result;
}

inline void callInEventLoop(std::function<void()> func, std::uint32_t msecs = 0)
{
    assert(static_cast<BaseCoroutine *>(EventLoopCoroutine::get()) != BaseCoroutine::current());

    std::shared_ptr<Event> done = std::make_shared<Event>();

    std::function<void()> wrapper = [done, func]() {
        func();
        done->set();
    };

    int callbackId = EventLoopCoroutine::get()->callLater(msecs, new LambdaFunctor(wrapper));
    try {
        done->tryWait();
        EventLoopCoroutine::get()->cancelCall(callbackId);
    } catch (...) {
        EventLoopCoroutine::get()->cancelCall(callbackId);
        throw;
    }
}

inline void callInEventLoopAsync(std::function<void()> func, std::uint32_t msecs = 0)
{
    EventLoopCoroutine::get()->callLater(msecs, new LambdaFunctor(func));
}

// Run func on a dedicated worker thread and return an Event that is set when
// the work completes. The caller yields on the Event (tryWait) so the calling
// coroutine does not block the OS thread. NgThread/DeferCallThread are internal
// details and live in private/coroutine_utils_p.h.
std::shared_ptr<Event> spawnInThread(const std::function<void()> &func);

template<typename T>
T callInThread(std::function<T()> func)
{
    std::shared_ptr<T> result = std::make_shared<T>();
    std::function<void()> makeResult = [result, func]() mutable { *result = func(); };
    spawnInThread(makeResult)->tryWait();
    return *result;
}

template<typename T, typename ARG1>
T callInThread(std::function<T(ARG1)> func, ARG1 arg1)
{
    return callInThread<T>([func, arg1]() -> T { return func(arg1); });
}

template<typename T, typename ARG1, typename ARG2>
T callInThread(std::function<T(ARG1, ARG2)> func, ARG1 arg1, ARG2 arg2)
{
    return callInThread<T>([func, arg1, arg2]() -> T { return func(arg1, arg2); });
}

template<typename T, typename ARG1, typename ARG2, typename ARG3>
T callInThread(std::function<T(ARG1, ARG2, ARG3)> func, ARG1 arg1, ARG2 arg2, ARG3 arg3)
{
    return callInThread<T>([func, arg1, arg2, arg3]() -> T { return func(arg1, arg2, arg3); });
}

template<typename T, typename ARG1, typename ARG2, typename ARG3, typename ARG4>
T callInThread(std::function<T(ARG1, ARG2, ARG3)> func, ARG1 arg1, ARG2 arg2, ARG3 arg3, ARG4 arg4)
{
    return callInThread<T>([func, arg1, arg2, arg3, arg4]() -> T { return func(arg1, arg2, arg3, arg4); });
}

inline void callInThread(const std::function<void()> &func)
{
    spawnInThread(func)->tryWait();
}

#ifdef QTNG_HAS_STD_THREAD
// Wait for a std::thread to finish without blocking the event loop: the join
// is performed on a worker thread (via callInThread) while the calling coroutine
// yields on an Event. Returns true if the thread was joinable and has finished.
bool waitThread(std::thread &thread);
#endif

#ifdef NG_OS_UNIX
bool waitProcessPid(int pid);
#elif defined(NG_OS_WIN)
bool waitint /*pid*/(void *handle);
#endif

class CoroutineThreadPrivate;
// A CoroutineThread runs its own event loop on a dedicated OS thread and
// dispatches submitted functors as coroutines on that loop. NgThread is no
// longer part of the public API, so CoroutineThread exposes start()/wait()/
// isFinished() through composition instead of inheritance.
class CoroutineThread
{
public:
    explicit CoroutineThread(std::uint32_t capacity = UINT_MAX);
    ~CoroutineThread();
    void start();
    void wait();
    bool isFinished() const;
    void apply(const std::function<void()> &f);

private:
    CoroutineThreadPrivate * const dd_ptr;
    NG_DECLARE_PRIVATE_D(dd_ptr, CoroutineThread)
};

inline std::shared_ptr<Deferred<std::shared_ptr<Coroutine>>> waitForAny()
{
    return std::make_shared<Deferred<std::shared_ptr<Coroutine>>>();
}

template<typename... CS>
std::shared_ptr<Deferred<std::shared_ptr<Coroutine>>> waitForAny(std::shared_ptr<Coroutine> c1, CS... cs)
{
    std::shared_ptr<Deferred<std::shared_ptr<Coroutine>>> df = waitForAny(cs...);
    std::weak_ptr<Coroutine> c1w = c1;
    int callbackId = c1->finished.addCallback([c1w, df](BaseCoroutine *) {
        assert(!c1w.expired());
        df->callback(c1w.lock());
    });

    df->addCallback([c1w, callbackId](std::shared_ptr<Coroutine>) {
        if (auto c = c1w.lock()) {
            c->finished.remove(callbackId);
        }
    });
    return df;
}

template<typename... CS>
std::shared_ptr<Coroutine> any(CS... cs)
{
    std::shared_ptr<Deferred<std::shared_ptr<Coroutine>>> df = waitForAny(cs...);
    std::shared_ptr<ValueEvent<std::shared_ptr<Coroutine>>> event =
            std::make_shared<ValueEvent<std::shared_ptr<Coroutine>>>();
    df->addCallback([event](std::shared_ptr<Coroutine> c) { event->send(c); });
    try {
        return event->tryWait();
    } catch (...) {
        df->callback(std::shared_ptr<Coroutine>());
        throw;
    }
}

class Coroutine;
class CoroutineGroup
{
public:
    CoroutineGroup();
    ~CoroutineGroup();
public:
    bool add(std::shared_ptr<Coroutine> coroutine, const std::string &name = std::string());
    bool add(Coroutine *coroutine, const std::string &name = std::string())
    {
        return add(std::shared_ptr<Coroutine>(coroutine), name);
    }
    bool start(Coroutine *coroutine, const std::string &name = std::string()) { return add(coroutine->start(), name); }
    std::shared_ptr<Coroutine> get(const std::string &name);
    bool has(const std::string &name);
    bool isCurrent(const std::string &name);
    bool kill(const std::string &name, bool join = true);
    bool killall(bool join = true);
    bool join(const std::string &name);
    bool joinall();
    int size() const { return static_cast<int>(coroutines.size()); }
    bool isEmpty() const { return coroutines.empty(); }
    std::shared_ptr<Coroutine> any();

    inline std::shared_ptr<Coroutine> spawnWithName(const std::string &name, const std::function<void()> &func,
                                                   bool replace = false);
    inline std::shared_ptr<Coroutine> spawn(const std::function<void()> &func);

    template<typename T, typename S>
    static std::vector<T> map(std::function<T(S)> func, const std::vector<S> &l, int chunk = INT16_MAX)
    {
        CoroutineGroup operations;
        std::shared_ptr<std::vector<T>> result = std::make_shared<std::vector<T>>();
        std::shared_ptr<Semaphore> semaphore = std::make_shared<Semaphore>(chunk);
        for (size_t i = 0; i < l.size(); ++i) {
            result->push_back(T());
            S s = l[i];
            semaphore->tryAcquire();
            operations.spawn([func, s, result, i, semaphore] {
                try {
                    (*result)[i] = func(s);
                    semaphore->release();
                } catch (...) {
                    semaphore->release();
                    throw;
                }
            });
        }
        operations.joinall();
        return *result;
    }

    template<typename S>
    static void each(std::function<void(S)> func, const std::vector<S> &l, int chunk = INT16_MAX)
    {
        CoroutineGroup operations;
        std::shared_ptr<Semaphore> semaphore = std::make_shared<Semaphore>(chunk);
        for (size_t i = 0; i < l.size(); ++i) {
            semaphore->tryAcquire();
            S s = l[i];
            operations.spawn([func, s, semaphore] {
                try {
                    func(s);
                    semaphore->release();
                } catch (...) {
                    semaphore->release();
                    throw;
                }
            });
        }
        operations.joinall();
    }

    template<typename T, typename S>
    T apply(std::function<T(S)> func, S s)
    {
        std::shared_ptr<T> result = std::make_shared<T>();
        std::shared_ptr<Coroutine> t = spawn([func, result, s] { (*result) = func(s); });
        t->join();
        return *result;
    }

private:
    void deleteCoroutine(BaseCoroutine *coroutine);
    std::unordered_set<std::shared_ptr<Coroutine>> coroutines;
    std::shared_ptr<std::atomic<bool>> alive;
};

std::shared_ptr<Coroutine> CoroutineGroup::spawnWithName(const std::string &name, const std::function<void()> &func,
                                                        bool replace)
{
    std::shared_ptr<Coroutine> old = get(name);
    if (old) {
        if (replace) {
            old->kill();
            coroutines.erase(old);
            old->join();
        } else {
            return old;
        }
    }
    std::shared_ptr<Coroutine> coroutine(Coroutine::spawn(func));
    add(coroutine, name);
    return coroutine;
}

std::shared_ptr<Coroutine> CoroutineGroup::spawn(const std::function<void()> &func)
{
    std::shared_ptr<Coroutine> coroutine(Coroutine::spawn(func));
    add(coroutine);
    return coroutine;
}

namespace detail {
struct NormalType
{
};
struct VoidType
{
};
template<typename T>
struct ApplyDispatchTag
{
    using Tag = NormalType;
};
template<>
struct ApplyDispatchTag<void>
{
    using Tag = VoidType;
};
}  // namespace detail

class ThreadPool
{
public:
    ThreadPool(int threads = 0);
    ~ThreadPool();

    template<typename T, typename S>
    std::vector<T> map(std::function<T(S)> func, const std::vector<S> &l, int chunk = INT16_MAX);

    template<typename S>
    void each(std::function<void(S)> func, const std::vector<S> &l, int chunk = INT16_MAX);

    template<typename T, typename Func, typename... ARGS>
    T apply(Func func, ARGS... s);

    template<typename T>
    T call(std::function<T()> func);

    void call(std::function<void()> func);

private:
    template<typename T, typename Func, typename... ARGS>
    T apply_dispatch(Func func, detail::NormalType, ARGS... args);
    template<typename T, typename Func, typename... ARGS>
    T apply_dispatch(Func func, detail::VoidType, ARGS... args);

    class WorkThread;
    std::vector<std::shared_ptr<WorkThread>> threads;
    std::shared_ptr<Semaphore> semaphore;
    std::shared_ptr<std::atomic<bool>> alive;
};

template<typename T, typename S>
std::vector<T> ThreadPool::map(std::function<T(S)> func, const std::vector<S> &l, int chunk)
{
    std::function<T(S)> f = [this, func](S s) -> T {
        std::function<T()> wrapped = [s, func]() -> T { return func(s); };
        return call(wrapped);
    };
    return CoroutineGroup::map(f, l, chunk);
}

template<typename S>
void ThreadPool::each(std::function<void(S)> func, const std::vector<S> &l, int chunk)
{
    std::function<void(S)> f = [this, func](S s) {
        std::function<void()> wrapped = [s, func] { func(s); };
        call(wrapped);
    };
    CoroutineGroup::each(f, l, chunk);
}

template<typename T, typename Func, typename... ARGS>
T ThreadPool::apply(Func func, ARGS... args)
{
    return apply_dispatch<T, Func, ARGS...>(func, typename detail::ApplyDispatchTag<T>::Tag{}, args...);
}

template<typename T, typename Func, typename... ARGS>
T ThreadPool::apply_dispatch(Func func, detail::NormalType, ARGS... args)
{
    std::shared_ptr<T> result = std::make_shared<T>();
    std::function<void()> wrapped = [func, result, args...] { *result = func(args...); };
    call(wrapped);
    return *result;
}

template<typename T, typename Func, typename... ARGS>
T ThreadPool::apply_dispatch(Func func, detail::VoidType, ARGS... args)
{
    std::function<void()> wrapped = [func, args...] { func(args...); };
    call(wrapped);
    return T();
}

template<typename T>
T ThreadPool::call(std::function<T()> func)
{
    std::shared_ptr<T> result = std::make_shared<T>();
    std::function<void()> wrapped = [result, func] { *result = func(); };
    call(wrapped);
    return *result;
}

}  // namespace qtng

#endif  // QTNG_COROUTINE_UTILS_H
