#include "qtng/utils/platform.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

#ifdef NG_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

#include "qtng/coroutine_utils.h"
#include "qtng/coroutine.h"
#include "qtng/private/coroutine_utils_p.h"
#include "qtng/eventloop.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.coroutine");

namespace qtng {

void ngThreadEntry(NgThread *self)
{
    self->run();
    self->finished.store(true);
    self->notifyFinished();
}

NgThread::~NgThread()
{
    if (thread.joinable()) {
        // Joining the current thread would self-deadlock (EDEADLK); detach
        // instead. Callers must not destroy a still-running NgThread from another
        // thread without joining first (ThreadPool joins its workers in its
        // destructor; CoroutineThread joins in wait()).
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
        } else {
            thread.join();
        }
    }
}

void NgThread::start()
{
    if (started.exchange(true)) {
        return;
    }
    finished.store(false);
    thread = std::thread(ngThreadEntry, this);
}

void NgThread::wait()
{
    if (thread.joinable()) {
        thread.join();
    }
}

void NgThread::addFinishedCallback(const function<void()> &callback)
{
    {
        lock_guard<mutex> lock(finishedCallbacksMutex);
        if (!finished.load()) {
            finishedCallbacks.push_back(callback);
            return;
        }
    }
    // The thread has already finished; invoke the callback inline so the waiter
    // never observes a missing notification due to a TOCTOU race with isFinished().
    callback();
}

void NgThread::notifyFinished()
{
    vector<function<void()>> callbacks;
    {
        lock_guard<mutex> lock(finishedCallbacksMutex);
        callbacks.swap(finishedCallbacks);
    }
    for (const function<void()> &callback : callbacks) {
        callback();
    }
}

bool LambdaFunctor::operator()()
{
    callback();
    return true;
}

class MarkDoneFunctor : public Functor
{
public:
    explicit MarkDoneFunctor(const shared_ptr<Event> &done)
        : done(done)
    {
    }
    bool operator()() override;

    shared_ptr<Event> done;
};

bool MarkDoneFunctor::operator()()
{
    done->set();
    return true;
}

namespace {

ThreadPool &sharedThreadPool()
{
    // Process-lifetime pool: never destroyed, so workers are not joined during
    // static teardown (the event loop is already gone by then).
    static ThreadPool *const pool = new ThreadPool;
    return *pool;
}

}  // namespace

shared_ptr<Event> spawnInThread(const function<void()> &func)
{
    return sharedThreadPool().spawn(func);
}

void callInThread(const function<void()> &func)
{
    sharedThreadPool().call(func);
}

class CoroutineThreadPrivate : public NgThread
{
public:
    explicit CoroutineThreadPrivate(uint32_t capacity)
        : tasks(capacity)
    {
    }
    void run() override
    {
        shared_ptr<EventLoopCoroutine> loop = currentLoop()->getOrCreate();
        shared_ptr<Coroutine> worker(Coroutine::spawn([this] {
            CoroutineGroup operations;
            while (true) {
                function<void()> f = tasks.get();
                if (!f) {
                    return;
                }
                operations.spawn(f);
            }
        }));
        loop->runUntil(worker.get());
    }
    void apply(const function<void()> &f) { tasks.put(f); }

private:
    ThreadQueue<function<void()>> tasks;
};

CoroutineThread::CoroutineThread(uint32_t capacity)
    : dd_ptr(new CoroutineThreadPrivate(capacity))
{
}

CoroutineThread::~CoroutineThread()
{
    delete dd_ptr;
}

void CoroutineThread::start()
{
    dd_ptr->start();
}

void CoroutineThread::wait()
{
    dd_ptr->wait();
}

bool CoroutineThread::isFinished() const
{
    return dd_ptr->isFinished();
}

void CoroutineThread::apply(const function<void()> &f)
{
    dd_ptr->apply(f);
}

bool waitThread(std::thread &thread)
{
    if (!thread.joinable()) {
        return true;
    }
    // std::thread has no "finished" notification, so perform the join on a
    // dedicated worker thread (via callInThread) while the calling coroutine
    // yields on an Event. This keeps the event loop responsive instead of
    // blocking the OS thread on thread.join().
    return callInThread<bool>([&thread] {
        thread.join();
        return true;
    });
}

#ifdef NG_OS_UNIX
bool waitProcessPid(int pid)
{
    if (pid <= 0) {
        return false;
    }
    return callInThread<bool>([pid] {
        int wstatus = 0;
        return waitpid(pid, &wstatus, 0) == pid;
    });
}
#elif defined(NG_OS_WIN)
bool waitint /*pid*/(void *handle)
{
    if (!handle) {
        return false;
    }
    return callInThread<bool>([handle] {
        return WaitForSingleObject(static_cast<HANDLE>(handle), INFINITE) == WAIT_OBJECT_0;
    });
}
#endif

CoroutineGroup::CoroutineGroup()
    : alive(make_shared<atomic<bool>>(true))
{
}

CoroutineGroup::~CoroutineGroup()
{
    alive->store(false);
    killall(true);
}

bool CoroutineGroup::add(shared_ptr<Coroutine> coroutine, const string &name)
{
    if (!name.empty()) {
        if (get(name)) {
            return false;
        }
        coroutine->setObjectName(name);
    }
    shared_ptr<atomic<bool>> token = alive;
    coroutine->finished.addCallback([token, this](BaseCoroutine *coroutine) {
        if (!token->load()) {
            return;
        }
        deleteCoroutine(coroutine);
    });
    coroutines.insert(coroutine);
    return true;
}

shared_ptr<Coroutine> CoroutineGroup::get(const string &name)
{
    for (const shared_ptr<Coroutine> &coroutine : coroutines) {
        if (coroutine->objectName() == name) {
            return coroutine;
        }
    }
    return shared_ptr<Coroutine>();
}

bool CoroutineGroup::has(const string &name)
{
    return static_cast<bool>(get(name));
}

bool CoroutineGroup::isCurrent(const string &name)
{
    for (const shared_ptr<Coroutine> &coroutine : coroutines) {
        if (coroutine->objectName() == name && coroutine.get() == Coroutine::current()) {
            return true;
        }
    }
    return false;
}

bool CoroutineGroup::kill(const string &name, bool join)
{
    shared_ptr<Coroutine> found = get(name);
    if (found) {
        if (found.get() == Coroutine::current()) {
            ngWarning() << "killing current coroutine?";
        } else {
            if (join) {
                if (found->isRunning()) {
                    found->setPrevious(BaseCoroutine::current());
                    found->raise(new CoroutineExitException());
                } else {
                    found->kill();
                }
            } else {
                found->kill();
            }
            return true;
        }
    }
    return false;
}

bool CoroutineGroup::killall(bool join)
{
    bool done = false;
    unordered_set<shared_ptr<Coroutine>> copy = coroutines;
    if (join) {
        BaseCoroutine *current = BaseCoroutine::current();
        for (shared_ptr<Coroutine> coroutine : copy) {
            if (coroutine.get() == Coroutine::current()) {
                continue;
            }
            if (coroutine->isRunning()) {
                coroutine->setPrevious(current);
                coroutine->raise(new CoroutineExitException());
            } else {
                coroutine->kill();
            }
            done = true;
        }
    } else {
        for (shared_ptr<Coroutine> coroutine : copy) {
            if (coroutine.get() == Coroutine::current()) {
                continue;
            }
            coroutine->kill();
            done = true;
        }
    }
    return done;
}

bool CoroutineGroup::join(const string &name)
{
    shared_ptr<Coroutine> found = get(name);
    if (found) {
        if (found.get() == Coroutine::current()) {
            ngWarning() << "joining current coroutine?";
        } else {
            found->join();
            return true;
        }
    }
    return false;
}

bool CoroutineGroup::joinall()
{
    bool hasCoroutines = !coroutines.empty();
    unordered_set<shared_ptr<Coroutine>> copy = coroutines;
    for (shared_ptr<Coroutine> coroutine : copy) {
        if (coroutine.get() == Coroutine::current()) {
            continue;
        }
        coroutine->join();
    }
    return hasCoroutines;
}

shared_ptr<Coroutine> CoroutineGroup::any()
{
    shared_ptr<ValueEvent<shared_ptr<Coroutine>>> event = make_shared<ValueEvent<shared_ptr<Coroutine>>>();

    vector<pair<weak_ptr<Coroutine>, int>> toRemove;
    for (shared_ptr<Coroutine> c : coroutines) {
        weak_ptr<Coroutine> cw = c;
        int callbackId = c->finished.addCallback([event, cw](BaseCoroutine *) { event->send(cw.lock()); });
        toRemove.emplace_back(cw, callbackId);
    }
    try {
        shared_ptr<Coroutine> c = event->tryWait();
        for (const pair<weak_ptr<Coroutine>, int> &item : toRemove) {
            if (shared_ptr<Coroutine> cc = item.first.lock()) {
                cc->finished.remove(item.second);
            }
        }
        return c;
    } catch (...) {
        for (const pair<weak_ptr<Coroutine>, int> &item : toRemove) {
            if (shared_ptr<Coroutine> cc = item.first.lock()) {
                cc->finished.remove(item.second);
            }
        }
        throw;
    }
}

class DeleteCoroutineFunctor : public Functor
{
public:
    ~DeleteCoroutineFunctor() override = default;
    bool operator()() override;
    shared_ptr<BaseCoroutine> coroutine;
};

bool DeleteCoroutineFunctor::operator()()
{
    return true;
}

void CoroutineGroup::deleteCoroutine(BaseCoroutine *baseCoroutine)
{
    Coroutine *coroutine = dynamic_cast<Coroutine *>(baseCoroutine);
    assert(coroutine != nullptr);
    shared_ptr<Coroutine> c = coroutine->shared_from_this();
    DeleteCoroutineFunctor *callback = new DeleteCoroutineFunctor();
    callback->coroutine = c;
    EventLoopCoroutine::get()->callLater(0, callback);
    coroutines.erase(c);
}

class ThreadPoolWorkItem
{
public:
    ThreadPoolWorkItem()
        : done(make_shared<Event>())
        , error(make_shared<exception_ptr>())
    {
    }
    function<void()> makeResult;
    function<void()> onLoopFinished;
    shared_ptr<Event> done;
    weak_ptr<EventLoopCoroutine> eventloop;
    shared_ptr<exception_ptr> error;
};

class ThreadPool::WorkThread : public NgThread
{
public:
    WorkThread() = default;
    ThreadPool::SubmitResult enqueue(function<void()> func, function<void()> onLoopFinished);
    void kill();

private:
    void run() override;

    deque<ThreadPoolWorkItem> queue;
    mutex queueMutex;
    condition_variable hasWork;
    atomic<bool> exiting{false};
};

ThreadPool::SubmitResult ThreadPool::WorkThread::enqueue(function<void()> func, function<void()> onLoopFinished)
{
    ThreadPoolWorkItem item;
    item.makeResult = std::move(func);
    item.onLoopFinished = std::move(onLoopFinished);
    item.eventloop = currentLoop()->get();
    SubmitResult result;
    result.done = item.done;
    result.error = item.error;
    {
        lock_guard<mutex> lock(queueMutex);
        if (!exiting.load()) {
            queue.push_back(std::move(item));
            hasWork.notify_all();
            return result;
        }
    }
    if (item.onLoopFinished) {
        item.onLoopFinished();
    }
    result.done->set();
    return result;
}

void ThreadPool::WorkThread::kill()
{
    {
        lock_guard<mutex> lock(queueMutex);
        exiting.store(true);
    }
    hasWork.notify_all();
    wait();
}

void ThreadPool::WorkThread::run()
{
    while (true) {
        ThreadPoolWorkItem item;
        {
            unique_lock<mutex> lock(queueMutex);
            hasWork.wait(lock, [this] { return exiting.load() || !queue.empty(); });
            if (queue.empty()) {
                return;
            }
            item = std::move(queue.front());
            queue.pop_front();
        }
        shared_ptr<EventLoopCoroutine> loop = item.eventloop.lock();
        if (!loop) {
            // Cannot signal Event or recycle: both need the submitting loop.
            // Dropping onLoopFinished is safe because workers still owns *this.
            return;
        }
        try {
            item.makeResult();
        } catch (...) {
            if (item.error) {
                *item.error = current_exception();
            }
        }
        if (item.onLoopFinished) {
            loop->callLaterThreadSafe(0, new LambdaFunctor(std::move(item.onLoopFinished)));
        }
        loop->callLaterThreadSafe(0, new MarkDoneFunctor(item.done));
    }
}

ThreadPool::ThreadPool(int threads)
    : alive(make_shared<atomic<bool>>(true))
{
    if (threads <= 0) {
        unsigned int n = thread::hardware_concurrency();
        if (n == 0) {
            n = 4;
        }
        semaphore = make_shared<Semaphore>(static_cast<int>(n * 2 + 1));
    } else {
        semaphore = make_shared<Semaphore>(threads);
    }
}

ThreadPool::~ThreadPool()
{
    alive->store(false);
    for (shared_ptr<WorkThread> &thread : workers) {
        thread->kill();
    }
}

ThreadPool::SubmitResult ThreadPool::submit(function<void()> func)
{
    auto alreadyDone = []() {
        SubmitResult result;
        result.done = make_shared<Event>();
        result.done->set();
        result.error = make_shared<exception_ptr>();
        return result;
    };
    shared_ptr<Semaphore> sem = semaphore;
    if (!sem->tryAcquire()) {
        return alreadyDone();
    }
    if (!alive->load()) {
        sem->release();
        return alreadyDone();
    }
    shared_ptr<WorkThread> thread;
    if (idle.empty()) {
        thread = make_shared<WorkThread>();
        thread->start();
        workers.push_back(thread);
    } else {
        thread = idle.front();
        idle.erase(idle.begin());
    }
    shared_ptr<atomic<bool>> token = alive;
    return thread->enqueue(std::move(func), [this, thread, token, sem]() {
        sem->release();
        // ~ThreadPool kills every worker in `workers`; do not join from here.
        if (token->load()) {
            idle.push_back(thread);
        }
    });
}

void ThreadPool::call(function<void()> func)
{
    SubmitResult result = submit(std::move(func));
    result.done->tryWait();
    if (result.error && *result.error) {
        rethrow_exception(*result.error);
    }
}

shared_ptr<Event> ThreadPool::spawn(const function<void()> &func)
{
    return submit([func] {
               try {
                   func();
               } catch (const exception &e) {
                   ngWarning() << "thread pool task threw std::exception: " << e.what();
               } catch (...) {
                   ngWarning() << "thread pool task threw an unknown exception";
               }
           })
            .done;
}

}  // namespace qtng
