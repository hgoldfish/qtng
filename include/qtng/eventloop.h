#ifndef QTNG_EVENTLOOP_H
#define QTNG_EVENTLOOP_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "qtng/coroutine.h"
#include "qtng/utils/platform.h"

namespace qtng {

class CoroutinePrivate;
class Coroutine : public BaseCoroutine, public std::enable_shared_from_this<Coroutine>
{
    NG_DISABLE_COPY(Coroutine)
public:
    explicit Coroutine(size_t stackSize = DEFAULT_COROUTINE_STACK_SIZE);
    virtual ~Coroutine() override;
public:
    Coroutine *start(std::uint32_t msecs = 0);
    void kill(CoroutineException *e = nullptr, std::uint32_t msecs = 0);
    bool join();
    virtual void run() override;
public:
    static Coroutine *current();
    static void msleep(std::uint32_t msecs);
    static void sleep(float secs) { msleep(static_cast<std::uint32_t>(secs * 1000)); }
    static Coroutine *spawn(std::function<void()> f);
protected:
    virtual void cleanup() override;
private:
    CoroutinePrivate * const d_ptr;
    NG_DECLARE_PRIVATE_D(d_ptr, Coroutine)
};

class TimeoutException : public CoroutineException
{
public:
    explicit TimeoutException();
    virtual std::string what() const override;
    virtual void raise() override;
    virtual CoroutineException *clone() const override;
};

class Timeout
{
public:
    Timeout(float secs);
    Timeout(std::uint32_t msecs, int);  // the second parameter is not used.
    ~Timeout();
public:
    void cancel();
    void restart();
private:
    std::uint32_t msecs;
    int timeoutId;
};

class EventLoopCoroutine;

// Event loop backends. The built-in values are fixed: Ev=1, Qt=2. Third-party backends
// (io_uring, gtk, kqueue, ...) register under values >= 100 via registerEventLoop().
enum class EventLoopType {
    Ev = 1,
    Qt = 2,
    Gtk = 3,
    IOUring = 4,
    IOCP = 5,
    UserDefined = 100,
};

// A factory that creates the thread's event loop for a registered backend. Returning nullptr
// falls back to the default libev/Win backend.
using EventLoopFactory = std::function<std::shared_ptr<EventLoopCoroutine>()>;

// Select the event loop backend created on each thread. Call once at the very beginning of
// main(), before any coroutine/network API is used; replaces qtnetworkng 1.0's preferLibev().
// Without an explicit call the default backend is used (Qt on the GUI thread when the qt
// binding is linked, libev/Win elsewhere).
void useEventloop(EventLoopType type);

// Register a factory for a custom event loop backend (type >= 100). Built-in backends
// (Ev, Qt) are registered by qtng itself.
void registerEventLoop(EventLoopType type, EventLoopFactory factory);

}  // namespace qtng

#endif  // QTNG_EVENTLOOP_H
