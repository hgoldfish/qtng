#include <cstdint>
#include <memory>

#include "qtng/eventloop.h"
#include "qtng/utils/platform.h"
#include "qtng/private/eventloop_p.h"
#include "qtng/locks.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.eventloop");

namespace qtng {

namespace {

CurrentLoopStorage *currentLoopStorageInstance()
{
    static CurrentLoopStorage storage;
    return &storage;
}

}  // namespace

CurrentLoopStorage *currentLoop()
{
    return currentLoopStorageInstance();
}

class CoroutineSpawnHelper : public Coroutine
{
public:
    explicit CoroutineSpawnHelper(function<void()> f)
        : f(make_unique<function<void()>>(move(f)))
    {
    }
    ~CoroutineSpawnHelper() override = default;
    void run() override
    {
        (*f)();
        f.reset();
    }

private:
    unique_ptr<function<void()>> f;
};

Coroutine *Coroutine::spawn(function<void()> f)
{
    Coroutine *c = new CoroutineSpawnHelper(move(f));
    c->start();
    return c;
}

Functor::~Functor() { }

bool DoNothingFunctor::operator()()
{
    return false;
}

YieldCurrentFunctor::YieldCurrentFunctor()
{
    coroutine = BaseCoroutine::current();
}

bool YieldCurrentFunctor::operator()()
{
    if (!coroutine) {
        ngDebug() << "coroutine is deleted while YieldCurrentFunctor called.";
        return false;
    }
    try {
        return coroutine->yield();
    } catch (CoroutineException &e) {
        ngDebug() << "do not send exception to event loop, just delete event loop:" << e.what();
    }
    return false;
}

EventLoopCoroutinePrivate::EventLoopCoroutinePrivate(EventLoopCoroutine *q)
    : q_ptr(q)
    , loopCoroutine(nullptr)
{
}

EventLoopCoroutinePrivate::~EventLoopCoroutinePrivate() { }

EventLoopCoroutine::EventLoopCoroutine(EventLoopCoroutinePrivate *d, size_t stackSize)
    : BaseCoroutine(BaseCoroutine::current(), stackSize)
    , dd_ptr(d)
    , m_backend(Backend::Ev)
{
}

EventLoopCoroutine::~EventLoopCoroutine()
{
    delete dd_ptr;
}

EventLoopCoroutine *EventLoopCoroutine::get()
{
    return currentLoopStorageInstance()->getOrCreate().get();
}

void EventLoopCoroutine::run()
{
    EventLoopCoroutinePrivate *d = d_func();
    d->run();
}

int EventLoopCoroutine::createWatcher(EventType event, intptr_t fd, Functor *callback)
{
    return d_func()->createWatcher(event, fd, callback);
}

void EventLoopCoroutine::startWatcher(int watcherId)
{
    d_func()->startWatcher(watcherId);
}

void EventLoopCoroutine::stopWatcher(int watcherId)
{
    d_func()->stopWatcher(watcherId);
}

void EventLoopCoroutine::removeWatcher(int watcherId)
{
    d_func()->removeWatcher(watcherId);
}

void EventLoopCoroutine::triggerIoWatchers(intptr_t fd)
{
    d_func()->triggerIoWatchers(fd);
}

int EventLoopCoroutine::callLater(uint32_t msecs, Functor *callback)
{
    return d_func()->callLater(msecs, callback);
}

void EventLoopCoroutine::callLaterThreadSafe(uint32_t msecs, Functor *callback)
{
    d_func()->callLaterThreadSafe(msecs, callback);
}

int EventLoopCoroutine::callRepeat(uint32_t msecs, Functor *callback)
{
    return d_func()->callRepeat(msecs, callback);
}

void EventLoopCoroutine::cancelCall(int callbackId)
{
    d_func()->cancelCall(callbackId);
}

int EventLoopCoroutine::exitCode()
{
    return d_func()->exitCode();
}

bool EventLoopCoroutine::runUntil(BaseCoroutine *coroutine)
{
    return d_func()->runUntil(coroutine);
}

bool EventLoopCoroutine::yield()
{
    EventLoopCoroutinePrivate *d = d_func();
    if (d->loopCoroutine) {
        return d->loopCoroutine->yield();
    }
    return BaseCoroutine::yield();
}

shared_ptr<EventLoopCoroutine> CurrentLoopStorage::getOrCreate()
{
    shared_ptr<EventLoopCoroutine> eventLoop;
    if (storage.hasLocalData()) {
        eventLoop = storage.localData();
    }
    if (!eventLoop) {
#if defined(QTNG_USE_EV)
        eventLoop = make_shared<EvEventLoopCoroutine>();
        eventLoop->setObjectName("libev_eventloop_coroutine");
        eventLoop->setBackend(EventLoopCoroutine::Backend::Ev);
        storage.setLocalData(eventLoop);
#elif defined(QTNG_USE_WIN)
        eventLoop = make_shared<WinEventLoopCoroutine>();
        eventLoop->setObjectName("win_eventloop_coroutine");
        eventLoop->setBackend(EventLoopCoroutine::Backend::Win);
        storage.setLocalData(eventLoop);
#else
#  error "No event loop backend configured"
#endif
    }
    return eventLoop;
}

shared_ptr<EventLoopCoroutine> CurrentLoopStorage::get()
{
    if (storage.hasLocalData()) {
        return storage.localData();
    }
    return shared_ptr<EventLoopCoroutine>();
}

void CurrentLoopStorage::set(shared_ptr<EventLoopCoroutine> eventLoop)
{
    storage.setLocalData(eventLoop);
}

void CurrentLoopStorage::clean()
{
    if (storage.hasLocalData()) {
        storage.setLocalData(shared_ptr<EventLoopCoroutine>());
    }
}

ScopedIoWatcher::ScopedIoWatcher(EventLoopCoroutine::EventType event, intptr_t fd)
    : event(event)
    , fd(fd)
    , watcherId(0)
{
}

bool ScopedIoWatcher::start()
{
    shared_ptr<EventLoopCoroutine> eventLoop = currentLoopStorageInstance()->getOrCreate();
    if (watcherId <= 0) {
        watcherId = eventLoop->createWatcher(event, fd, new YieldCurrentFunctor());
    }
    eventLoop->startWatcher(watcherId);
    return eventLoop->yield();
}

ScopedIoWatcher::~ScopedIoWatcher()
{
    if (watcherId > 0) {
        shared_ptr<EventLoopCoroutine> eventLoop = currentLoopStorageInstance()->getOrCreate();
        eventLoop->removeWatcher(watcherId);
    }
}

class CoroutinePrivate
{
public:
    explicit CoroutinePrivate(Coroutine *q)
        : q_ptr(q)
        , callbackId(0)
    {
        q->finished.addCallback([this](BaseCoroutine *) { finishedEvent.set(); });
    }
    ~CoroutinePrivate() = default;

    Coroutine *q_func() { return q_ptr; }
    const Coroutine *q_func() const { return q_ptr; }

    Coroutine * const q_ptr;
    Event finishedEvent;
    int callbackId;

    friend struct StartCoroutineFunctor;
    friend struct KillCoroutineFunctor;
};

struct StartCoroutineFunctor : public Functor
{
    explicit StartCoroutineFunctor(CoroutinePrivate *cp)
        : cp(cp)
    {
    }
    ~StartCoroutineFunctor() override = default;
    CoroutinePrivate *cp;
    bool operator()() override
    {
        if (!cp) {
            ngWarning() << "startCouroutine is called without coroutine.";
            return false;
        }
        cp->callbackId = 0;
        if (cp->q_func()->state() != BaseCoroutine::Initialized) {
            return false;
        }
        cp->q_func()->yield();
        return true;
    }
};

struct KillCoroutineFunctor : public Functor
{
    KillCoroutineFunctor(CoroutinePrivate *cp, CoroutineException *e)
        : cp(cp)
        , e(e)
    {
    }
    ~KillCoroutineFunctor() override
    {
        if (e) {
            delete e;
        }
    }
    CoroutinePrivate *cp;
    CoroutineException *e;
    bool operator()() override
    {
        if (!cp) {
            ngWarning() << "killCoroutine is called without coroutine";
            delete e;
            e = nullptr;
            return false;
        }
        if (cp->q_func()->state() != BaseCoroutine::Started) {
            delete e;
        } else {
            cp->q_func()->raise(e);
        }
        e = nullptr;
        return true;
    }
};

Coroutine::Coroutine(size_t stackSize)
    : BaseCoroutine(nullptr, stackSize)
    , d_ptr(new CoroutinePrivate(this))
{
}

Coroutine::~Coroutine()
{
    delete d_ptr;
}

Coroutine *Coroutine::start(uint32_t msecs)
{
    CoroutinePrivate *d = d_func();
    if (d->callbackId > 0 || isRunning() || isFinished()) {
        return this;
    }
    d->callbackId = EventLoopCoroutine::get()->callLater(msecs, new StartCoroutineFunctor(d));
    return this;
}

void Coroutine::kill(CoroutineException *e, uint32_t msecs)
{
    CoroutinePrivate *d = d_func();
    if (!e) {
        e = new CoroutineExitException();
    }
    EventLoopCoroutine *c = EventLoopCoroutine::get();
    if (state() == Coroutine::Initialized) {
        if (dynamic_cast<CoroutineExitException *>(e)) {
            if (d->callbackId > 0) {
                EventLoopCoroutine::get()->cancelCall(d->callbackId);
                d->callbackId = 0;
            }
            setState(Coroutine::Stopped);
            delete e;
            finished.callback(this);
        } else {
            if (d->callbackId == 0) {
                d->callbackId = c->callLater(msecs, new StartCoroutineFunctor(d));
            }
            c->callLater(msecs, new KillCoroutineFunctor(d, e));
        }
    } else if (isFinished()) {
        delete e;
    } else if (isRunning()) {
        c->callLater(msecs, new KillCoroutineFunctor(d, e));
    } else {
        ngWarning() << "invalid state while kiling coroutine.";
        delete e;
    }
}

void Coroutine::run()
{
    CoroutinePrivate *d = d_func();
    d->callbackId = 0;
}

void Coroutine::cleanup()
{
    if (previous()) {
        previous()->yield();
    } else {
        EventLoopCoroutine::get()->yield();
    }
}

bool Coroutine::join()
{
    CoroutinePrivate *d = d_func();
    if (state() == BaseCoroutine::Initialized || state() == BaseCoroutine::Started) {
        bool ok;
        if (!dynamic_cast<Coroutine *>(BaseCoroutine::current())) {
            ok = EventLoopCoroutine::get()->runUntil(this);
        } else {
            ok = d->finishedEvent.tryWait();
        }
        if (ok) {
            setState(Joined);
        }
        return ok;
    }
    return true;
}

Coroutine *Coroutine::current()
{
    BaseCoroutine *c = BaseCoroutine::current();
    return dynamic_cast<Coroutine *>(c);
}

struct ScopedCallLater
{
    explicit ScopedCallLater(int callbackId)
        : callbackId(callbackId)
    {
    }
    ~ScopedCallLater() { EventLoopCoroutine::get()->cancelCall(callbackId); }
    int callbackId;
};

void Coroutine::msleep(uint32_t msecs)
{
    int callbackId = EventLoopCoroutine::get()->callLater(msecs, new YieldCurrentFunctor());
    ScopedCallLater scl(callbackId);
    (void)scl;
    EventLoopCoroutine::get()->yield();
}

struct TimeoutFunctor : public Functor
{
    TimeoutFunctor(Timeout *out, BaseCoroutine *coroutine)
        : out(out)
        , coroutine(coroutine)
    {
    }
    ~TimeoutFunctor() override = default;
    Timeout *out;
    BaseCoroutine *coroutine;
    bool operator()() override
    {
        if (!out || !coroutine) {
            ngDebug() << "triggerTimeout is called while timeout or coroutine is deleted.";
            return false;
        }
        coroutine->raise(new TimeoutException());
        return true;
    }
};

TimeoutException::TimeoutException() { }

string TimeoutException::what() const
{
    return "coroutine had set timeout.";
}

void TimeoutException::raise()
{
    throw *this;
}

CoroutineException *TimeoutException::clone() const
{
    return new TimeoutException();
}

Timeout::Timeout(float secs)
    : msecs(static_cast<uint32_t>((secs > 0.0f ? secs : 0.0f) * 1000))
    , timeoutId(0)
{
    if (msecs) {
        restart();
    }
}

Timeout::Timeout(uint32_t msecs, int)
    : msecs(msecs)
    , timeoutId(0)
{
    if (msecs) {
        restart();
    }
}

Timeout::~Timeout()
{
    if (timeoutId) {
        EventLoopCoroutine::get()->cancelCall(timeoutId);
    }
}

void Timeout::cancel()
{
    if (timeoutId) {
        EventLoopCoroutine::get()->cancelCall(timeoutId);
        timeoutId = 0;
    }
}

void Timeout::restart()
{
    if (timeoutId) {
        EventLoopCoroutine::get()->cancelCall(timeoutId);
    }
    timeoutId = EventLoopCoroutine::get()->callLater(msecs, new TimeoutFunctor(this, BaseCoroutine::current()));
}

}  // namespace qtng
