#include <QtCore/qmap.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qthread.h>
#include <QtCore/qsocketnotifier.h>
#include <QtCore/qtimer.h>
#include <QtCore/qpointer.h>
#include <QtCore/qcoreevent.h>
#include <QtCore/qdebug.h>
#include <memory>

#include "bridge/core_access.h"
#include "bridge/eventloop_qt_core_p.h"
#include "eventloop.h"
#include "private/eventloop_p.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

// ===== core-level Qt event loop backend =====
// Ported from qtnetworkng 1.0 (eventloop_qt.cpp). This backend lets the core coroutine scheduler
// run on the Qt event loop: io readiness is watched with QSocketNotifier and timers with
// QObject::startTimer(), so QCoreApplication::exec() drives all spawned coroutines.
namespace qtng_core {

namespace {

struct QtWatcher
{
    virtual ~QtWatcher() = default;
};

struct QtIoWatcher : public QtWatcher
{
    QtIoWatcher(intptr_t fd, EventLoopCoroutine::EventType event, Functor *callback)
        : callback(callback)
        , fd(fd)
        , event(event)
    {
    }
    ~QtIoWatcher() override { delete callback; }

    QSharedPointer<QSocketNotifier> readNotifier;
    QSharedPointer<QSocketNotifier> writeNotifier;
    Functor *callback;
    intptr_t fd;
    EventLoopCoroutine::EventType event;
};

struct QtTimerWatcher : public QtWatcher
{
    QtTimerWatcher(uint32_t interval, bool singleshot, Functor *callback)
        : callback(callback)
        , interval(interval)
        , singleshot(singleshot)
    {
    }
    ~QtTimerWatcher() override { delete callback; }

    Functor *callback;
    int timerId;
    uint32_t interval;
    bool singleshot;
};

}  // namespace

class QtEventLoopCoroutinePrivate : public EventLoopCoroutinePrivate
{
public:
    explicit QtEventLoopCoroutinePrivate(EventLoopCoroutine *q);
    ~QtEventLoopCoroutinePrivate() override;
    void run() override;
    int createWatcher(EventLoopCoroutine::EventType event, intptr_t fd, Functor *callback) override;
    void startWatcher(int watcherId) override;
    void stopWatcher(int watcherId) override;
    void removeWatcher(int watcherId) override;
    void triggerIoWatchers(intptr_t fd) override;
    int callLater(uint32_t msecs, Functor *callback) override;
    void callLaterThreadSafe(uint32_t msecs, Functor *callback) override;
    int callRepeat(uint32_t msecs, Functor *callback) override;
    void cancelCall(int callbackId) override;
    int exitCode() override;
    bool runUntil(BaseCoroutine *coroutine) override;
    void timerEvent(QTimerEvent *event);
    bool handleIoEvent(int socket, QSocketNotifier *n);

    QMap<int, QtWatcher *> watchers;
    QMap<int, int> timers;
    int nextWatcherId;
    int qtExitCode;
    QtEventLoopHelper *helper;

    static QtEventLoopCoroutinePrivate *getPrivateHelper(EventLoopCoroutine *coroutine)
    {
        return static_cast<QtEventLoopCoroutinePrivate *>(EventLoopCoroutinePrivate::getPrivateHelper(coroutine));
    }

    friend class ::qtng::EventLoopCoroutineWrapper;
    friend int ::qtng::startQtLoop();
};

struct QtTriggerIoWatchersFunctor : public Functor
{
    QtTriggerIoWatchersFunctor(int watcherId, EventLoopCoroutine *eventloop)
        : eventloop(eventloop)
        , watcherId(watcherId)
    {
    }
    ~QtTriggerIoWatchersFunctor() override = default;
    bool operator()() override;
    EventLoopCoroutine *eventloop;
    int watcherId;
};

bool QtTriggerIoWatchersFunctor::operator()()
{
    if (!eventloop) {
        return false;
    }
    QtEventLoopCoroutinePrivate *d = QtEventLoopCoroutinePrivate::getPrivateHelper(eventloop);
    QtIoWatcher *w = dynamic_cast<QtIoWatcher *>(d->watchers.value(watcherId));
    if (w) {
        return (*w->callback)();
    }
    return false;
}

QtEventLoopHelper::QtEventLoopHelper(QtEventLoopCoroutinePrivate *parent)
    : parent(parent)
{
}

void QtEventLoopHelper::timerEvent(QTimerEvent *event)
{
    parent->timerEvent(event);
}

void QtEventLoopHelper::callLaterThreadSafeStub(uint32_t msecs, void *callback)
{
    parent->callLater(msecs, static_cast<Functor *>(callback));
}

void QtEventLoopHelper::handleIoEvent(int socket)
{
    QSocketNotifier *n = dynamic_cast<QSocketNotifier *>(sender());
    if (n && !parent->handleIoEvent(socket, n)) {
        // prevent cpu 100%
        n->setEnabled(false);
    }
}

QtEventLoopCoroutinePrivate::QtEventLoopCoroutinePrivate(EventLoopCoroutine *q)
    : EventLoopCoroutinePrivate(q)
    , nextWatcherId(1)
    , qtExitCode(0)
    , helper(new QtEventLoopHelper(this))
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
    QtIoWatcher *w = static_cast<QtIoWatcher *>(n->property("parent").value<void *>());
    return w && (*w->callback)();
}

int QtEventLoopCoroutinePrivate::createWatcher(EventLoopCoroutine::EventType event, intptr_t fd, Functor *callback)
{
    QtIoWatcher *w = new QtIoWatcher(fd, event, callback);
    watchers.insert(nextWatcherId, w);
    return nextWatcherId++;
}

void QtEventLoopCoroutinePrivate::startWatcher(int watcherId)
{
    QtIoWatcher *w = dynamic_cast<QtIoWatcher *>(watchers.value(watcherId));
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
    QtIoWatcher *w = dynamic_cast<QtIoWatcher *>(watchers.value(watcherId));
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

void QtEventLoopCoroutinePrivate::triggerIoWatchers(intptr_t fd)
{
    for (QMap<int, QtWatcher *>::const_iterator it = watchers.constBegin(); it != watchers.constEnd(); ++it) {
        QtIoWatcher *w = dynamic_cast<QtIoWatcher *>(it.value());
        if (w && w->fd == fd) {
            if (!w->readNotifier.isNull()) {
                w->readNotifier->setEnabled(false);
            }
            if (!w->writeNotifier.isNull()) {
                w->writeNotifier->setEnabled(false);
            }
            callLater(0, new QtTriggerIoWatchersFunctor(it.key(), q_ptr));
        }
    }
}

void QtEventLoopCoroutinePrivate::timerEvent(QTimerEvent *event)
{
    if (!timers.contains(event->timerId())) {
        return;
    }
    const int watcherId = timers.value(event->timerId());
    QtTimerWatcher *watcher = dynamic_cast<QtTimerWatcher *>(watchers.value(watcherId));
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

int QtEventLoopCoroutinePrivate::callLater(uint32_t msecs, Functor *callback)
{
    QtTimerWatcher *w = new QtTimerWatcher(msecs, true, callback);
    w->timerId = helper->startTimer(static_cast<int>(msecs), Qt::PreciseTimer);
    watchers.insert(nextWatcherId, w);
    timers.insert(w->timerId, nextWatcherId);
    return nextWatcherId++;
}

void QtEventLoopCoroutinePrivate::callLaterThreadSafe(uint32_t msecs, Functor *callback)
{
    QMetaObject::invokeMethod(helper, "callLaterThreadSafeStub", Qt::QueuedConnection, Q_ARG(quint32, msecs),
                              Q_ARG(void *, callback));
}

int QtEventLoopCoroutinePrivate::callRepeat(uint32_t msecs, Functor *callback)
{
    QtTimerWatcher *w = new QtTimerWatcher(msecs, false, callback);
    w->timerId = helper->startTimer(static_cast<int>(msecs));
    watchers.insert(nextWatcherId, w);
    timers.insert(w->timerId, nextWatcherId);
    return nextWatcherId++;
}

void QtEventLoopCoroutinePrivate::cancelCall(int callbackId)
{
    QtTimerWatcher *w = dynamic_cast<QtTimerWatcher *>(watchers.take(callbackId));
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
    setObjectName("qt_eventloop_coroutine");
}

}  // namespace qtng_core

namespace {

std::shared_ptr<qtng_core::EventLoopCoroutine> createQtCoreLoop()
{
    // The Qt backend needs a QCoreApplication and must run on the thread that owns it
    // (QSocketNotifier/QTimer are QObjects bound to the GUI thread).
    QCoreApplication *app = QCoreApplication::instance();
    if (app && app->thread() == QThread::currentThread()) {
        return std::shared_ptr<qtng_core::EventLoopCoroutine>(new qtng_core::QtEventLoopCoroutine());
    }
    return nullptr;
}

struct QtLoopFactoryAutoRegister
{
    QtLoopFactoryAutoRegister()
    {
        // Default backend (no useEventloop() call): the GUI thread gets a Qt event loop
        // automatically, so coroutines are driven by QCoreApplication::exec() without extra setup.
        qtng_core::setEventLoopFactory(createQtCoreLoop);
        // Explicit qtng::useEventloop(EventLoopQt) / qtng::useQtEventloop().
        qtng_core::registerEventLoop(qtng_core::EventLoopType::Qt, createQtCoreLoop);
    }
};

const QtLoopFactoryAutoRegister qtLoopFactoryAutoRegister;

}  // namespace

namespace QTNETWORKNG_NAMESPACE {

void useQtEventloop()
{
    qtng_core::useEventloop(qtng_core::EventLoopType::Qt);
}

void useEventloop(EventLoopType type)
{
    qtng_core::useEventloop(static_cast<qtng_core::EventLoopType>(type));
}

int startQtLoop()
{
    if (!QCoreApplication::instance()) {
        qFatal("Qt eventloop require QCoreApplication.");
    }

    // Run the coroutine scheduler on the Qt event loop: make sure the current core loop is the
    // Qt-backed one (qtng_core::QtEventLoopCoroutine). Normally the factory registered below
    // already created it on first use on the GUI thread; if a libev/Win loop was created
    // beforehand (e.g. qtng::useEventloop(Ev) or coroutines were first used on a non-GUI thread),
    // there is no Qt loop to run here.
    qtng_core::EventLoopCoroutine *coreLoop = qtng_core::EventLoopCoroutine::get();
    qtng_core::QtEventLoopCoroutine *qtLoop = dynamic_cast<qtng_core::QtEventLoopCoroutine *>(coreLoop);
    if (!qtLoop) {
        qWarning() << "startQtLoop: current event loop is not the Qt backend. "
                      "qtng::useEventloop(Ev) was called, coroutines were used before startQtLoop() "
                      "(e.g. on a non-GUI thread), or the Qt event loop factory was not registered.";
        return -1;
    }

    qtng_core::QtEventLoopCoroutinePrivate *priv = qtng_core::QtEventLoopCoroutinePrivate::getPrivateHelper(qtLoop);
    priv->loopCoroutine = qtng_core::BaseCoroutine::current();
    const int result = QCoreApplication::instance()->exec();
    QCoreApplication::instance()->processEvents();
    priv->loopCoroutine = nullptr;
    return result;
}

}  // namespace QTNETWORKNG_NAMESPACE
