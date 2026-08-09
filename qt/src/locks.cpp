#include "bridge/core_access.h"
#include "locks.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class SemaphorePrivate
{
public:
    explicit SemaphorePrivate(int value)
        : core(value)
    {
    }

    qtng_core::Semaphore core;
};

Semaphore::Semaphore(int value)
    : d(new SemaphorePrivate(value))
{
}

Semaphore::~Semaphore() = default;

bool Semaphore::acquireMany(int value, quint32 msecs)
{
    return d->core.acquireMany(value, msecs);
}

bool Semaphore::tryAcquire(quint32 msecs)
{
    return d->core.tryAcquire(msecs);
}

void Semaphore::release(int value)
{
    d->core.release(value);
}

bool Semaphore::isLocked() const
{
    return d->core.isLocked();
}

bool Semaphore::isUsed() const
{
    return d->core.isUsed();
}

quint32 Semaphore::getting() const
{
    return d->core.getting();
}

Lock::Lock()
    : Semaphore(1)
{
}

class RLockPrivate
{
public:
    qtng_core::RLock core;
    Q_DECLARE_PUBLIC(RLock)
    RLock *q_ptr;
    explicit RLockPrivate(RLock *q)
        : q_ptr(q)
    {
    }
};

RLock::RLock()
    : d_ptr(new RLockPrivate(this))
{
}

RLock::~RLock()
{
    delete d_ptr;
}

bool RLock::tryAcquire(quint32 msecs)
{
    Q_D(RLock);
    return d->core.tryAcquire(msecs);
}

void RLock::release()
{
    Q_D(RLock);
    d->core.release();
}

bool RLock::isLocked() const
{
    Q_D(const RLock);
    return d->core.isLocked();
}

bool RLock::isOwned() const
{
    Q_D(const RLock);
    return d->core.isOwned();
}

class ConditionPrivate
{
public:
    qtng_core::Condition core;
    Q_DECLARE_PUBLIC(Condition)
    Condition *q_ptr;
    explicit ConditionPrivate(Condition *q)
        : q_ptr(q)
    {
    }
};

Condition::Condition()
    : d_ptr(new ConditionPrivate(this))
{
}

Condition::~Condition()
{
    delete d_ptr;
}

bool Condition::wait(quint32 msecs)
{
    Q_D(Condition);
    return d->core.wait(msecs);
}

void Condition::notify(int value)
{
    Q_D(Condition);
    d->core.notify(value);
}

void Condition::notifyAll()
{
    Q_D(Condition);
    d->core.notifyAll();
}

quint32 Condition::getting() const
{
    Q_D(const Condition);
    return d->core.getting();
}

class EventPrivate
{
public:
    qtng_core::Event core;
    Q_DECLARE_PUBLIC(Event)
    Event *q_ptr;
    explicit EventPrivate(Event *q)
        : q_ptr(q)
    {
    }
};

Event::Event()
    : d_ptr(new EventPrivate(this))
{
}

Event::~Event()
{
    delete d_ptr;
}

bool Event::tryWait(quint32 msecs)
{
    Q_D(Event);
    return d->core.tryWait(msecs);
}

void Event::set()
{
    Q_D(Event);
    d->core.set();
}

void Event::clear()
{
    Q_D(Event);
    d->core.clear();
}

bool Event::isSet() const
{
    Q_D(const Event);
    return d->core.isSet();
}

quint32 Event::getting() const
{
    Q_D(const Event);
    return d->core.getting();
}

void Event::link(Event &other)
{
    Q_D(Event);
    d->core.link(other.d_func()->core);
}

void Event::unlink(Event &other)
{
    Q_D(Event);
    d->core.unlink(other.d_func()->core);
}

class ThreadEventPrivate
{
public:
    qtng_core::ThreadEvent core;
};

ThreadEvent::ThreadEvent()
    : d(new ThreadEventPrivate)
{
}

ThreadEvent::~ThreadEvent()
{
    delete d;
}

bool ThreadEvent::tryWait(quint32 msecs)
{
    return d->core.tryWait(msecs);
}

void ThreadEvent::set()
{
    d->core.set();
}

void ThreadEvent::clear()
{
    d->core.clear();
}

bool ThreadEvent::isSet() const
{
    return d->core.isSet();
}

quint32 ThreadEvent::getting() const
{
    return d->core.getting();
}

void ThreadEvent::link(ThreadEvent &other)
{
    d->core.link(other.d->core);
}

void ThreadEvent::unlink(ThreadEvent &other)
{
    d->core.unlink(other.d->core);
}

bool Gate::tryWait(quint32 msecs)
{
    if (!lock.isLocked()) {
        return true;
    }
    bool success = lock.tryAcquire(msecs);
    if (!success) {
        return false;
    }
    lock.release();
    return true;
}

}  // namespace QTNETWORKNG_NAMESPACE
