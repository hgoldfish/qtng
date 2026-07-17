#ifndef QTNG_COROUTINE_UTILS_P_H
#define QTNG_COROUTINE_UTILS_P_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "qtng/locks.h"

namespace qtng {

class EventLoopCoroutine;

// NgThread is an internal primitive backed by std::thread. It is intentionally
// not part of the public API: its destructor joins the OS thread (a blocking
// operation that must not run on the event loop thread while coroutines are
// alive), so callers must not destroy a running NgThread from a coroutine.
class NgThread
{
public:
    NgThread() = default;
    virtual ~NgThread();

    void start();
    void wait();
    bool isFinished() const { return finished.load(); }
    // The callback is invoked exactly once when the thread finishes; if the
    // thread has already finished it is invoked synchronously from the caller's
    // thread. It is safe to call set() on a ThreadEvent from inside the callback.
    void addFinishedCallback(const std::function<void()> &callback);

protected:
    virtual void run() = 0;

private:
    void notifyFinished();

    std::thread thread;
    std::atomic<bool> finished{false};
    std::atomic<bool> started{false};
    std::mutex finishedCallbacksMutex;
    std::vector<std::function<void()>> finishedCallbacks;
    friend void ngThreadEntry(NgThread *self);
};

class DeferCallThread : public NgThread
{
public:
    DeferCallThread(std::function<void()> makeResult, std::shared_ptr<Event> done,
                    std::shared_ptr<EventLoopCoroutine> eventloop);
    void run() override;

private:
    std::function<void()> makeResult;
    std::shared_ptr<Event> done;
    std::weak_ptr<EventLoopCoroutine> eventloop;
};

}  // namespace qtng

#endif  // QTNG_COROUTINE_UTILS_P_H
