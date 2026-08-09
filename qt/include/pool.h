#ifndef QTNG_POOL_H
#define QTNG_POOL_H

#include <functional>
#include <QtCore/qthread.h>
#include <QtCore/qsharedpointer.h>
#include <QtCore/qlist.h>
#include <QtCore/qvariant.h>

#include "coroutine_utils.h"
#include "eventloop.h"
#include "locks.h"

QTNETWORKNG_NAMESPACE_BEGIN

class EventLoopThread : public QThread
{
public:
    EventLoopThread();
    virtual ~EventLoopThread() override;
public:
    bool isIdle();
    bool isReady();
    QSharedPointer<Event> idleEvent();
public:
    bool kill(const QString &name);
    bool killall();
    int size() const;
    bool isEmpty() const;
    void spawnWithName(const QString &name, const std::function<void()> &func, bool replace = false);
    void spawn(const std::function<void()> &func);
    template<typename T, typename S>
    QList<T> map(std::function<T(S)> func, const QList<S> &l);
    template<typename S>
    void each(std::function<void(S)> func, const QList<S> &l);
protected:
    void run() override;
    virtual void createEventLoop() = 0;
    QSharedPointer<EventLoopCoroutine> eventLoop;
private:
    CoroutineGroup *operations;
    QSharedPointer<Event> mIdleEvent;
};

class EvEventLoopThread : public EventLoopThread
{
protected:
    virtual void createEventLoop() override;
};

class QtEventLoopThread : public EventLoopThread
{
protected:
    virtual void createEventLoop() override;
};

class ChannelPrivate;
class Channel
{
public:
    Channel();
    ~Channel();
public:
    void send(const QVariant &obj);
    QVariant recv();
private:
    ChannelPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(Channel)
};

class EventLoopPoolPrivate;
class EventLoopPool
{
public:
    explicit EventLoopPool(int maxThreads);
    ~EventLoopPool();
public:
    template<typename T, typename S>
    QList<T> map(std::function<T(S)> func, const QList<S> &l)
    {
        return applyList<T, S>(func, l);
    }
    template<typename S>
    void each(std::function<void(S)> func, const QList<S> &l)
    {
        applyEach<S>(func, l);
    }
    template<typename T, typename S>
    T apply(std::function<T(S)> func, S s);
private:
    template<typename T, typename S>
    QList<T> applyList(std::function<T(S)> func, const QList<S> &l);
    template<typename S>
    void applyEach(std::function<void(S)> func, const QList<S> &l);
    EventLoopPoolPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(EventLoopPool)
};

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_POOL_H
