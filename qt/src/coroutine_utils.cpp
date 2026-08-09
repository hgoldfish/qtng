#include <QtCore/qprocess.h>
#include <QtCore/qthread.h>

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

bool waitThread(QThread *thread)
{
    if (!thread || !thread->isRunning()) {
        return true;
    }
    return qtng_core::callInThread<bool>([thread]() {
        thread->wait();
        return true;
    });
}

bool waitProcess(QProcess *process)
{
    if (!process) {
        return false;
    }
    return qtng_core::callInThread<bool>([process]() { return process->waitForFinished(-1); });
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
        if (QSharedPointer<Coroutine> c = weak.toStrongRef()) {
            coroutines.remove(c);
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

void CoroutineGroup::deleteCoroutine(BaseCoroutine *coroutine)
{
    if (Coroutine *c = dynamic_cast<Coroutine *>(coroutine)) {
        coroutines.remove(c->sharedFromThis());
    }
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
