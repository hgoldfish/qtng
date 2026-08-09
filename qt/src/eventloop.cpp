#include <QtCore/qdebug.h>
#include <QtCore/qpointer.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qatomic.h>

#include "bridge/core_access.h"
#include "eventloop.h"
#include "private/eventloop_p.h"
#include "locks.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

qtng_core::Functor *toCoreFunctor(QTNETWORKNG_NAMESPACE::Functor *callback);

class ForwardingEventLoopPrivate : public EventLoopCoroutinePrivate
{
public:
    ForwardingEventLoopPrivate(EventLoopCoroutine *q, qtng_core::EventLoopCoroutine *coreEl)
        : EventLoopCoroutinePrivate(q)
        , core(coreEl)
    {
    }

    void run() override
    {
        if (core) {
            core->run();
        }
    }
    int createWatcher(EventLoopCoroutine::EventType event, qintptr fd, QTNETWORKNG_NAMESPACE::Functor *callback) override
    {
        return core->createWatcher(static_cast<qtng_core::EventLoopCoroutine::EventType>(event), fd,
                                   toCoreFunctor(callback));
    }
    void startWatcher(int watcherId) override { core->startWatcher(watcherId); }
    void stopWatcher(int watcherId) override { core->stopWatcher(watcherId); }
    void removeWatcher(int watcherId) override { core->removeWatcher(watcherId); }
    void triggerIoWatchers(qintptr fd) override { core->triggerIoWatchers(fd); }
    int callLater(quint32 msecs, QTNETWORKNG_NAMESPACE::Functor *callback) override
    {
        return core->callLater(msecs, toCoreFunctor(callback));
    }
    void callLaterThreadSafe(quint32 msecs, QTNETWORKNG_NAMESPACE::Functor *callback) override
    {
        core->callLaterThreadSafe(msecs, toCoreFunctor(callback));
    }
    int callRepeat(quint32 msecs, QTNETWORKNG_NAMESPACE::Functor *callback) override
    {
        return core->callRepeat(msecs, toCoreFunctor(callback));
    }
    void cancelCall(int callbackId) override { core->cancelCall(callbackId); }
    int exitCode() override { return core ? core->exitCode() : 0; }
    bool runUntil(BaseCoroutine *coroutine) override
    {
        return core && core->runUntil(coreFor(coroutine));
    }

    qtng_core::EventLoopCoroutine *core;
};

class FunctorBridge : public qtng_core::Functor
{
public:
    explicit FunctorBridge(QTNETWORKNG_NAMESPACE::Functor *qtFunctor)
        : qtFunctor(qtFunctor)
    {
    }
    ~FunctorBridge() override { delete qtFunctor; }
    bool operator()() override { return qtFunctor ? (*qtFunctor)() : false; }

    QTNETWORKNG_NAMESPACE::Functor *qtFunctor;
};

qtng_core::Functor *toCoreFunctor(QTNETWORKNG_NAMESPACE::Functor *callback)
{
    return callback ? new FunctorBridge(callback) : nullptr;
}

class EventLoopCoroutineWrapper : public EventLoopCoroutine
{
public:
    explicit EventLoopCoroutineWrapper(qtng_core::EventLoopCoroutine *coreEl)
        : EventLoopCoroutine(new ForwardingEventLoopPrivate(this, coreEl), 1024 * 256)
        , trackedCore(coreEl)
    {
        registerQtWrapper(coreEl, this);
        if (coreEl->isEv()) {
            setObjectName(QString::fromUtf8("libev_eventloop_coroutine"));
        } else if (coreEl->isWin()) {
            setObjectName(QString::fromUtf8("win_eventloop_coroutine"));
        } else {
            setObjectName(QString::fromUtf8("qt_eventloop_coroutine"));
        }
    }
    ~EventLoopCoroutineWrapper() override
    {
        if (trackedCore) {
            unregisterQtWrapper(trackedCore);
        }
    }

private:
    qtng_core::EventLoopCoroutine *trackedCore;
};

namespace {

QAtomicInteger<int> preferLibevFlag(0);

}  // namespace

// ---- Functors ----

Functor::~Functor() = default;

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
    if (coroutine.isNull()) {
        return false;
    }
    try {
        return coroutine->yield();
    } catch (CoroutineException &) {
        return false;
    }
}

bool LambdaFunctor::operator()()
{
    if (callback) {
        callback();
    }
    return true;
}

// ---- EventLoopCoroutinePrivate base ----

EventLoopCoroutinePrivate::EventLoopCoroutinePrivate(EventLoopCoroutine *q)
    : q_ptr(q)
{
}

EventLoopCoroutinePrivate::~EventLoopCoroutinePrivate() = default;

// ---- EventLoopCoroutine ----

static CurrentLoopStorage *currentLoopStorageInstance()
{
    static CurrentLoopStorage storage;
    return &storage;
}

CurrentLoopStorage *currentLoop()
{
    return currentLoopStorageInstance();
}

EventLoopCoroutine::EventLoopCoroutine(EventLoopCoroutinePrivate *d, size_t stackSize)
    : BaseCoroutine(BaseCoroutine::current(), stackSize)
    , dd_ptr(d)
{
}

EventLoopCoroutine::~EventLoopCoroutine()
{
    delete dd_ptr;
}

void EventLoopCoroutine::run()
{
    Q_D(EventLoopCoroutine);
    d->run();
}

int EventLoopCoroutine::createWatcher(EventType event, qintptr fd, Functor *callback)
{
    Q_D(EventLoopCoroutine);
    return d->createWatcher(event, fd, callback);
}

void EventLoopCoroutine::startWatcher(int watcherId)
{
    Q_D(EventLoopCoroutine);
    d->startWatcher(watcherId);
}

void EventLoopCoroutine::stopWatcher(int watcherId)
{
    Q_D(EventLoopCoroutine);
    d->stopWatcher(watcherId);
}

void EventLoopCoroutine::removeWatcher(int watcherId)
{
    Q_D(EventLoopCoroutine);
    d->removeWatcher(watcherId);
}

void EventLoopCoroutine::triggerIoWatchers(qintptr fd)
{
    Q_D(EventLoopCoroutine);
    d->triggerIoWatchers(fd);
}

int EventLoopCoroutine::callLater(quint32 msecs, Functor *callback)
{
    Q_D(EventLoopCoroutine);
    return d->callLater(msecs, callback);
}

void EventLoopCoroutine::callLaterThreadSafe(quint32 msecs, Functor *callback)
{
    Q_D(EventLoopCoroutine);
    d->callLaterThreadSafe(msecs, callback);
}

int EventLoopCoroutine::callRepeat(quint32 msecs, Functor *callback)
{
    Q_D(EventLoopCoroutine);
    return d->callRepeat(msecs, callback);
}

void EventLoopCoroutine::cancelCall(int callbackId)
{
    Q_D(EventLoopCoroutine);
    d->cancelCall(callbackId);
}

int EventLoopCoroutine::exitCode()
{
    Q_D(EventLoopCoroutine);
    return d->exitCode();
}

bool EventLoopCoroutine::runUntil(BaseCoroutine *coroutine)
{
    Q_D(EventLoopCoroutine);
    return d->runUntil(coroutine);
}

bool EventLoopCoroutine::yield()
{
    Q_D(EventLoopCoroutine);
    if (d->loopCoroutine) {
        return d->loopCoroutine->yield();
    }
    ForwardingEventLoopPrivate *fp = dynamic_cast<ForwardingEventLoopPrivate *>(d);
    if (fp && fp->core) {
        return fp->core->yield();
    }
    return BaseCoroutine::yield();
}

EventLoopCoroutine *EventLoopCoroutine::get()
{
    return currentLoopStorageInstance()->getOrCreate().data();
}

QSharedPointer<EventLoopCoroutine> CurrentLoopStorage::getOrCreate()
{
    QSharedPointer<EventLoopCoroutine> eventLoop = get();
    if (eventLoop) {
        return eventLoop;
    }

    qtng_core::EventLoopCoroutine *core = qtng_core::EventLoopCoroutine::get();
    eventLoop = QSharedPointer<EventLoopCoroutine>(new EventLoopCoroutineWrapper(core));
    set(eventLoop);
    return eventLoop;
}

QSharedPointer<EventLoopCoroutine> CurrentLoopStorage::get()
{
    if (storage.hasLocalData()) {
        return storage.localData();
    }
    return QSharedPointer<EventLoopCoroutine>();
}

void CurrentLoopStorage::set(QSharedPointer<EventLoopCoroutine> eventLoop)
{
    storage.setLocalData(eventLoop);
}

void CurrentLoopStorage::clean()
{
    if (storage.hasLocalData()) {
        storage.setLocalData(QSharedPointer<EventLoopCoroutine>());
    }
}

ScopedIoWatcher::ScopedIoWatcher(EventLoopCoroutine::EventType event, qintptr fd)
    : event(event)
    , fd(fd)
    , watcherId(0)
{
}

bool ScopedIoWatcher::start()
{
    EventLoopCoroutine *el = EventLoopCoroutine::get();
    if (watcherId <= 0) {
        watcherId = el->createWatcher(event, fd, new YieldCurrentFunctor());
    }
    el->startWatcher(watcherId);
    return el->yield();
}

ScopedIoWatcher::~ScopedIoWatcher()
{
    if (watcherId > 0) {
        EventLoopCoroutine::get()->removeWatcher(watcherId);
    }
}

#if QTNETWOKRNG_USE_EV
EvEventLoopCoroutine::EvEventLoopCoroutine()
    : EventLoopCoroutine(new ForwardingEventLoopPrivate(this, qtng_core::EventLoopCoroutine::get()))
{
    setObjectName(QString::fromUtf8("libev_eventloop_coroutine"));
}
#endif

#if QTNETWORKNG_USE_WIN
WinEventLoopCoroutine::WinEventLoopCoroutine()
    : EventLoopCoroutine(new ForwardingEventLoopPrivate(this, qtng_core::EventLoopCoroutine::get()))
{
    setObjectName(QString::fromUtf8("win_eventloop_coroutine"));
}
#endif

// QtEventLoopCoroutine is implemented in eventloop_qt.cpp

// ---- Coroutine ----

class CoroutinePrivate
{
public:
    explicit CoroutinePrivate(Coroutine *q)
        : q_ptr(q)
        , callbackId(0)
    {
        q->finished.addCallback([this](BaseCoroutine *) { finishedEvent.set(); });
    }
    CoroutinePrivate(Coroutine *q, QObject *o, const char *slotName)
        : q_ptr(q)
        , obj(o)
        , slot(slotName)
        , callbackId(0)
    {
        q->finished.addCallback([this](BaseCoroutine *) { finishedEvent.set(); });
    }

    Coroutine *q_func() { return q_ptr; }
    const Coroutine *q_func() const { return q_ptr; }

    Coroutine * const q_ptr;
    QPointer<QObject> obj;
    QByteArray slot;
    Event finishedEvent;
    int callbackId;
};

Coroutine::Coroutine(size_t stackSize)
    : BaseCoroutine(nullptr, stackSize)
    , d_ptr(new CoroutinePrivate(this))
{
}

Coroutine::Coroutine(QObject *obj, const char *slot, size_t stackSize)
    : BaseCoroutine(nullptr, stackSize)
    , d_ptr(new CoroutinePrivate(this, obj, slot))
{
}

Coroutine::~Coroutine()
{
    delete d_ptr;
}

void Coroutine::run()
{
    Q_D(Coroutine);
    d->callbackId = 0;
    if (d->obj && !d->slot.isEmpty()) {
        QMetaObject::invokeMethod(d->obj.data(), d->slot.constData(), Qt::DirectConnection);
    }
}

Coroutine *Coroutine::start(quint32 msecs)
{
    qtng_core::Coroutine *cc = dynamic_cast<qtng_core::Coroutine *>(coreFor(this));
    if (cc) {
        cc->start(msecs);
    }
    return this;
}

void Coroutine::kill(CoroutineException *e, quint32 msecs)
{
    qtng_core::Coroutine *cc = dynamic_cast<qtng_core::Coroutine *>(coreFor(this));
    if (!cc) {
        delete e;
        return;
    }
    qtng_core::CoroutineException *coreEx = nullptr;
    if (!e || dynamic_cast<CoroutineExitException *>(e)) {
        coreEx = new qtng_core::CoroutineExitException();
        delete e;
    } else if (dynamic_cast<TimeoutException *>(e)) {
        coreEx = new qtng_core::TimeoutException();
        delete e;
    } else if (dynamic_cast<CoroutineInterruptedException *>(e)) {
        coreEx = new qtng_core::CoroutineInterruptedException();
        delete e;
    } else {
        struct Holder : qtng_core::CoroutineException
        {
            explicit Holder(QTNETWORKNG_NAMESPACE::CoroutineException *q)
                : q(q)
            {
            }
            ~Holder() override { delete q; }
            void raise() override { q->raise(); }
            std::string what() const override { return toStdString(q->what()); }
            qtng_core::CoroutineException *clone() const override { return new Holder(q->clone()); }
            QTNETWORKNG_NAMESPACE::CoroutineException *q;
        };
        coreEx = new Holder(e);
    }
    cc->kill(coreEx, msecs);
}

bool Coroutine::join()
{
    qtng_core::Coroutine *cc = dynamic_cast<qtng_core::Coroutine *>(coreFor(this));
    return cc ? cc->join() : true;
}

void Coroutine::cleanup()
{
    if (previous()) {
        previous()->yield();
    } else {
        EventLoopCoroutine::get()->yield();
    }
}

Coroutine *Coroutine::current()
{
    return dynamic_cast<Coroutine *>(BaseCoroutine::current());
}

void Coroutine::msleep(quint32 msecs)
{
    qtng_core::Coroutine::msleep(msecs);
}

Coroutine *Coroutine::spawn(std::function<void()> f)
{
    class Helper : public Coroutine
    {
    public:
        explicit Helper(std::function<void()> func)
            : func(std::move(func))
        {
        }
        void run() override
        {
            if (func) {
                func();
            }
        }
        std::function<void()> func;
    };
    Coroutine *c = new Helper(std::move(f));
    c->start();
    return c;
}

void Coroutine::preferLibev()
{
    preferLibevFlag.storeRelease(1);
}

// ---- Timeout ----

TimeoutException::TimeoutException() = default;

QString TimeoutException::what() const
{
    return QString::fromLatin1("coroutine had set timeout.");
}

void TimeoutException::raise()
{
    throw *this;
}

CoroutineException *TimeoutException::clone() const
{
    return new TimeoutException();
}

namespace {

struct TimeoutFunctor : public Functor
{
    TimeoutFunctor(Timeout *out, BaseCoroutine *coroutine)
        : out(out)
        , coroutine(coroutine)
    {
    }
    bool operator()() override
    {
        if (!out || !coroutine) {
            return false;
        }
        coroutine->raise(new TimeoutException());
        return true;
    }
    Timeout *out;
    BaseCoroutine *coroutine;
};

}  // namespace

Timeout::Timeout(float secs)
    : msecs(static_cast<quint32>((secs > 0.0f ? secs : 0.0f) * 1000))
    , timeoutId(0)
{
    if (msecs) {
        restart();
    }
}

Timeout::Timeout(quint32 msecs, int)
    : msecs(msecs)
    , timeoutId(0)
{
    if (msecs) {
        restart();
    }
}

Timeout::~Timeout()
{
    cancel();
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
    cancel();
    timeoutId = EventLoopCoroutine::get()->callLater(msecs, new TimeoutFunctor(this, BaseCoroutine::current()));
}

}  // namespace QTNETWORKNG_NAMESPACE

QDebug operator<<(QDebug out, const QTNETWORKNG_NAMESPACE::EventLoopCoroutine &el)
{
    QDebugStateSaver saver(out);
    out.nospace() << "EventLoopCoroutine(" << el.objectName() << ")";
    return out;
}
