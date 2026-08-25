#ifndef QTNG_EVENTLOOP_H
#define QTNG_EVENTLOOP_H

#include <functional>
#include <QtCore/qthreadstorage.h>
#include <QtCore/qvariant.h>
#include <QtCore/qpointer.h>
#include "coroutine.h"

QTNETWORKNG_NAMESPACE_BEGIN

class CoroutinePrivate;
class Coroutine : public BaseCoroutine, public QEnableSharedFromThis<Coroutine>
{
    Q_DISABLE_COPY(Coroutine)
public:
    explicit Coroutine(size_t stackSize = DEFAULT_COROUTINE_STACK_SIZE);
    Coroutine(QObject *obj, const char *slot, size_t stackSize = DEFAULT_COROUTINE_STACK_SIZE);
    virtual ~Coroutine() override;
public:
    Coroutine *start(quint32 msecs = 0);
    void kill(CoroutineException *e = nullptr, quint32 msecs = 0);
    bool join();
    virtual void run() override;
public:
    static Coroutine *current();
    static void msleep(quint32 msecs);
    static void sleep(float secs) { msleep(static_cast<quint32>(secs * 1000)); }
    static Coroutine *spawn(std::function<void()> f);
protected:
    virtual void cleanup() override;
private:
    CoroutinePrivate * const d_ptr;
    Q_DECLARE_PRIVATE(Coroutine)
};

class TimeoutException : public CoroutineException
{
public:
    explicit TimeoutException();
    virtual QString what() const override;
    virtual void raise() override;
    virtual CoroutineException *clone() const override;
};

class Timeout : public QObject
{
public:
    Timeout(float secs);
    Timeout(quint32 msecs, int);  // the second parameter is not used.
    ~Timeout();
public:
    void cancel();
    void restart();
private:
    quint32 msecs;
    int timeoutId;
    Q_DISABLE_COPY(Timeout)
    Timeout(Timeout &&) = delete;
    Timeout &operator=(Timeout &&) = delete;
};

// useful for qt application.
int startQtLoop();

// Event loop backends. The built-in values are fixed: Ev=1, Qt=2. Third-party backends
// (io_uring, gtk, kqueue, ...) register under values >= 100 via qtng_core::registerEventLoop().
enum class EventLoopType {
    Ev = 1,
    Qt = 2,
};

// Select the event loop backend created on each thread. Call once at the very beginning of
// main(), before any coroutine/network API is used; replaces qtnetworkng 1.0's preferLibev().
// Without an explicit call the default backend is used (Qt on the GUI thread, libev/Win elsewhere).
void useEventloop(EventLoopType type);

// Select the Qt event loop backend explicitly (equivalent to useEventloop(EventLoopType::Qt)).
// Call at the very beginning of main(), before any coroutine/network API is used.
void useQtEventloop();

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_EVENTLOOP_H
