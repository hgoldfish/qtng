#include <QtCore/qmap.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qthread.h>
#include <QtCore/qsocketnotifier.h>
#include <QtCore/qtimer.h>
#include <QtCore/qpointer.h>
#include <QtCore/qcoreevent.h>

#include "bridge/core_access.h"
#include "eventloop.h"
#include "private/eventloop_p.h"
#include "eventloop_qt_p.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

namespace {

struct QtWatcher
{
    virtual ~QtWatcher();
};

struct IoWatcher : public QtWatcher
{
    IoWatcher(qintptr fd, EventLoopCoroutine::EventType event, Functor *callback);
    ~IoWatcher() override;

    QSharedPointer<QSocketNotifier> readNotifier;
    QSharedPointer<QSocketNotifier> writeNotifier;
    Functor *callback;
    qintptr fd;
    EventLoopCoroutine::EventType event;
};

struct TimerWatcher : public QtWatcher
{
    TimerWatcher(quint32 interval, bool singleshot, Functor *callback);
    ~TimerWatcher() override;

    Functor *callback;
    int timerId;
    quint32 interval;
    bool singleshot;
};

QtWatcher::~QtWatcher() { }

IoWatcher::IoWatcher(qintptr fd, EventLoopCoroutine::EventType event, Functor *callback)
    : callback(callback)
    , fd(fd)
    , event(event)
{
}

IoWatcher::~IoWatcher()
{
    delete callback;
}

TimerWatcher::TimerWatcher(quint32 interval, bool singleshot, Functor *callback)
    : callback(callback)
    , interval(interval)
    , singleshot(singleshot)
{
}

TimerWatcher::~TimerWatcher()
{
    delete callback;
}

}  // namespace

class QtEventLoopCoroutinePrivate : public EventLoopCoroutinePrivate
{
public:
    explicit QtEventLoopCoroutinePrivate(EventLoopCoroutine *q);
    ~QtEventLoopCoroutinePrivate() override;
    void run() override;
    int createWatcher(EventLoopCoroutine::EventType event, qintptr fd, Functor *callback) override;
    void startWatcher(int watcherId) override;
    void stopWatcher(int watcherId) override;
    void removeWatcher(int watcherId) override;
    void triggerIoWatchers(qintptr fd) override;
    int callLater(quint32 msecs, Functor *callback) override;
    void callLaterThreadSafe(quint32 msecs, Functor *callback) override;
    int callRepeat(quint32 msecs, Functor *callback) override;
    void cancelCall(int callbackId) override;
    int exitCode() override;
    bool runUntil(BaseCoroutine *coroutine) override;
    void timerEvent(QTimerEvent *event);
    bool handleIoEvent(int socket, QSocketNotifier *n);

    QMap<int, QtWatcher *> watchers;
    QMap<int, int> timers;
    int nextWatcherId;
    int qtExitCode;
    EventLoopCoroutinePrivateQtHelper *helper;

    static QtEventLoopCoroutinePrivate *getPrivateHelper(EventLoopCoroutine *coroutine)
    {
        return static_cast<QtEventLoopCoroutinePrivate *>(EventLoopCoroutinePrivate::getPrivateHelper(coroutine));
    }

    friend class EventLoopCoroutineWrapper;

    friend int startQtLoop();
};

struct TriggerIoWatchersArgumentsFunctor : public Functor
{
    TriggerIoWatchersArgumentsFunctor(int watcherId, EventLoopCoroutine *eventloop)
        : eventloop(eventloop)
        , watcherId(watcherId)
    {
    }
    ~TriggerIoWatchersArgumentsFunctor() override = default;
    EventLoopCoroutine *eventloop;
    int watcherId;
    bool operator()() override;
};

bool TriggerIoWatchersArgumentsFunctor::operator()()
{
    if (!eventloop) {
        return false;
    }
    QtEventLoopCoroutinePrivate *d = QtEventLoopCoroutinePrivate::getPrivateHelper(eventloop);
    IoWatcher *w = dynamic_cast<IoWatcher *>(d->watchers.value(watcherId));
    if (w) {
        return (*w->callback)();
    }
    return false;
}

EventLoopCoroutinePrivateQtHelper::EventLoopCoroutinePrivateQtHelper(QtEventLoopCoroutinePrivate *parent)
    : parent(parent)
{
}

void EventLoopCoroutinePrivateQtHelper::timerEvent(QTimerEvent *event)
{
    parent->timerEvent(event);
}

void EventLoopCoroutinePrivateQtHelper::callLaterThreadSafeStub(quint32 msecs, void *callback)
{
    parent->callLater(msecs, static_cast<Functor *>(callback));
}

void EventLoopCoroutinePrivateQtHelper::handleIoEvent(int socket)
{
    QSocketNotifier *n = dynamic_cast<QSocketNotifier *>(sender());
    if (!parent->handleIoEvent(socket, n)) {
        n->setEnabled(false);
    }
}

QtEventLoopCoroutinePrivate::QtEventLoopCoroutinePrivate(EventLoopCoroutine *q)
    : EventLoopCoroutinePrivate(q)
    , nextWatcherId(1)
    , qtExitCode(0)
    , helper(new EventLoopCoroutinePrivateQtHelper(this))
{
}

QtEventLoopCoroutinePrivate::~QtEventLoopCoroutinePrivate()
{
    qDeleteAll(watchers);
    delete helper;
}

void QtEventLoopCoroutinePrivate::run()
{
    QEventLoop localLoop;
    qtExitCode = localLoop.exec();
}

bool QtEventLoopCoroutinePrivate::handleIoEvent(int, QSocketNotifier *n)
{
    if (!n) {
        return false;
    }
    IoWatcher *w = static_cast<IoWatcher *>(n->property("parent").value<void *>());
    return w && (*w->callback)();
}

int QtEventLoopCoroutinePrivate::createWatcher(EventLoopCoroutine::EventType event, qintptr fd, Functor *callback)
{
    IoWatcher *w = new IoWatcher(fd, event, callback);
    watchers.insert(nextWatcherId, w);
    return nextWatcherId++;
}

void QtEventLoopCoroutinePrivate::startWatcher(int watcherId)
{
    IoWatcher *w = dynamic_cast<IoWatcher *>(watchers.value(watcherId));
    if (!w) {
        return;
    }
    if (w->event & EventLoopCoroutine::Read) {
        if (w->readNotifier.isNull()) {
            w->readNotifier.reset(new QSocketNotifier(static_cast<qintptr>(w->fd), QSocketNotifier::Read));
            w->readNotifier->setProperty("parent", QVariant::fromValue(static_cast<void *>(w)));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            QObject::connect(w->readNotifier.data(), &QSocketNotifier::activated, helper,
                             [this](QSocketDescriptor, QSocketNotifier::Type) { helper->handleIoEvent(0); },
                             Qt::DirectConnection);
#else
            QObject::connect(w->readNotifier.data(), SIGNAL(activated(int)), helper, SLOT(handleIoEvent(int)),
                             Qt::DirectConnection);
#endif
        }
        w->readNotifier->setEnabled(true);
    }
    if (w->event & EventLoopCoroutine::Write) {
        if (w->writeNotifier.isNull()) {
            w->writeNotifier.reset(new QSocketNotifier(static_cast<qintptr>(w->fd), QSocketNotifier::Write));
            w->writeNotifier->setProperty("parent", QVariant::fromValue(static_cast<void *>(w)));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            QObject::connect(w->writeNotifier.data(), &QSocketNotifier::activated, helper,
                             [this](QSocketDescriptor, QSocketNotifier::Type) { helper->handleIoEvent(0); },
                             Qt::DirectConnection);
#else
            QObject::connect(w->writeNotifier.data(), SIGNAL(activated(int)), helper, SLOT(handleIoEvent(int)),
                             Qt::DirectConnection);
#endif
        }
        w->writeNotifier->setEnabled(true);
    }
}

void QtEventLoopCoroutinePrivate::stopWatcher(int watcherId)
{
    IoWatcher *w = dynamic_cast<IoWatcher *>(watchers.value(watcherId));
    if (!w) {
        return;
    }
    if (!w->readNotifier.isNull()) {
        w->readNotifier->setEnabled(false);
    }
    if (!w->writeNotifier.isNull()) {
        w->writeNotifier->setEnabled(false);
    }
}

void QtEventLoopCoroutinePrivate::removeWatcher(int watcherId)
{
    delete watchers.take(watcherId);
}

void QtEventLoopCoroutinePrivate::triggerIoWatchers(qintptr fd)
{
    for (QMap<int, QtWatcher *>::const_iterator it = watchers.constBegin(); it != watchers.constEnd(); ++it) {
        IoWatcher *w = dynamic_cast<IoWatcher *>(it.value());
        if (w && w->fd == fd) {
            if (!w->readNotifier.isNull()) {
                w->readNotifier->setEnabled(false);
            }
            if (!w->writeNotifier.isNull()) {
                w->writeNotifier->setEnabled(false);
            }
            callLater(0, new TriggerIoWatchersArgumentsFunctor(it.key(), q_ptr));
        }
    }
}

void QtEventLoopCoroutinePrivate::timerEvent(QTimerEvent *event)
{
    if (!timers.contains(event->timerId())) {
        return;
    }
    const int watcherId = timers.value(event->timerId());
    TimerWatcher *watcher = dynamic_cast<TimerWatcher *>(watchers.value(watcherId));
    if (!watcher) {
        return;
    }
    const bool singleshot = watcher->singleshot;
    if (singleshot) {
        watchers.remove(watcherId);
        timers.remove(event->timerId());
        helper->killTimer(event->timerId());
    }
    (*watcher->callback)();
    if (singleshot) {
        delete watcher;
    }
}

int QtEventLoopCoroutinePrivate::callLater(quint32 msecs, Functor *callback)
{
    TimerWatcher *w = new TimerWatcher(msecs, true, callback);
    w->timerId = helper->startTimer(static_cast<int>(msecs), Qt::PreciseTimer);
    watchers.insert(nextWatcherId, w);
    timers.insert(w->timerId, nextWatcherId);
    return nextWatcherId++;
}

void QtEventLoopCoroutinePrivate::callLaterThreadSafe(quint32 msecs, Functor *callback)
{
    QMetaObject::invokeMethod(helper, "callLaterThreadSafeStub", Qt::QueuedConnection, Q_ARG(quint32, msecs),
                              Q_ARG(void *, callback));
}

int QtEventLoopCoroutinePrivate::callRepeat(quint32 msecs, Functor *callback)
{
    TimerWatcher *w = new TimerWatcher(msecs, false, callback);
    w->timerId = helper->startTimer(static_cast<int>(msecs));
    watchers.insert(nextWatcherId, w);
    timers.insert(w->timerId, nextWatcherId);
    return nextWatcherId++;
}

void QtEventLoopCoroutinePrivate::cancelCall(int callbackId)
{
    TimerWatcher *w = dynamic_cast<TimerWatcher *>(watchers.take(callbackId));
    if (!w) {
        return;
    }
    timers.remove(w->timerId);
    helper->killTimer(w->timerId);
    delete w;
}

int QtEventLoopCoroutinePrivate::exitCode()
{
    return qtExitCode;
}

bool QtEventLoopCoroutinePrivate::runUntil(BaseCoroutine *coroutine)
{
    BaseCoroutine *current = BaseCoroutine::current();
    if (loopCoroutine && loopCoroutine != current) {
        Deferred<BaseCoroutine *>::Callback returnHere = [current](BaseCoroutine *) {
            if (current) {
                current->yield();
            }
        };
        const int callbackId = coroutine->finished.addCallback(returnHere);
        loopCoroutine->yield();
        coroutine->finished.remove(callbackId);
    } else {
        BaseCoroutine *old = loopCoroutine;
        loopCoroutine = current;
        QSharedPointer<QEventLoop> sub(new QEventLoop());
        constexpr int SubEventLoopExitValue = 9527;
        Deferred<BaseCoroutine *>::Callback shutdown = [sub](BaseCoroutine *) { sub->exit(SubEventLoopExitValue); };
        const int callbackId = coroutine->finished.addCallback(shutdown);
        const int exitValue = sub->exec();
        coroutine->finished.remove(callbackId);
        loopCoroutine = old;
        return exitValue == SubEventLoopExitValue;
    }
    return true;
}

QtEventLoopCoroutine::QtEventLoopCoroutine()
    : EventLoopCoroutine(new QtEventLoopCoroutinePrivate(this))
{
    setObjectName(QString::fromUtf8("qt_eventloop_coroutine"));
}

int startQtLoop()
{
    if (!QCoreApplication::instance()) {
        qFatal("Qt eventloop require QCoreApplication.");
    }

    QSharedPointer<EventLoopCoroutine> eventLoop = currentLoop()->get();
    QtEventLoopCoroutine *qtEventLoop = nullptr;
    if (eventLoop) {
        qtEventLoop = dynamic_cast<QtEventLoopCoroutine *>(eventLoop.data());
        if (!qtEventLoop) {
            return -1;
        }
    } else {
        qtEventLoop = new QtEventLoopCoroutine();
        currentLoop()->set(QSharedPointer<EventLoopCoroutine>(qtEventLoop));
    }

    QtEventLoopCoroutinePrivate *priv = QtEventLoopCoroutinePrivate::getPrivateHelper(qtEventLoop);
    priv->loopCoroutine = BaseCoroutine::current();
    const int result = QCoreApplication::instance()->exec();
    QCoreApplication::instance()->processEvents();
    priv->loopCoroutine.clear();
    return result;
}

}  // namespace QTNETWORKNG_NAMESPACE

#include "eventloop_qt.moc"
