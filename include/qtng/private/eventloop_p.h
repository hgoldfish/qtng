#ifndef QTNG_EVENTLOOP_P_H
#define QTNG_EVENTLOOP_P_H

#include <memory>
#include "qtng/eventloop.h"
#include "qtng/utils/thread_local.h"
#include "qtng/utils/platform.h"

namespace qtng {

class Functor
{
public:
    virtual ~Functor();
    virtual bool operator()() = 0;
};

class DoNothingFunctor : public Functor
{
public:
    virtual bool operator()();
};

class YieldCurrentFunctor : public Functor
{
public:
    explicit YieldCurrentFunctor();
    virtual bool operator()();
    BaseCoroutine *coroutine;
};

template<typename T>
class DeleteLaterFunctor : public Functor
{
public:
    explicit DeleteLaterFunctor(T *p)
        : p(p)
    {
    }
    virtual bool operator()() { delete p; return true; }
    T * const p;
};

class LambdaFunctor : public Functor
{
public:
    LambdaFunctor(const std::function<void()> &callback)
        : callback(callback)
    {
    }
    virtual bool operator()() override;
    std::function<void()> callback;
};

class EventLoopCoroutinePrivate;
class EventLoopCoroutine : public BaseCoroutine
{
    NG_DISABLE_COPY(EventLoopCoroutine)
public:
    enum EventType {
        Read = 1,
        Write = 2,
        ReadWrite = 3,
    };
    enum class Backend {
        Ev,
        Win,
    };
public:
    virtual ~EventLoopCoroutine() override;
    virtual void run() override;
public:
    int createWatcher(EventType event, std::intptr_t fd, Functor *callback);  // the ownership of callback is taken
    void startWatcher(int watcherId);
    void stopWatcher(int watcherId);
    void removeWatcher(int watcherId);
    void triggerIoWatchers(std::intptr_t fd);
    int callLater(std::uint32_t msecs, Functor *callback);  // the ownership of callback is taken
    void callLaterThreadSafe(std::uint32_t msecs, Functor *callback);  // the ownership of callback is taken
    int callRepeat(std::uint32_t msecs, Functor *callback);  // the ownership of callback is taken
    void cancelCall(int callbackId);
    int exitCode();
    bool runUntil(BaseCoroutine *coroutine);
    bool yield();
    Backend backend() const { return m_backend; }
    void setBackend(Backend backend) { m_backend = backend; }
    bool isEv() const { return m_backend == Backend::Ev; }
    bool isWin() const { return m_backend == Backend::Win; }
public:
    static EventLoopCoroutine *get();
protected:
    // eventloop coroutine should use a bigger stack size instead of DEFAULT_COROUTINE_STACK_SIZE, which may be defined
    // smaller.
    EventLoopCoroutine(EventLoopCoroutinePrivate *d, size_t stackSize = 1024 * 1024 * 8);
    friend class EventLoopCoroutinePrivate;
private:
    EventLoopCoroutinePrivate * const dd_ptr;
    Backend m_backend;
    NG_DECLARE_PRIVATE_D(dd_ptr, EventLoopCoroutine)
};

class ScopedIoWatcher
{
public:
    ScopedIoWatcher(EventLoopCoroutine::EventType event, std::intptr_t fd);
    ~ScopedIoWatcher();
    bool start();
private:
    EventLoopCoroutine::EventType event;
    std::intptr_t fd;
    int watcherId;
};

class EventLoopCoroutinePrivate
{
public:
    explicit EventLoopCoroutinePrivate(EventLoopCoroutine *q);
    virtual ~EventLoopCoroutinePrivate();
public:
    virtual void run() = 0;
    virtual int createWatcher(EventLoopCoroutine::EventType event, std::intptr_t fd, Functor *callback) = 0;
    virtual void startWatcher(int watcherId) = 0;
    virtual void stopWatcher(int watcherId) = 0;
    virtual void removeWatcher(int watcherId) = 0;
    virtual void triggerIoWatchers(std::intptr_t fd) = 0;
    virtual int callLater(std::uint32_t msecs, Functor *callback) = 0;
    virtual void callLaterThreadSafe(std::uint32_t msecs, Functor *callback) = 0;
    virtual int callRepeat(std::uint32_t msecs, Functor *callback) = 0;
    virtual void cancelCall(int callbackId) = 0;
    virtual int exitCode() = 0;
    virtual bool runUntil(BaseCoroutine *coroutine) = 0;
protected:
    EventLoopCoroutine * const q_ptr;
    BaseCoroutine *loopCoroutine;
    static EventLoopCoroutinePrivate *getPrivateHelper(EventLoopCoroutine *coroutine) { return reinterpret_cast<EventLoopCoroutinePrivate *>(coroutine->dd_ptr); }
    NG_DECLARE_PUBLIC(EventLoopCoroutine)
    friend class EventLoopCoroutine;
};

class CurrentLoopStorage
{
public:
    std::shared_ptr<EventLoopCoroutine> getOrCreate();
    std::shared_ptr<EventLoopCoroutine> get();
    void set(std::shared_ptr<EventLoopCoroutine> eventLoop);
    void clean();
private:
    qtng::utils::ThreadLocal<std::shared_ptr<EventLoopCoroutine>> storage;
};

CurrentLoopStorage *currentLoop();

#if QTNG_USE_EV
class EvEventLoopCoroutine : public EventLoopCoroutine
{
public:
    EvEventLoopCoroutine();
};
#endif

#if QTNG_USE_WIN
class WinEventLoopCoroutine : public EventLoopCoroutine
{
public:
    WinEventLoopCoroutine();
};
#endif

}  // namespace qtng

#endif
