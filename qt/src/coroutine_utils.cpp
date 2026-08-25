#include <QtCore/qobject.h>
#include <QtCore/qprocess.h>
#include <QtCore/qthread.h>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

#include "bridge/core_access.h"
#include "coroutine_utils.h"
#include "coroutine.h"
#include "eventloop.h"
#include "private/eventloop_p.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

DeferCallThread::DeferCallThread(std::function<void()> makeResult, QSharedPointer<Event> done,
                                 EventLoopCoroutine *eventloop)
    : makeResult(std::move(makeResult))
    , done(std::move(done))
    , eventloop(eventloop)
{
}

void DeferCallThread::run()
{
    struct Cleanup
    {
        DeferCallThread *self;
        ~Cleanup()
        {
            QPointer<EventLoopCoroutine> loop = self->eventloop;
            if (loop.isNull()) {
                return;
            }
            DeferCallThread *thread = self;
            QSharedPointer<Event> doneEvent = self->done;
            loop->callLaterThreadSafe(0, new LambdaFunctor([doneEvent]() { doneEvent->set(); }));
            loop->callLaterThreadSafe(0, new LambdaFunctor([thread] {
                thread->wait();
                delete thread;
            }));
        }
    } cleanup{this};
    makeResult();
}

class CoroutineThreadPrivate
{
public:
    explicit CoroutineThreadPrivate(quint32 capacity)
        : core(capacity)
    {
    }

    qtng_core::CoroutineThread core;
};

CoroutineThread::CoroutineThread(quint32 capacity)
    : dd_ptr(new CoroutineThreadPrivate(capacity))
{
}

CoroutineThread::~CoroutineThread()
{
    delete dd_ptr;
}

void CoroutineThread::run()
{
    dd_ptr->core.start();
    dd_ptr->core.wait();
}

void CoroutineThread::apply(const std::function<void()> &f)
{
    dd_ptr->core.apply(f);
}

namespace {

bool qtEventLoopIsRunning()
{
    // The current thread's core loop is Qt-backed if and only if a Qt event loop (the one driven
    // by startQtLoop()) is scheduling coroutines here. Use the non-creating accessor: this
    // predicate must not allocate an event loop just to answer "is it running?".
    const std::shared_ptr<qtng_core::EventLoopCoroutine> core = qtng_core::currentLoop()->get();
    return core && dynamic_cast<qtng_core::QtEventLoopCoroutine *>(core.get()) != nullptr;
}

struct DisconnectGuard {
    QMetaObject::Connection a;
    QMetaObject::Connection b;
    ~DisconnectGuard()
    {
        QObject::disconnect(a);
        QObject::disconnect(b);
    }
};

}  // namespace

bool waitThread(QThread *thread)
{
    if (!thread) {
        return false;
    }
    if (!thread->isRunning() || thread->isFinished()) {
        return true;
    }
    if (qtEventLoopIsRunning()) {
        QSharedPointer<ThreadEvent> event = QSharedPointer<ThreadEvent>::create();
        DisconnectGuard guard{
            QObject::connect(thread, &QThread::finished, [event] { event->set(); }),
            QObject::connect(thread, &QThread::destroyed, [event] { event->set(); }),
        };
        if (thread->isFinished()) {
            event->set();
        }
        return event->tryWait();
    }
    return qtng_core::callInThread<bool>([thread]() { return thread->wait(); });
}

bool waitProcess(QProcess *process)
{
    if (!process) {
        return false;
    }
    if (!qtEventLoopIsRunning()) {
        // QProcess does not observe child exit unless a Qt loop is pumping.
#ifdef Q_OS_UNIX
        return qtng_core::waitProcessPid(static_cast<int>(process->processId()));
#else
#  if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const DWORD nativePid = static_cast<DWORD>(process->processId());
        if (!nativePid) {
            return false;
        }
        HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, nativePid);
        if (!handle) {
            return false;
        }
        struct ScopedHandle {
            HANDLE h;
            ~ScopedHandle()
            {
                if (h) {
                    CloseHandle(h);
                }
            }
        } closer{handle};
        return qtng_core::callInThread<bool>(
                [handle] { return WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0; });
#  else
        HANDLE handle = process->pid() ? process->pid()->hProcess : nullptr;
        if (!handle) {
            return false;
        }
        return qtng_core::callInThread<bool>(
                [handle] { return WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0; });
#  endif
#endif
    }
    if (process->state() == QProcess::NotRunning) {
        return true;
    }
    QSharedPointer<ThreadEvent> event = QSharedPointer<ThreadEvent>::create();
    // static_cast disambiguates finished(int, ExitStatus) from finished(int); it works on every Qt the
    // binding supports, so QOverload (Qt 5.7+) is not needed here.
    DisconnectGuard guard{
        QObject::connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                         [event](int, QProcess::ExitStatus) { event->set(); }),
        QObject::connect(process, &QProcess::destroyed, [event] { event->set(); }),
    };
    if (process->state() == QProcess::NotRunning) {
        event->set();
    }
    return event->tryWait();
}

CoroutineGroup::CoroutineGroup() { }

CoroutineGroup::~CoroutineGroup()
{
    killall(true);
}

bool CoroutineGroup::add(QSharedPointer<Coroutine> coroutine, const QString &name)
{
    if (!name.isEmpty()) {
        if (get(name)) {
            return false;
        }
        coroutine->setObjectName(name);
    }
    QWeakPointer<Coroutine> weak = coroutine.toWeakRef();
    coroutine->finished.addCallback([this, weak](BaseCoroutine *) {
        // finished fires synchronously on the coroutine's own stack; dropping the last reference
        // here would destroy the still-running coroutine. deleteCoroutine defers the removal to
        // the event loop instead.
        if (QSharedPointer<Coroutine> c = weak.toStrongRef()) {
            deleteCoroutine(c.data());
        }
    });
    coroutines.insert(coroutine);
    return true;
}

QSharedPointer<Coroutine> CoroutineGroup::get(const QString &name)
{
    for (const QSharedPointer<Coroutine> &coroutine : coroutines) {
        if (coroutine->objectName() == name) {
            return coroutine;
        }
    }
    return QSharedPointer<Coroutine>();
}

bool CoroutineGroup::has(const QString &name)
{
    return !get(name).isNull();
}

bool CoroutineGroup::isCurrent(const QString &name)
{
    for (const QSharedPointer<Coroutine> &coroutine : coroutines) {
        if (coroutine->objectName() == name && coroutine.data() == Coroutine::current()) {
            return true;
        }
    }
    return false;
}

bool CoroutineGroup::kill(const QString &name, bool join)
{
    QSharedPointer<Coroutine> found = get(name);
    if (found.isNull()) {
        return false;
    }
    if (found.data() == Coroutine::current()) {
        return false;
    }
    if (join) {
        if (found->isRunning()) {
            found->kill(new CoroutineExitException());
        } else {
            found->kill();
        }
    } else {
        found->kill();
    }
    return true;
}

bool CoroutineGroup::killall(bool join)
{
    bool done = false;
    const QSet<QSharedPointer<Coroutine>> copy = coroutines;
    for (const QSharedPointer<Coroutine> &coroutine : copy) {
        if (coroutine.data() == Coroutine::current()) {
            continue;
        }
        if (join && coroutine->isRunning()) {
            coroutine->kill(new CoroutineExitException());
        } else {
            coroutine->kill();
        }
        done = true;
    }
    if (join) {
        // kill() only schedules a deferred KillCoroutineFunctor, so we must wait until the coroutine
        // actually finishes; otherwise the coroutine object may be destroyed before the kill timer fires,
        // leading to dangling-pointer crashes (e.g. when CoroutineGroup's QSet releases still-running
        // coroutines on destruction).
        for (const QSharedPointer<Coroutine> &coroutine : copy) {
            if (coroutine.data() == Coroutine::current()) {
                continue;
            }
            if (coroutine->isRunning()) {
                try {
                    coroutine->join();
                } catch (CoroutineException &) {
                }
            }
        }
    }
    return done;
}

bool CoroutineGroup::join(const QString &name)
{
    QSharedPointer<Coroutine> found = get(name);
    if (found.isNull() || found.data() == Coroutine::current()) {
        return false;
    }
    found->join();
    return true;
}

bool CoroutineGroup::joinall()
{
    const bool hasCoroutines = !coroutines.isEmpty();
    const QSet<QSharedPointer<Coroutine>> copy = coroutines;
    for (const QSharedPointer<Coroutine> &coroutine : copy) {
        if (coroutine.data() == Coroutine::current()) {
            continue;
        }
        coroutine->join();
    }
    return hasCoroutines;
}

QSharedPointer<Coroutine> CoroutineGroup::any()
{
    QSharedPointer<ValueEvent<QSharedPointer<Coroutine>>> event =
            QSharedPointer<ValueEvent<QSharedPointer<Coroutine>>>::create();
    QList<QPair<QWeakPointer<Coroutine>, int>> toRemove;
    for (const QSharedPointer<Coroutine> &c : coroutines) {
        QWeakPointer<Coroutine> cw = c.toWeakRef();
        const int callbackId = c->finished.addCallback([event, cw](BaseCoroutine *) {
            event->send(cw.toStrongRef());
        });
        toRemove.append(qMakePair(cw, callbackId));
    }
    try {
        QSharedPointer<Coroutine> result = event->tryWait();
        for (const QPair<QWeakPointer<Coroutine>, int> &item : toRemove) {
            if (QSharedPointer<Coroutine> cc = item.first.toStrongRef()) {
                cc->finished.remove(item.second);
            }
        }
        return result;
    } catch (...) {
        for (const QPair<QWeakPointer<Coroutine>, int> &item : toRemove) {
            if (QSharedPointer<Coroutine> cc = item.first.toStrongRef()) {
                cc->finished.remove(item.second);
            }
        }
        throw;
    }
}

namespace {

class DeleteCoroutineFunctor : public Functor
{
public:
    bool operator()() override { return true; }
    QSharedPointer<Coroutine> coroutine;
};

}  // namespace

void CoroutineGroup::deleteCoroutine(BaseCoroutine *coroutine)
{
    Coroutine *c = dynamic_cast<Coroutine *>(coroutine);
    if (!c) {
        return;
    }
    // Hold the last strong reference until the event loop runs this functor, so the coroutine
    // object is destroyed only after the core coroutine has fully exited its own stack.
    QSharedPointer<Coroutine> keep = c->sharedFromThis();
    DeleteCoroutineFunctor *callback = new DeleteCoroutineFunctor();
    callback->coroutine = keep;
    EventLoopCoroutine::get()->callLater(0, callback);
    coroutines.remove(keep);
}

class ThreadPoolWorkThread : public QThread
{
public:
    void call(function<void()> func)
    {
        QMutexLocker locker(&mutex);
        queue.push_back(std::move(func));
        cond.wakeOne();
    }

    void stop()
    {
        requestInterruption();
        cond.wakeAll();
        wait();
    }

protected:
    void run() override
    {
        while (!isInterruptionRequested()) {
            function<void()> task;
            {
                QMutexLocker locker(&mutex);
                while (queue.isEmpty() && !isInterruptionRequested()) {
                    cond.wait(&mutex);
                }
                if (isInterruptionRequested()) {
                    return;
                }
                task = queue.takeFirst();
            }
            if (task) {
                task();
            }
        }
    }

    QMutex mutex;
    QWaitCondition cond;
    QList<function<void()>> queue;
};

ThreadPool::ThreadPool(int threads)
    : semaphore(new Semaphore(threads <= 0 ? static_cast<int>(QThread::idealThreadCount() * 2 + 1) : threads))
{
}

ThreadPool::~ThreadPool()
{
    for (const QSharedPointer<ThreadPoolWorkThread> &thread : threads) {
        thread->stop();
    }
}

void ThreadPool::call(std::function<void()> func)
{
    ScopedLock<Semaphore> lock(*semaphore);
    if (!lock.isSuccess()) {
        return;
    }
    QSharedPointer<ThreadPoolWorkThread> thread;
    if (threads.isEmpty()) {
        thread = QSharedPointer<ThreadPoolWorkThread>::create();
        thread->start();
        threads.append(thread);
    } else {
        thread = threads.takeFirst();
    }
    thread->call(std::move(func));
    threads.append(thread);
}

}  // namespace QTNETWORKNG_NAMESPACE
