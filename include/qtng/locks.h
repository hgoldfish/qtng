#ifndef QTNG_LOCKS_H
#define QTNG_LOCKS_H

#include <algorithm>
#include <climits>
#include <cstdint>
#include <deque>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "qtng/utils/shared_mutex_compat.h"
#include "qtng/coroutine.h"
#include "qtng/utils/platform.h"

namespace qtng {

class SemaphorePrivate;
class Semaphore
{
public:
    explicit Semaphore(int value = 1);
    virtual ~Semaphore();
public:
    NG_DEPRECATED inline bool acquire(bool blocking = true) { return tryAcquire(blocking ? UINT_MAX : 0); }
    bool acquireMany(int value, std::uint32_t msecs = UINT_MAX);
    bool tryAcquire(std::uint32_t msecs = UINT_MAX);
    void release(int value = 1);
    bool isLocked() const;
    bool isUsed() const;
    std::uint32_t getting() const;
private:
    std::shared_ptr<SemaphorePrivate> d;
    NG_DISABLE_COPY(Semaphore)
};

class Lock : public Semaphore
{
public:
    Lock();
private:
    NG_DISABLE_COPY(Lock)
};

class RLockPrivate;
class RLock
{
public:
    RLock();
    virtual ~RLock();
public:
    NG_DEPRECATED inline bool acquire(bool blocking = true) { return tryAcquire(blocking ? UINT_MAX : 0); }
    bool tryAcquire(std::uint32_t msecs = UINT_MAX);
    void release();
    bool isLocked() const;
    bool isOwned() const;
private:
    RLockPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(RLock)
    NG_DISABLE_COPY(RLock)
    friend class ConditionPrivate;
    friend class RLockPrivate;
};

class ConditionPrivate;
class Condition
{
public:
    Condition();
    virtual ~Condition();
public:
    bool wait(std::uint32_t msecs = UINT_MAX);
    void notify(int value = 1);
    void notifyAll();
    std::uint32_t getting() const;
private:
    ConditionPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(Condition)
    NG_DISABLE_COPY(Condition)
};

class EventPrivate;
class Event
{
public:
    Event();
    virtual ~Event();
public:
    NG_DEPRECATED inline bool wait(bool blocking = true) { return tryWait(blocking ? UINT_MAX : 0); }
    bool tryWait(std::uint32_t msecs = UINT_MAX);
    void set();
    void clear();
    bool isSet() const;
    std::uint32_t getting() const;
public:
    void link(Event &other);
    void unlink(Event &other);
private:
    EventPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(Event)
    NG_DISABLE_COPY(Event)
    friend class EventPrivate;
};

class ThreadEventPrivate;
class ThreadEvent
{
public:
    ThreadEvent();
    virtual ~ThreadEvent();
public:
    NG_DEPRECATED inline bool wait(bool blocking = true) { return tryWait(blocking ? UINT_MAX : 0); }
    bool tryWait(std::uint32_t msecs = UINT_MAX);
    void set();
    void clear();
    bool isSet() const;
    std::uint32_t getting() const;
public:
    void link(ThreadEvent &other);
    void unlink(ThreadEvent &other);
private:
    ThreadEventPrivate *d;
    NG_DISABLE_COPY(ThreadEvent)
};

template<typename EventType>
bool waitAnyEvent(const std::vector<std::shared_ptr<EventType>> &events)
{
    EventType event;
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i]->isSet()) {
            return true;
        }
        events[i]->link(event);
    }
    return event.tryWait();
}

template<typename EventType>
bool waitAllEvents(const std::vector<std::shared_ptr<EventType>> &events)
{
    for (size_t i = 0; i < events.size(); ++i) {
        if (!events[i]->tryWait()) {
            return false;
        }
    }
    return true;
}

template<typename Value>
class ValueEvent
{
public:
    ValueEvent() { }
    ~ValueEvent() { }
    void send(const Value &value);
    NG_DEPRECATED Value wait(bool blocking = true);
    Value tryWait(std::uint32_t msces = UINT_MAX);
    void set() { event.set(); }
    void clear() { event.clear(); }
    bool isSet() const { return event.isSet(); }
    std::uint32_t getting() const { return event.getting(); }
public:
    Event event;
    Value value;
private:
    NG_DISABLE_COPY(ValueEvent)
};

template<typename Value>
void ValueEvent<Value>::send(const Value &value)
{
    this->value = value;
    event.set();
}

template<typename Value>
NG_DEPRECATED Value ValueEvent<Value>::wait(bool blocking /*= true*/)
{
    return tryWait(blocking ? UINT_MAX : 0);
}

template<typename Value>
Value ValueEvent<Value>::tryWait(std::uint32_t msces /*= UINT_MAX*/)
{
    if (!event.tryWait(msces)) {
        return Value();
    } else {
        return value;
    }
}

class Gate
{
public:
    inline Gate() { }
public:
    NG_DEPRECATED inline bool goThrough(bool blocking = true) { return tryWait(blocking ? UINT_MAX : 0); }
    NG_DEPRECATED inline bool wait(bool blocking = true) { return tryWait(blocking ? UINT_MAX : 0); }
    bool tryWait(std::uint32_t msecs = UINT_MAX);
    inline void open()
    {
        if (lock.isLocked())
            lock.release();
    }
    inline void close()
    {
        if (!lock.isLocked()) {
            lock.tryAcquire();
        }
    }
    inline bool isOpen() const { return !lock.isLocked(); }
    inline bool isClosed() const { return lock.isLocked(); }
private:
    Lock lock;
    NG_DISABLE_COPY(Gate)
};

template<typename LockType>
class ScopedLock
{
public:
    ScopedLock(LockType &lock)
        : lock(lock)
        , success(false)
    {
        success = lock.tryAcquire();
    }
    ~ScopedLock()
    {
        if (success) {
            lock.release();
        }
    }
    inline void release()
    {
        if (success) {
            lock.release();
            success = false;
        }
    }
    inline bool isSuccess() const { return success; }
private:
    LockType &lock;
    bool success;
};

struct DummyReadWriteLock
{
    inline void lockForRead() { }
    inline void lockForWrite() { }
    inline void unlock() { }
};

class SharedReadWriteLock
{
public:
    void lockForRead() { m.lock_shared(); mode = Read; }
    void lockForWrite() { m.lock(); mode = Write; }
    void unlock()
    {
        if (mode == Read) {
            m.unlock_shared();
        } else if (mode == Write) {
            m.unlock();
        }
        mode = None;
    }

private:
    utils::SharedMutex m;
    enum { None, Read, Write } mode = None;
};

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
class SizedQueueType;

template<typename T>
struct UnitElementSizeGetter
{
    static inline std::uint32_t sizeOf(const T &) { return 1; }
};

template<typename T, typename EventType, typename ReadWriteLockType>
using QueueType = SizedQueueType<T, EventType, ReadWriteLockType, UnitElementSizeGetter<T>>;

template<typename T>
struct DefaultElementSizeGetter
{
    static inline std::uint32_t sizeOf(const T &e) { return static_cast<std::uint32_t>(e.size()); }
};

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter = DefaultElementSizeGetter<T>>
class SizedQueueType
{
public:
    explicit SizedQueueType(std::uint32_t capacity);
    SizedQueueType()
        : SizedQueueType(UINT_MAX)
    {
    }
    ~SizedQueueType();
    void setCapacity(std::uint32_t capacity);
    bool put(const T &e);
    bool put(T &&e);
    bool putForcedly(const T &e);
    bool putForcedly(T &&e);
    bool returns(const T &e);
    bool returns(T &&e);
    bool returnsForcely(const T &e);
    bool returnsForcely(T &&e);

    template<typename U = EventType>
    typename std::enable_if<std::is_same<U, ThreadEvent>::value, T>::type get()
    {
        do {
            if (!notEmpty.tryWait()) {
                return T();
            }
            lock.lockForWrite();
            if (!queue.empty()) {
                break;
            }
            lock.unlock();
        } while (true);

        T e = std::move(queue.front());
        queue.pop_front();
        currentSize -= SizeGetter::sizeOf(e);
        if (queue.empty()) {
            notEmpty.clear();
        }
        if (currentSize < mCapacity) {
            notFull.set();
        }
        lock.unlock();
        return e;
    }

    template<typename U = EventType>
    typename std::enable_if<!std::is_same<U, ThreadEvent>::value, T>::type get()
    {
        if (!notEmpty.tryWait()) {
            return T();
        }
        lock.lockForWrite();

        T e = std::move(queue.front());
        queue.pop_front();
        currentSize -= SizeGetter::sizeOf(e);
        if (queue.empty()) {
            notEmpty.clear();
        }
        if (currentSize < mCapacity) {
            notFull.set();
        }
        lock.unlock();
        return e;
    }

    template<typename U = EventType>
    typename std::enable_if<std::is_same<U, ThreadEvent>::value, T>::type peek();

    template<typename U = EventType>
    typename std::enable_if<!std::is_same<U, ThreadEvent>::value, T>::type peek();

    void clear();
    bool remove(const T &e);
public:
    inline bool isEmpty();
    inline bool isFull();
    inline std::uint32_t capacity() const;
    inline std::uint32_t size() const;
    inline std::uint32_t getting() const;
    inline bool contains(const T &e);
public:
    std::deque<T> queue;
    EventType notEmpty;
    EventType notFull;
    ReadWriteLockType lock;
    std::uint32_t mCapacity;
    std::uint32_t currentSize;
    NG_DISABLE_COPY(SizedQueueType)
};

template<typename T, typename EventType, typename ReadWriteLockType>
class MultiQueueType
{
public:
    inline void addQueue(std::shared_ptr<QueueType<T, EventType, ReadWriteLockType>> queue);
    inline void removeQueue(std::shared_ptr<QueueType<T, EventType, ReadWriteLockType>> queue);
    inline T tryWait();
private:
    EventType notEmpty;
    std::vector<std::shared_ptr<QueueType<T, EventType, ReadWriteLockType>>> queues;
    ReadWriteLockType lock;
};

template<typename T>
class SizedQueue : public SizedQueueType<T, Event, DummyReadWriteLock>
{
public:
    explicit SizedQueue(std::uint32_t capacity)
        : SizedQueueType<T, Event, DummyReadWriteLock>(capacity)
    {
    }
    explicit SizedQueue()
        : SizedQueueType<T, Event, DummyReadWriteLock>()
    {
    }
};

template<typename T>
class Queue : public QueueType<T, Event, DummyReadWriteLock>
{
public:
    explicit Queue(std::uint32_t capacity)
        : QueueType<T, Event, DummyReadWriteLock>(capacity)
    {
    }
    explicit Queue()
        : QueueType<T, Event, DummyReadWriteLock>()
    {
    }
};

template<typename T>
class MultiQueue : public MultiQueueType<T, Event, DummyReadWriteLock>
{
};

template<typename T>
class ThreadQueue : public QueueType<T, ThreadEvent, SharedReadWriteLock>
{
public:
    explicit ThreadQueue(std::uint32_t capacity)
        : QueueType<T, ThreadEvent, SharedReadWriteLock>(capacity)
    {
    }
    explicit ThreadQueue()
        : QueueType<T, ThreadEvent, SharedReadWriteLock>()
    {
    }
};

template<typename T>
class MultiThreadQueue : public MultiQueueType<T, ThreadEvent, SharedReadWriteLock>
{
};

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::SizedQueueType(std::uint32_t capacity)
    : mCapacity(capacity)
    , currentSize(0)
{
    notEmpty.clear();
    notFull.set();
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::~SizedQueueType()
{
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
void SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::setCapacity(std::uint32_t capacity)
{
    lock.lockForWrite();
    mCapacity = capacity;
    if (currentSize >= mCapacity) {
        notFull.clear();
    } else {
        notFull.set();
    }
    lock.unlock();
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
void SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::clear()
{
    lock.lockForWrite();
    queue.clear();
    currentSize = 0;
    notFull.set();
    notEmpty.clear();
    lock.unlock();
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::remove(const T &e)
{
    lock.lockForWrite();
    int n = 0;
    std::uint32_t removedSize = 0;
    for (int i = static_cast<int>(queue.size()) - 1; i >= 0; --i) {
        if (queue[static_cast<size_t>(i)] == e) {
            removedSize += SizeGetter::sizeOf(queue[static_cast<size_t>(i)]);
            queue.erase(queue.begin() + i);
            ++n;
        }
    }
    if (n > 0) {
        currentSize -= removedSize;
        if (queue.empty()) {
            notEmpty.clear();
        } else {
            notEmpty.set();
        }
        if (currentSize >= mCapacity) {
            notFull.clear();
        } else {
            notFull.set();
        }
        lock.unlock();
        return true;
    }
    lock.unlock();
    return false;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::put(const T &e)
{
    if (!notFull.tryWait()) {
        return false;
    }
    lock.lockForWrite();
    queue.push_back(e);
    currentSize += SizeGetter::sizeOf(e);
    notEmpty.set();
    if (currentSize >= mCapacity) {
        notFull.clear();
    }
    lock.unlock();
    return true;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::put(T &&e)
{
    if (!notFull.tryWait()) {
        return false;
    }
    lock.lockForWrite();
    const std::uint32_t elementSize = SizeGetter::sizeOf(e);
    queue.push_back(std::move(e));
    currentSize += elementSize;
    notEmpty.set();
    if (currentSize >= mCapacity) {
        notFull.clear();
    }
    lock.unlock();
    return true;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::putForcedly(const T &e)
{
    lock.lockForWrite();
    queue.push_back(e);
    currentSize += SizeGetter::sizeOf(e);
    notEmpty.set();
    if (currentSize >= mCapacity) {
        notFull.clear();
    }
    lock.unlock();
    return true;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::putForcedly(T &&e)
{
    lock.lockForWrite();
    const std::uint32_t elementSize = SizeGetter::sizeOf(e);
    queue.push_back(std::move(e));
    currentSize += elementSize;
    notEmpty.set();
    if (currentSize >= mCapacity) {
        notFull.clear();
    }
    lock.unlock();
    return true;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::returns(const T &e)
{
    if (!notFull.tryWait()) {
        return false;
    }
    lock.lockForWrite();
    queue.push_front(e);
    currentSize += SizeGetter::sizeOf(e);
    notEmpty.set();
    if (currentSize >= mCapacity) {
        notFull.clear();
    }
    lock.unlock();
    return true;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::returns(T &&e)
{
    if (!notFull.tryWait()) {
        return false;
    }
    lock.lockForWrite();
    const std::uint32_t elementSize = SizeGetter::sizeOf(e);
    queue.push_front(std::move(e));
    currentSize += elementSize;
    notEmpty.set();
    if (currentSize >= mCapacity) {
        notFull.clear();
    }
    lock.unlock();
    return true;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::returnsForcely(const T &e)
{
    lock.lockForWrite();
    queue.push_front(e);
    currentSize += SizeGetter::sizeOf(e);
    notEmpty.set();
    if (currentSize >= mCapacity) {
        notFull.clear();
    }
    lock.unlock();
    return true;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::returnsForcely(T &&e)
{
    lock.lockForWrite();
    const std::uint32_t elementSize = SizeGetter::sizeOf(e);
    queue.push_front(std::move(e));
    currentSize += elementSize;
    notEmpty.set();
    if (currentSize >= mCapacity) {
        notFull.clear();
    }
    lock.unlock();
    return true;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
template<typename U>
typename std::enable_if<std::is_same<U, ThreadEvent>::value, T>::type
SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::peek()
{
    lock.lockForRead();
    if (queue.empty()) {
        lock.unlock();
        return T();
    }
    T t = queue.front();
    lock.unlock();
    return t;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
template<typename U>
typename std::enable_if<!std::is_same<U, ThreadEvent>::value, T>::type
SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::peek()
{
    lock.lockForRead();
    if (queue.empty()) {
        lock.unlock();
        return T();
    }
    T t = queue.front();
    lock.unlock();
    return t;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
inline bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::isEmpty()
{
    lock.lockForRead();
    bool t = queue.empty();
    lock.unlock();
    return t;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
inline bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::isFull()
{
    lock.lockForRead();
    bool t = currentSize >= mCapacity;
    lock.unlock();
    return t;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
inline std::uint32_t SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::capacity() const
{
    const_cast<SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter> *>(this)->lock.lockForRead();
    std::uint32_t c = mCapacity;
    const_cast<SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter> *>(this)->lock.unlock();
    return c;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
inline std::uint32_t SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::size() const
{
    const_cast<SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter> *>(this)->lock.lockForRead();
    std::uint32_t s = currentSize;
    const_cast<SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter> *>(this)->lock.unlock();
    return s;
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
inline std::uint32_t SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::getting() const
{
    const_cast<SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter> *>(this)->lock.lockForRead();
    int g = notEmpty.getting();
    const_cast<SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter> *>(this)->lock.unlock();
    return static_cast<std::uint32_t>(g);
}

template<typename T, typename EventType, typename ReadWriteLockType, typename SizeGetter>
inline bool SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter>::contains(const T &e)
{
    const_cast<SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter> *>(this)->lock.lockForRead();
    bool t = std::find(queue.begin(), queue.end(), e) != queue.end();
    const_cast<SizedQueueType<T, EventType, ReadWriteLockType, SizeGetter> *>(this)->lock.unlock();
    return t;
}

template<typename T, typename EventType, typename ReadWriteLockType>
inline void MultiQueueType<T, EventType, ReadWriteLockType>::addQueue(
        std::shared_ptr<QueueType<T, EventType, ReadWriteLockType>> queue)
{
    lock.lockForWrite();
    queue->notEmpty.link(notEmpty);
    queues.push_back(queue);
    lock.unlock();
}

template<typename T, typename EventType, typename ReadWriteLockType>
inline void MultiQueueType<T, EventType, ReadWriteLockType>::removeQueue(
        std::shared_ptr<QueueType<T, EventType, ReadWriteLockType>> queue)
{
    lock.lockForWrite();
    queue->notEmpty.unlink(notEmpty);
    queues.erase(std::remove(queues.begin(), queues.end(), queue), queues.end());
    lock.unlock();
}

template<typename T, typename EventType, typename ReadWriteLockType>
inline T MultiQueueType<T, EventType, ReadWriteLockType>::tryWait()
{
    notEmpty.tryWait();
    T result;
    lock.lockForWrite();
    bool allEmpty = true;
    size_t i = 0;
    for (; i < queues.size(); ++i) {
        std::shared_ptr<QueueType<T, EventType, ReadWriteLockType>> queue = queues[i];
        if (!queue->empty()) {
            result = queue->data();
            allEmpty = queue->empty();
            break;
        }
    }
    if (allEmpty) {
        ++i;
        for (; i < queues.size(); ++i) {
            std::shared_ptr<QueueType<T, EventType, ReadWriteLockType>> queue = queues[i];
            if (!queue->empty()) {
                allEmpty = false;
                break;
            }
        }
    }
    lock.unlock();
    return result;
}

}  // namespace qtng

#endif  // QTNG_LOCKS_H
