#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "qtng/private/eventloop_p.h"
#include "qtng/locks.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.locks");

namespace qtng {

class SemaphorePrivate
{
public:
    SemaphorePrivate(int value);
    virtual ~SemaphorePrivate();
public:
    bool acquire(shared_ptr<SemaphorePrivate> self, int value, uint32_t msecs);
    void release(shared_ptr<SemaphorePrivate> self, int value);
    void scheduleDelete(shared_ptr<SemaphorePrivate> self);
public:
    vector<BaseCoroutine *> waiters;
    const int init_value;
    volatile int counter;
    int notified;
};

SemaphorePrivate::SemaphorePrivate(int value)
    : init_value(max(1, value))
    , counter(value)
    , notified(0)
{
    if (value < 1) {
        ngWarning() << "Semaphore got init value less than 1:" << value << ", we treat it as 1.";
    }
}

SemaphorePrivate::~SemaphorePrivate()
{
    assert(waiters.empty());
}

bool SemaphorePrivate::acquire(shared_ptr<SemaphorePrivate> self, int value, uint32_t msecs)
{
    if (counter >= value) {
        counter -= value;
        return true;
    }
    if (msecs == 0) {
        return false;
    }
    // UINT_MAX: means wait until success
    int callbackId = 0;
    if (msecs != (UINT_MAX)) {
        callbackId = EventLoopCoroutine::get()->callLater(msecs, new YieldCurrentFunctor());
    }

    assert(EventLoopCoroutine::get() != BaseCoroutine::current());
    assert(value <= init_value);

    int gotNum = counter;
    int remain = value - counter;
    counter = 0;

    while (remain > 0) {
        waiters.push_back(BaseCoroutine::current());

        try {
            EventLoopCoroutine::get()->yield();
        } catch (...) {
            // if we caught an exception, the release() must not touch me.
            // the waiter should be remove.
            bool found = waiters.erase(remove(waiters.begin(), waiters.end(), BaseCoroutine::current()), waiters.end()) != waiters.end();
            assert(found);
            if (callbackId) {
                EventLoopCoroutine::get()->cancelCall(callbackId);
            }
            release(self, gotNum);
            throw;
        }

        bool found = waiters.erase(remove(waiters.begin(), waiters.end(), BaseCoroutine::current()), waiters.end()) != waiters.end();
        if (found) {  // timeout
            release(self, gotNum);  // release what has been acquired
            return false;
        }

        assert(notified != 0);
        assert(counter > 0);
        if (counter >= remain) {
            counter -= remain;
            break;
        } else {
            gotNum += counter;
            remain -= counter;
            counter = 0;
        }
    }
    if (callbackId) {
        EventLoopCoroutine::get()->cancelCall(callbackId);
    }
    return true;
}

class SemaphoreNotifyWaitersFunctor : public Functor
{
public:
    SemaphoreNotifyWaitersFunctor(shared_ptr<SemaphorePrivate> sp, bool doDelete)
        : sp(sp)
        , doDelete(doDelete)
    {
    }
    shared_ptr<SemaphorePrivate> sp;
    bool doDelete;
    virtual bool operator()() override
    {
        while ((doDelete || (sp->notified != 0 && sp->counter > 0)) && !sp->waiters.empty()) {
            BaseCoroutine *waiter = sp->waiters.front(); sp->waiters.erase(sp->waiters.begin());
            if (waiter== nullptr) {
                ngDebug() << "waiter was deleted.";
                continue;
            }
            waiter->yield();
        }
        // do not move this line above the loop, see the Q_ASSERT_X(notified != 0) in SemaphorePrivate::acquire()
        sp->notified = 0;
        return true;
    }
};

void SemaphorePrivate::release(shared_ptr<SemaphorePrivate> self, int value)
{
    if (value <= 0) {
        return;
    }
    if (counter > INT_MAX - value) {
        counter = INT_MAX;
    } else {
        counter += value;
    }
    counter = min(static_cast<int>(counter), init_value);
    if (!notified && !waiters.empty()) {
        notified = EventLoopCoroutine::get()->callLater(0, new SemaphoreNotifyWaitersFunctor(self, false));
    }
}

void SemaphorePrivate::scheduleDelete(shared_ptr<SemaphorePrivate> self)
{
    if (notified) {
        EventLoopCoroutine::get()->cancelCall(notified);
        notified = 0;
    }
    counter = init_value;
    EventLoopCoroutine::get()->callLater(0, new SemaphoreNotifyWaitersFunctor(self, true));
}

Semaphore::Semaphore(int value)
    : d(new SemaphorePrivate(value))
{
}

Semaphore::~Semaphore()
{
    d->scheduleDelete(d);
    d.reset();
}

bool Semaphore::acquireMany(int value, uint32_t msecs)
{
    if (!d) {
        return false;
    }
    shared_ptr<SemaphorePrivate> d(this->d);
    if (value > d->init_value) {
        return false;
    }
    return d->acquire(d, value, msecs);
}

bool Semaphore::tryAcquire(uint32_t msecs /*= (UINT_MAX)*/)
{
    if (!d) {
        return false;
    }
    shared_ptr<SemaphorePrivate> d(this->d);
    if (1 > d->init_value) {
        return false;
    }
    return d->acquire(d, 1, msecs);
}

void Semaphore::release(int value)
{
    if (!d) {
        return;
    }
    d->release(d, value);
}

bool Semaphore::isLocked() const
{
    if (!d) {
        return false;
    }
    return d->counter <= 0;
}

bool Semaphore::isUsed() const
{
    if (!d) {
        return false;
    }
    return d->counter < d->init_value;
}

uint32_t Semaphore::getting() const
{
    if (!d) {
        return 0;
    }
    return d->waiters.size();
}

Lock::Lock()
    : Semaphore(1)
{
}

struct RLockState
{
    uintptr_t holder;
    int counter;
};

class RLockPrivate
{
public:
    RLockPrivate(RLock *q);
    ~RLockPrivate();
public:
    bool acquire(uint32_t msecs);
    void release();
    RLockState reset();
    void set(const RLockState &state);
private:
    RLock * const q_ptr;
public:
    Lock lock;
    uintptr_t holder;
    int counter;
    NG_DECLARE_PUBLIC(RLock)
};

RLockPrivate::RLockPrivate(RLock *q)
    : q_ptr(q)
    , holder(0)
    , counter(0)
{
}

RLockPrivate::~RLockPrivate() { }

bool RLockPrivate::acquire(uint32_t msecs)
{
    if (holder == BaseCoroutine::current()->id()) {
        counter += 1;
        return true;
    }
    if (lock.tryAcquire(msecs)) {
        counter = 1;
        holder = BaseCoroutine::current()->id();
        return true;
    }
    return false;  // XXX lock is deleted.
}

void RLockPrivate::release()
{
    if (holder != BaseCoroutine::current()->id()) {
        ngWarning() << "do not release other coroutine's rlock.";
        return;
    }
    counter -= 1;
    if (counter == 0) {
        holder = 0;
        lock.release();
    }
}

RLockState RLockPrivate::reset()
{
    RLockState state;
    state.counter = counter;
    counter = 0;
    state.holder = holder;
    holder = 0;
    if (state.counter > 0) {
        lock.release();
    }
    return state;
}

void RLockPrivate::set(const RLockState &state)
{
    counter = state.counter;
    holder = state.holder;
    if (counter > 0) {
        lock.tryAcquire();
    }
}

RLock::RLock()
    : d_ptr(new RLockPrivate(this))
{
}

RLock::~RLock()
{
    delete d_ptr;
}

bool RLock::tryAcquire(uint32_t msecs)
{
    NG_D(RLock);
    return d->acquire(msecs);
}

void RLock::release()
{
    NG_D(RLock);
    d->release();
}

bool RLock::isLocked() const
{
    NG_D(const RLock);
    return d->lock.isLocked();
}

bool RLock::isOwned() const
{
    NG_D(const RLock);
    return d->holder == BaseCoroutine::current()->id();
}

class ConditionPrivate
{
public:
    vector<shared_ptr<Lock>> waiters;
};

Condition::Condition()
    : d_ptr(new ConditionPrivate())
{
}

Condition::~Condition()
{
    notify(d_ptr->waiters.size());
    delete d_ptr;
}

bool Condition::wait(uint32_t msecs)
{
    NG_D(Condition);
    shared_ptr<Lock> waiter(new Lock());
    if (!waiter->tryAcquire(0)) {
        return false;
    }
    d->waiters.push_back(waiter);

    bool ok = false;
    try {
        ok = waiter->tryAcquire(msecs);
    } catch (...) {
        waiter->release();
        d->waiters.erase(remove(d->waiters.begin(), d->waiters.end(), waiter), d->waiters.end());
        throw;
    }

    if (ok) {
        waiter->release();
    }
    d->waiters.erase(remove(d->waiters.begin(), d->waiters.end(), waiter), d->waiters.end());
    return ok;
}

void Condition::notify(int value)
{
    NG_D(Condition);
    for (int i = 0; i < value && !d->waiters.empty(); ++i) {
        shared_ptr<Lock> waiter = d->waiters.front();
        d->waiters.erase(d->waiters.begin());
        waiter->release();
    }
}

void Condition::notifyAll()
{
    NG_D(Condition);
    notify(d->waiters.size());
}

uint32_t Condition::getting() const
{
    NG_D(const Condition);
    return static_cast<uint32_t>(d->waiters.size());
}

class EventPrivate
{
public:
    EventPrivate(Event *q);
    ~EventPrivate();
public:
    void set();
    void clear();
    bool wait(uint32_t msecs);
private:
    Event * const q_ptr;
    Condition condition;
    volatile bool flag;
    vector<Event *> linkTo;
    vector<Event *> linkFrom;
    friend class Event;
    NG_DECLARE_PUBLIC(Event)
};

EventPrivate::EventPrivate(Event *q)
    : q_ptr(q)
    , flag(false)
{
}

EventPrivate::~EventPrivate()
{
    if (!flag && condition.getting() > 0) {
        condition.notifyAll();
    }
    for (Event *event : linkFrom) {
        event->d_ptr->linkTo.erase(remove(event->d_ptr->linkTo.begin(), event->d_ptr->linkTo.end(), q_ptr), event->d_ptr->linkTo.end());
    }
    for (Event *event : linkTo) {
        event->d_ptr->linkFrom.erase(remove(event->d_ptr->linkFrom.begin(), event->d_ptr->linkFrom.end(), q_ptr), event->d_ptr->linkFrom.end());
    }
}

void EventPrivate::set()
{
    if (!flag) {
        flag = true;
        condition.notifyAll();
        for (Event *other : linkTo) {
            other->set();
        }
    }
}

void EventPrivate::clear()
{
    flag = false;
}

bool EventPrivate::wait(uint32_t msecs)
{
    if (msecs == 0 || flag) {
        return flag;
    }

    if (msecs == UINT_MAX) {
        do {
            try {
                if (!condition.wait(UINT_MAX)) {
                    return false;
                }
            } catch (...) {
                throw;
            }
        } while (!flag);
    } else {
        chrono::steady_clock::time_point timer;
        timer = chrono::steady_clock::now();

        uint32_t elapsed = 0;
        while (true) {
            try {
                if (!condition.wait(msecs - elapsed)) {
                    return false;
                }
            } catch (...) {
                throw;
            }
            if (flag) {
                break;
            }
            elapsed = static_cast<uint32_t>(chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - timer).count());
            if (elapsed >= msecs) {
                return false;
            }
        }
    }
    return flag;
}

Event::Event()
    : d_ptr(new EventPrivate(this))
{
}

Event::~Event()
{
    delete d_ptr;
}

bool Event::tryWait(uint32_t msecs)
{
    NG_D(Event);
    return d->wait(msecs);
}

void Event::set()
{
    NG_D(Event);
    d->set();
}

bool Event::isSet() const
{
    NG_D(const Event);
    return d->flag;
}

void Event::clear()
{
    NG_D(Event);
    d->clear();
}

uint32_t Event::getting() const
{
    NG_D(const Event);
    return d->condition.getting();
}

void Event::link(Event &other)
{
    NG_D(Event);
    d->linkTo.push_back(&other);
    other.d_func()->linkFrom.push_back(this);
}

void Event::unlink(Event &other)
{
    NG_D(Event);
    d->linkTo.erase(remove(d->linkTo.begin(), d->linkTo.end(), &other), d->linkTo.end());
    other.d_ptr->linkFrom.erase(remove(other.d_ptr->linkFrom.begin(), other.d_ptr->linkFrom.end(), this), other.d_ptr->linkFrom.end());
}

struct Behold
{
    shared_ptr<EventLoopCoroutine> eventloop;
    shared_ptr<Condition> condition;
};

class ThreadEventPrivate
{
public:
    ThreadEventPrivate();
    void notify();
    bool wait(uint32_t msecs);
    uint32_t getting();
    inline void incref();
    inline bool decref();
public:
    std::condition_variable condition;
    std::mutex mutex;
    vector<Behold> holds;
    vector<ThreadEvent *> linkTo;
    vector<ThreadEvent *> linkFrom;
    atomic<int> flag;
    atomic<int> count;
    atomic<uint32_t> ref;
};

class NotifiyCondition : public Functor
{
public:
    NotifiyCondition(shared_ptr<Condition> condition)
        : condition(condition)
    {
    }
    virtual bool operator()()
    {
        condition->notifyAll();
        return true;
    }
    shared_ptr<Condition> condition;
};

ThreadEventPrivate::ThreadEventPrivate()
    : flag(false)
    , count(0)
    , ref(1)
{
}

void ThreadEventPrivate::notify()
{
    incref();
    mutex.lock();
    shared_ptr<EventLoopCoroutine> current = currentLoop()->get();
    for (auto it = holds.begin(); it != holds.end() && ref.load() > 1; ) {
        const Behold &hold = *it;
        shared_ptr<Condition> holdCondition = hold.condition;
        EventLoopCoroutine *holdEventloop = hold.eventloop.get();
        if (holdEventloop) {
            if (holdEventloop == current.get()) {
                holdCondition->notifyAll();
            } else {
                holdEventloop->callLaterThreadSafe(0, new NotifiyCondition(holdCondition));
            }
            ++it;
        } else {
            it = holds.erase(it);
        }
    }
    mutex.unlock();
    // XXX the flag can be false.
    if (count.load() > 0) {
        condition.notify_all();
    }
    decref();
}

bool ThreadEventPrivate::wait(uint32_t msecs)
{
    bool f = flag.load();
    if (msecs == 0 || f) {
        return f;
    }

    shared_ptr<chrono::steady_clock::time_point> timer;
    if (msecs != UINT_MAX) {
        timer.reset(new chrono::steady_clock::time_point());
        (*timer) = chrono::steady_clock::now();
    }

    incref();
    mutex.lock();
    EventLoopCoroutine *current = currentLoop()->get().get();
    assert(!f);
    if (!current) {
        if (msecs != UINT_MAX) {
            ngWarning() << "useless arg:msecs when call ThreadEvent::wait";
        }

        ++count;
        while (!(f = flag.load()) && ref.load() > 1) {
            { std::unique_lock<std::mutex> lk(mutex, std::adopt_lock); condition.wait(lk); lk.release(); }
        }
        --count;
        mutex.unlock();
    } else {
        shared_ptr<Condition> condition;
        // should we use map<EventLoopCoroutine *, Hold> to accelerate?
        for (const Behold &hold : holds) {
            if (hold.eventloop.get() == current) {
                condition = hold.condition;
                break;
            }
        }
        if (!condition) {
            condition.reset(new Condition());
            Behold hold;
            hold.condition = condition;
            hold.eventloop = currentLoop()->get();
            holds.push_back(hold);
        }
        mutex.unlock();
        bool ok = false;

        while (!(f = flag.load()) && ref.load() > 1) {
            try {
                if (msecs == UINT_MAX) {
                    ok = condition->wait();
                } else {
                    uint32_t elapsed = static_cast<uint32_t>(chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - *timer).count());
                    if (msecs <= elapsed) {
                        return false;
                    }
                    ok = condition->wait(msecs - elapsed);
                }
            } catch (...) {
                decref();
                throw;
            }
            if (!ok) {
                decref();
                return false;
            }
        }
    }
    decref();
    return f;
}

uint32_t ThreadEventPrivate::getting()
{
    incref();
    mutex.lock();
    uint32_t count = this->count.load();
    for (const Behold &hold : holds) {
        if (hold.condition) {
            count += hold.condition->getting();
        }
    }
    mutex.unlock();
    decref();
    return count;
}

void ThreadEventPrivate::incref()
{
    ++ref;
}

bool ThreadEventPrivate::decref()
{
    if (ref.fetch_sub(1) == 1) {
        delete this;
        return false;
    }
    return true;
}

ThreadEvent::ThreadEvent()
    : d(new ThreadEventPrivate())
{
}

ThreadEvent::~ThreadEvent()
{
    if (d->decref()) {
        d->notify();
    }
    d = nullptr;
}

bool ThreadEvent::tryWait(uint32_t msecs)
{
    if (d) {
        return d->wait(msecs);
    } else {
        return false;
    }
}

void ThreadEvent::set()
{
    if (!d) {
        return;
    }

    if (d->flag.exchange(true)) {
        return;
    }
    d->notify();
}

void ThreadEvent::clear()
{
    if (!d) {
        return;
    }
    d->flag.store(false);
    // d->flag.testAndSetAcquire(true, false);
}

bool ThreadEvent::isSet() const
{
    if (!d) {
        return false;
    }
    return d->flag.load();
}

uint32_t ThreadEvent::getting() const
{
    if (!d) {
        return 0;
    }
    return d->getting();
}

void ThreadEvent::link(ThreadEvent &other)
{
    if (!d) {
        return;
    }
    d->mutex.lock();
    d->linkTo.push_back(&other);
    d->mutex.unlock();
    other.d->mutex.lock();
    other.d->linkFrom.push_back(this);
    other.d->mutex.unlock();
}

void ThreadEvent::unlink(ThreadEvent &other)
{
    if (!d) {
        return;
    }
    d->mutex.lock();
    d->linkTo.erase(remove(d->linkTo.begin(), d->linkTo.end(), &other), d->linkTo.end());
    d->mutex.unlock();
    other.d->mutex.lock();
    other.d->linkFrom.erase(remove(other.d->linkFrom.begin(), other.d->linkFrom.end(), this), other.d->linkFrom.end());
    other.d->mutex.unlock();
}

class GatePrivate
{
public:
    Lock lock;
};

bool Gate::tryWait(uint32_t msecs /*= (UINT_MAX)*/)
{
    if (!lock.isLocked()) {
        return true;
    } else {
        bool success = lock.tryAcquire(msecs);
        if (!success) {
            return false;
        } else {
            lock.release();
            return true;
        }
    }
}

}  // namespace qtng
