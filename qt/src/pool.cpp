#include "bridge/core_access.h"
#include "pool.h"
#include "private/eventloop_p.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class ChannelPrivate
{
public:
    ValueEvent<QVariant> event;
};

Channel::Channel()
    : d_ptr(new ChannelPrivate)
{
}

Channel::~Channel()
{
    delete d_ptr;
}

void Channel::send(const QVariant &obj)
{
    Q_D(Channel);
    d->event.send(obj);
}

QVariant Channel::recv()
{
    Q_D(Channel);
    return d->event.tryWait();
}

EventLoopThread::EventLoopThread()
    : operations(new CoroutineGroup)
    , mIdleEvent(new Event())
{
}

EventLoopThread::~EventLoopThread()
{
    killall();
    wait(5000);
    delete operations;
}

bool EventLoopThread::isIdle()
{
    return operations->isEmpty();
}

bool EventLoopThread::isReady()
{
    return eventLoop && isRunning();
}

QSharedPointer<Event> EventLoopThread::idleEvent()
{
    return mIdleEvent;
}

bool EventLoopThread::kill(const QString &name)
{
    return operations->kill(name);
}

bool EventLoopThread::killall()
{
    return operations->killall();
}

int EventLoopThread::size() const
{
    return operations->size();
}

bool EventLoopThread::isEmpty() const
{
    return operations->isEmpty();
}

void EventLoopThread::spawnWithName(const QString &name, const std::function<void()> &func, bool replace)
{
    operations->spawnWithName(name, func, replace);
}

void EventLoopThread::spawn(const std::function<void()> &func)
{
    operations->spawn(func);
}

void EventLoopThread::run()
{
    createEventLoop();
    if (!eventLoop) {
        return;
    }
    Coroutine *runner = Coroutine::spawn([this]() { eventLoop->run(); });
    if (runner) {
        runner->join();
    }
}

void EvEventLoopThread::createEventLoop()
{
    eventLoop = currentLoop()->getOrCreate();
}

void QtEventLoopThread::createEventLoop()
{
    // This thread is driven by the Qt event loop backend explicitly (the core factory only hands
    // out the Qt backend on the GUI thread). Pin the core loop in the core thread-local storage
    // first (it is the single ownership source), then wrap it for the qt API layer.
    qtng_core::currentLoop()->set(std::shared_ptr<qtng_core::EventLoopCoroutine>(new qtng_core::QtEventLoopCoroutine()));
    eventLoop = wrapCoreLoop(qtng_core::currentLoop()->get().get());
    currentLoop()->set(eventLoop);
}

class EventLoopPoolPrivate
{
public:
    explicit EventLoopPoolPrivate(int maxThreads)
        : maxThreads(maxThreads)
        , nextThread(0)
    {
    }

    int maxThreads;
    int nextThread;
    QList<QSharedPointer<EventLoopThread>> threads;
};

EventLoopPool::EventLoopPool(int maxThreads)
    : d_ptr(new EventLoopPoolPrivate(maxThreads))
{
    Q_D(EventLoopPool);
    for (int i = 0; i < d->maxThreads; ++i) {
        QSharedPointer<EvEventLoopThread> thread(new EvEventLoopThread());
        thread->start();
        d->threads.append(thread);
    }
}

EventLoopPool::~EventLoopPool()
{
    Q_D(EventLoopPool);
    for (const QSharedPointer<EventLoopThread> &thread : d->threads) {
        thread->killall();
        thread->wait(5000);
    }
    delete d_ptr;
}

template<typename T, typename S>
QList<T> EventLoopPool::applyList(std::function<T(S)> func, const QList<S> &l)
{
    Q_D(EventLoopPool);
    QSharedPointer<EventLoopThread> thread = d->threads.at(d->nextThread % d->threads.size());
    d->nextThread++;
    return CoroutineGroup::map<T, S>(func, l);
}

template<typename S>
void EventLoopPool::applyEach(std::function<void(S)> func, const QList<S> &l)
{
    Q_D(EventLoopPool);
    QSharedPointer<EventLoopThread> thread = d->threads.at(d->nextThread % d->threads.size());
    d->nextThread++;
    Q_UNUSED(thread);
    CoroutineGroup::each<S>(func, l);
}

template<typename T, typename S>
T EventLoopPool::apply(std::function<T(S)> func, S s)
{
    Q_D(EventLoopPool);
    QSharedPointer<EventLoopThread> thread = d->threads.at(d->nextThread % d->threads.size());
    d->nextThread++;
    QSharedPointer<T> result(new T());
    thread->spawn([result, func, s]() { *result = func(s); });
    thread->killall();
    return *result;
}

}  // namespace QTNETWORKNG_NAMESPACE

#include "pool.h"

namespace QTNETWORKNG_NAMESPACE {

template QList<int> EventLoopPool::applyList<int, int>(std::function<int(int)> func, const QList<int> &l);
template void EventLoopPool::applyEach<int>(std::function<void(int)> func, const QList<int> &l);

template<typename T, typename S>
QList<T> EventLoopThread::map(std::function<T(S)> func, const QList<S> &l)
{
    return CoroutineGroup::map<T, S>(func, l);
}

template<typename S>
void EventLoopThread::each(std::function<void(S)> func, const QList<S> &l)
{
    CoroutineGroup::each<S>(func, l);
}

}  // namespace QTNETWORKNG_NAMESPACE
