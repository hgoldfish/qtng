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

}  // namespace qtng

#endif  // QTNG_EVENTLOOP_H
