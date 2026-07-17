using namespace std;

#include <cassert>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_set>

#include "qtng/eventloop.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/platform.h"
#include "qtng/private/eventloop_p.h"
#include "qtng/utils/logging.h"

NG_LOGGER("qtng.eventloop_win");

namespace qtng {

namespace {

void utf8ToWideChar(const string &utf8, wchar_t *wide, size_t wideSize)
{
    if (wideSize == 0) {
        return;
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide,
                                  static_cast<int>(wideSize - 1));
    if (len < 0) {
        len = 0;
    }
    wide[len] = 0;
}

WinWatcher *takeWatcher(map<int, WinWatcher *> &watchers, int watcherId)
{
    map<int, WinWatcher *>::iterator it = watchers.find(watcherId);
    if (it == watchers.end()) {
        return nullptr;
    }
    WinWatcher *watcher = it->second;
    watchers.erase(it);
    return watcher;
}

}  // namespace

class WinEventLoopCoroutinePrivate;


struct WinWatcher
{
    int id;
    WinWatcher();
    virtual ~WinWatcher();
};


struct IoWatcher: public WinWatcher{
    IoWatcher(EventLoopCoroutine::EventType event, intptr_t fd);
    virtual ~IoWatcher();

    EventLoopCoroutine::EventType event;
    intptr_t fd;
    Functor *callback;
};


struct TimerWatcher: public WinWatcher
{
    TimerWatcher(uint32_t interval, bool repeat, Functor *callback);
    virtual ~TimerWatcher();

    uint64_t at;
    uint32_t interval;
    Functor *callback;
    uint32_t repeat;
    uint32_t inUse;
};


WinWatcher::WinWatcher()
    : id(0) {}

WinWatcher::~WinWatcher() {}


IoWatcher::IoWatcher(EventLoopCoroutine::EventType event, intptr_t fd)
    : event(event), fd(fd), callback(nullptr)
{
}


IoWatcher::~IoWatcher()
{
    if (callback) {
        delete callback;
    }
}


TimerWatcher::TimerWatcher(uint32_t interval, bool repeat, Functor *callback)
    : at(0), interval(interval), callback(callback), repeat(repeat), inUse(false)
{
}


TimerWatcher::~TimerWatcher()
{
    delete callback;
}


class WinEventLoopCoroutinePrivate: public EventLoopCoroutinePrivate
{
public:
    WinEventLoopCoroutinePrivate(EventLoopCoroutine* parent);
    virtual ~WinEventLoopCoroutinePrivate() override;
public:
    virtual void run() override;
    virtual int createWatcher(EventLoopCoroutine::EventType event, intptr_t fd, Functor *callback) override;
    virtual void startWatcher(int watcherId) override;
    virtual void stopWatcher(int watcherId) override;
    virtual void removeWatcher(int watcherId) override;
    virtual void triggerIoWatchers(intptr_t fd) override;
    virtual int callLater(uint32_t msecs, Functor *callback) override;
    virtual int callRepeat(uint32_t msecs, Functor *callback) override;
    virtual void callLaterThreadSafe(uint32_t msecs, Functor *callback) override;
    virtual void cancelCall(int callbackId) override;
    virtual int exitCode() override;
    virtual bool runUntil(BaseCoroutine *coroutine) override;
    void doCallLater();
public:
    void updateIoMask(intptr_t fd);
    void stopWatcher(IoWatcher *watcher);
    void sendTimerEvent(TimerWatcher *watcher);
    void sendIoEvent(intptr_t fd, EventLoopCoroutine::EventType event);
    void createInternalWindow();
    void updateTimeStamp();
    void processTimers();
    int addTimer(TimerWatcher *watcher);
    HWND internalHwnd;
private:
    map<int, WinWatcher*> watchers;
    map<intptr_t, unordered_set<IoWatcher *> > activeSockets;

    multimap<uint64_t, TimerWatcher *> activeTimers;

    mutex mqMutex;
    queue<TimerWatcher *> callLaterQueue;
    bool interrupted;
    bool inProcessTimer;
    uint64_t perCnt;
    uint64_t timeCurrent;
    int nextWatcherId;
    int padding;
    NG_DECLARE_PUBLIC(EventLoopCoroutine)
    friend struct TriggerIoWatchersFunctor;
};



WinEventLoopCoroutinePrivate::WinEventLoopCoroutinePrivate(EventLoopCoroutine *parent)
    : EventLoopCoroutinePrivate(parent)
    , internalHwnd(nullptr)
    , interrupted(false)
    , inProcessTimer(false)
    , nextWatcherId(1)
{
    createInternalWindow();
    if (!QueryPerformanceFrequency((LARGE_INTEGER *)&perCnt)) {
        perCnt = 0;
    }
}


enum {
    WM_QTNG_SOCKETNOTIFIER = WM_USER,
    WM_QTNG_DO_CALL_LATER = WM_USER + 1,
    WM_QTNG_WAKEUP = WM_USER + 2,
};


WinEventLoopCoroutinePrivate::~WinEventLoopCoroutinePrivate()
{
    interrupted = true;
    if (internalHwnd) {
        for (map<intptr_t, unordered_set<IoWatcher *> >::iterator it = activeSockets.begin();
             it != activeSockets.end(); ++it) {
            WSAAsyncSelect(static_cast<SOCKET>(it->first), internalHwnd, 0, 0);
        }
        activeSockets.clear();
        activeTimers.clear();
        for (map<int, WinWatcher *>::iterator it = watchers.begin(); it != watchers.end(); ++it) {
            delete it->second;
        }
        watchers.clear();
        DestroyWindow(internalHwnd);
        internalHwnd = NULL;
        PostQuitMessage(0);
    }
}


void WinEventLoopCoroutinePrivate::run()
{
    if (internalHwnd && interrupted) {
        interrupted = false;
    }

    DWORD nCount = 0;
    HANDLE *pHandles = nullptr;
    do {
        processTimers();
        if (interrupted) {
            break;
        }
        
        MSG msg;
        bool haveMessage = PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
        if (!haveMessage) {
            uint64_t waittime = INFINITE;
            if (!activeTimers.empty()) {
                updateTimeStamp();
                uint64_t top_time = activeTimers.begin()->first;
                if (perCnt == 0) {
                    waittime = top_time > timeCurrent ? top_time - timeCurrent : 0;
                } else {
                    waittime = top_time > timeCurrent ? (double)(top_time - timeCurrent) / perCnt * 1000 : 0;
                }
            }
            if (waittime == 0) {
                continue;
            }
            DWORD waitRet = MsgWaitForMultipleObjectsEx(nCount, pHandles, static_cast<uint32_t>(waittime), QS_ALLINPUT, MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);
            if (waitRet == WAIT_TIMEOUT) {
                continue;
            }
            haveMessage = PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
            if (!haveMessage) {
                continue;
            }
        }
        if (msg.message == WM_QUIT) {
            return;
        }
        if (msg.message == WM_QTNG_WAKEUP) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        
    } while (true);
}


int WinEventLoopCoroutinePrivate::createWatcher(EventLoopCoroutine::EventType event, intptr_t fd, Functor *callback)
{
    IoWatcher *watcher = new IoWatcher(event, fd);
    watcher->callback = callback;
    int id = nextWatcherId++;
    watcher->id = id;
    watchers[id] = watcher;
    return id;
}


void WinEventLoopCoroutinePrivate::startWatcher(int watcherId)
{
    map<int, WinWatcher *>::iterator found = watchers.find(watcherId);
    if (found == watchers.end()) {
        return;
    }
    IoWatcher *watcher = dynamic_cast<IoWatcher*>(found->second);
    if (watcher) {
        map<intptr_t, unordered_set<IoWatcher *> >::iterator socketIt = activeSockets.find(watcher->fd);
        if (socketIt != activeSockets.end()) {
            socketIt->second.insert(watcher);
        } else {
            unordered_set<IoWatcher *> allWatchers;
            allWatchers.insert(watcher);
            activeSockets[watcher->fd] = allWatchers;
        }
        updateIoMask(watcher->fd);
    }
}


void WinEventLoopCoroutinePrivate::stopWatcher(int watcherId)
{
    map<int, WinWatcher *>::iterator found = watchers.find(watcherId);
    if (found == watchers.end()) {
        return;
    }
    IoWatcher *watcher = dynamic_cast<IoWatcher*>(found->second);
    if (watcher) {
        stopWatcher(watcher);
    }
}

void WinEventLoopCoroutinePrivate::stopWatcher(IoWatcher *watcher)
{
    bool found = false;
    map<intptr_t, unordered_set<IoWatcher *> >::iterator socketIt = activeSockets.find(watcher->fd);
    if (socketIt != activeSockets.end()) {
        found = socketIt->second.erase(watcher) > 0;
        if (socketIt->second.empty()) {
            activeSockets.erase(socketIt);
        }
    }
    if (found) {
        updateIoMask(watcher->fd);
    }
}


void WinEventLoopCoroutinePrivate::removeWatcher(int watcherId)
{
    IoWatcher *watcher = dynamic_cast<IoWatcher*>(takeWatcher(watchers, watcherId));
    if (watcher) {
        stopWatcher(watcher);
        delete watcher;
    }
}


struct TriggerIoWatchersFunctor: public Functor
{
    TriggerIoWatchersFunctor(int watcherId, WinEventLoopCoroutinePrivate *eventloop)
        :eventloop(eventloop), watcherId(watcherId) {}
    virtual ~TriggerIoWatchersFunctor() override;
    WinEventLoopCoroutinePrivate *eventloop;
    int watcherId;
    virtual bool operator()() override
    {
        IoWatcher *watcher = dynamic_cast<IoWatcher*>(takeWatcher(eventloop->watchers, watcherId));
        if (watcher) {
            (*watcher->callback)();
            delete watcher;
        }
        return true;
    }
};


TriggerIoWatchersFunctor::~TriggerIoWatchersFunctor() {}


void WinEventLoopCoroutinePrivate::triggerIoWatchers(intptr_t fd)
{
    map<intptr_t, unordered_set<IoWatcher *> >::iterator socketIt = activeSockets.find(fd);
    if (socketIt == activeSockets.end()) {
        return;
    }
    for (IoWatcher *watcher: socketIt->second) {
        callLater(0, new TriggerIoWatchersFunctor(watcher->id, this));
    }
    activeSockets.erase(socketIt);
}


int WinEventLoopCoroutinePrivate::addTimer(TimerWatcher *watcher)
{
    assert(!watcher->inUse);
    int timerId = watcher->id;
    if (!timerId) {
        timerId = nextWatcherId++;
        watcher->id = timerId;
        watchers[timerId] = watcher;
    }
    updateTimeStamp();
    if (perCnt == 0) {
        watcher->at = timeCurrent + watcher->interval;
    } else {
        watcher->at = timeCurrent + watcher->interval / 1000.0 * perCnt;
    }
    
    activeTimers.emplace(watcher->at, watcher);

    if (!inProcessTimer) {
        PostMessageW(internalHwnd, WM_QTNG_WAKEUP, 0, 0);
    }
    return timerId;
}


int WinEventLoopCoroutinePrivate::callLater(uint32_t msecs, Functor *callback)
{
    TimerWatcher *watcher = new TimerWatcher(msecs, false, callback);
    return addTimer(watcher);
}


void WinEventLoopCoroutinePrivate::doCallLater()
{
    lock_guard<mutex> locker(mqMutex);
    while (!callLaterQueue.empty()) {
        TimerWatcher *watcher = callLaterQueue.front();
        callLaterQueue.pop();
        addTimer(watcher);
    }
}

void WinEventLoopCoroutinePrivate::callLaterThreadSafe(uint32_t msecs, Functor *callback)
{
    TimerWatcher *watcher = new TimerWatcher(msecs, false, callback);
    lock_guard<mutex> locker(mqMutex);
    callLaterQueue.push(watcher);
    PostMessage(internalHwnd, WM_QTNG_DO_CALL_LATER, 0, 0);
}


int WinEventLoopCoroutinePrivate::callRepeat(uint32_t msecs, Functor *callback)
{
    assert(msecs > 0);
    TimerWatcher *watcher = new TimerWatcher(msecs, true, callback);
    return addTimer(watcher);
}


void WinEventLoopCoroutinePrivate::cancelCall(int callbackId)
{
    TimerWatcher *watcher = dynamic_cast<TimerWatcher*>(takeWatcher(watchers, callbackId));
    if (watcher) {
        pair<multimap<uint64_t, TimerWatcher *>::iterator, multimap<uint64_t, TimerWatcher *>::iterator> range = activeTimers.equal_range(watcher->at);
        for (multimap<uint64_t, TimerWatcher *>::iterator it = range.first; it != range.second; ++it) {
            if (it->second == watcher) {
                activeTimers.erase(it);
                break;
            }
        }
        if (watcher->inUse) {
            watcher->id = 0;
        } else {
            delete watcher;
        }
    }
}

int WinEventLoopCoroutinePrivate::exitCode()
{
    return 0;
}


bool WinEventLoopCoroutinePrivate::runUntil(BaseCoroutine *coroutine)
{
    BaseCoroutine *current = BaseCoroutine::current();
    if (loopCoroutine && loopCoroutine != current) {
        Deferred<BaseCoroutine *>::Callback here = [current](BaseCoroutine *) {
            if (current) {
                current->yield();
            }
        };
        int callbackId = coroutine->finished.addCallback(here);
        loopCoroutine->yield();
        coroutine->finished.remove(callbackId);
    } else {
        BaseCoroutine *old = loopCoroutine;
        loopCoroutine = current;
        Deferred<BaseCoroutine *>::Callback exitOneDepth = [this](BaseCoroutine *) {
            interrupted = true;
        };
        int callbackId = coroutine->finished.addCallback(exitOneDepth);
        run();
        loopCoroutine = old;
        coroutine->finished.remove(callbackId);
    }
    return true;
}


WinEventLoopCoroutine::WinEventLoopCoroutine()
    :EventLoopCoroutine(new WinEventLoopCoroutinePrivate(this))
{

}

struct QWindowsMessageWindowClassContext
{
    QWindowsMessageWindowClassContext();
    ~QWindowsMessageWindowClassContext();

    ATOM atom;
    wchar_t *className;
};


LRESULT QT_WIN_CALLBACK evl_win_internal_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE)
        return true;

#ifdef GWLP_USERDATA
    WinEventLoopCoroutinePrivate *d = reinterpret_cast<WinEventLoopCoroutinePrivate *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
#else
    WinEventLoopCoroutinePrivate *d = reinterpret_cast<WinEventLoopCoroutinePrivate *>(GetWindowLong(hwnd, GWL_USERDATA));
#endif
    switch (message) {
    case WM_QTNG_SOCKETNOTIFIER: {
        int event = WSAGETSELECTEVENT(lp);
        intptr_t fd = static_cast<intptr_t>(wp);
        if (event == FD_READ || event == FD_ACCEPT) {
            d->sendIoEvent(fd, EventLoopCoroutine::Read);
        } else if (event == FD_WRITE || event == FD_CONNECT) {
            d->sendIoEvent(fd, EventLoopCoroutine::Write);
        } else if (event == FD_CLOSE) {
            d->sendIoEvent(fd, EventLoopCoroutine::ReadWrite);
        } else {
            ngDebug() << "unknown select event!";
        }
    }
        break;
    case WM_QTNG_DO_CALL_LATER:
        d->doCallLater();
        break;
    }
    return DefWindowProc(hwnd, message, wp, lp);
}

QWindowsMessageWindowClassContext::QWindowsMessageWindowClassContext()
    : atom(0), className(nullptr)
{
    const string qClassName = string("WinEventLoopCoroutine_Internal_Widget")
        + to_string(uintptr_t(evl_win_internal_proc));
    className = new wchar_t[qClassName.size() + 1];
    utf8ToWideChar(qClassName, className, qClassName.size() + 1);

    WNDCLASSW wc;
    wc.style = 0;
    wc.lpfnWndProc = evl_win_internal_proc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hIcon = nullptr;
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = className;
    atom = RegisterClassW(&wc);
    if (!atom) {
        ngWarning() << "WinEventLoopCoroutine_Internal_Widget RegisterClass() failed";
        delete [] className;
        className = nullptr;
    }
}

QWindowsMessageWindowClassContext::~QWindowsMessageWindowClassContext()
{
    if (className) {
        UnregisterClassW(className, GetModuleHandle(nullptr));
        delete [] className;
    }
}

NG_GLOBAL_STATIC(QWindowsMessageWindowClassContext, qWindowsMessageWindowClassContext)


void WinEventLoopCoroutinePrivate::createInternalWindow()
{
    if (internalHwnd) {
        return;
    }

    QWindowsMessageWindowClassContext *ctx = qWindowsMessageWindowClassContext();
    if (!ctx->atom) {
        return;
    }

    internalHwnd = CreateWindowW(ctx->className,
                                ctx->className,
                                0,
                                0, 0, 0, 0,
                                HWND_MESSAGE,
                                nullptr,
                                GetModuleHandle(nullptr),
                                nullptr);

    if (!internalHwnd) {
        ngWarning() << "CreateWindow() for WinEventLoopCoroutine internal window failed";
        return;
    }

#ifdef GWLP_USERDATA
    SetWindowLongPtr(internalHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
#else
    SetWindowLong(internalHwnd, GWL_USERDATA, reinterpret_cast<LONG>(this));
#endif
}


void WinEventLoopCoroutinePrivate::sendTimerEvent(TimerWatcher *watcher)
{
    if (!watcher->repeat) {
        watchers.erase(watcher->id);
        watcher->id = 0;
    } else {
        addTimer(watcher);
    }
    watcher->inUse = true;
    (*watcher->callback)();
    assert(watcher->inUse);
    if (watcher->id == 0) {
        delete watcher;
    } else {
        watcher->inUse = false;
    }
}


void WinEventLoopCoroutinePrivate::sendIoEvent(intptr_t fd, EventLoopCoroutine::EventType event)
{
    if (event == EventLoopCoroutine::ReadWrite) {
        WSAAsyncSelect(static_cast<SOCKET>(fd), internalHwnd, 0, 0);
        map<intptr_t, unordered_set<IoWatcher *> >::iterator socketIt = activeSockets.find(fd);
        if (socketIt != activeSockets.end()) {
            const unordered_set<IoWatcher *> watcherSet = socketIt->second;
            for (unordered_set<IoWatcher *>::const_iterator it = watcherSet.begin(); it != watcherSet.end(); ++it) {
                IoWatcher *watcher = *it;
                int id = watcher->id;
                (*watcher->callback)();
                if (watchers.erase(id)) {
                    delete watcher;
                }
            }
            activeSockets.erase(socketIt);
        }
        for (map<int, WinWatcher *>::iterator it = watchers.begin(); it != watchers.end(); ) {
            IoWatcher *watcher = dynamic_cast<IoWatcher *>(it->second);
            if (NG_UNLIKELY(watcher && watcher->fd == fd)) {
                delete watcher;
                it = watchers.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        map<intptr_t, unordered_set<IoWatcher *> >::iterator socketIt = activeSockets.find(fd);
        if (socketIt == activeSockets.end()) {
            return;
        }
        for (IoWatcher *watcher: socketIt->second) {
            if (((event & EventLoopCoroutine::Read) && (watcher->event & EventLoopCoroutine::Read)) ||
                ((event & EventLoopCoroutine::Write) && (watcher->event & EventLoopCoroutine::Write))) {
                if (socketIt->second.count(watcher)) {
                    socketIt->second.erase(watcher);
                    (*watcher->callback)();
                }
            }
        }
        if (socketIt->second.empty()) {
            activeSockets.erase(socketIt);
            WSAAsyncSelect(static_cast<SOCKET>(fd), internalHwnd, 0, 0);
        } else {
            updateIoMask(fd);
        }
    }
}


void WinEventLoopCoroutinePrivate::updateIoMask(intptr_t fd)
{
    map<intptr_t, unordered_set<IoWatcher *> >::iterator socketIt = activeSockets.find(fd);
    if (socketIt == activeSockets.end()) {
        return;
    }
    const unordered_set<IoWatcher *> &fdWatchers = socketIt->second;
    long event = 0;
    for (IoWatcher *watcher: fdWatchers) {
        if (watcher->event & EventLoopCoroutine::Read) {
            event |= FD_READ | FD_ACCEPT | FD_CLOSE ;
        } else if (watcher->event & EventLoopCoroutine::Write) {
            event |= FD_WRITE | FD_CONNECT | FD_CLOSE;
        }
    }
    int result = WSAAsyncSelect(static_cast<SOCKET>(fd), internalHwnd, (event ? WM_QTNG_SOCKETNOTIFIER : 0), event);
    if (result && event) {
        ngDebug() << result << WSAGetLastError();
    }
}


void WinEventLoopCoroutinePrivate::processTimers()
{
    updateTimeStamp();
    inProcessTimer = true;

    uint64_t dstTime = perCnt == 0 ? timeCurrent : timeCurrent + 2e-4 * perCnt;
    while (!activeTimers.empty() && !interrupted) {
        multimap<uint64_t, TimerWatcher *>::iterator it = activeTimers.begin();
        if (it->first > dstTime) {
            break;
        }
        TimerWatcher *watcher = it->second;
        activeTimers.erase(it);
        sendTimerEvent(watcher);
    }
    inProcessTimer = false;
}


void WinEventLoopCoroutinePrivate::updateTimeStamp()
{
    if (perCnt == 0) {
#if _WIN32_WINNT >= 0x0600
        timeCurrent = GetTickCount64();
#else
        timeCurrent = static_cast<uint64_t>(GetTickCount());
#endif
    } else {
        QueryPerformanceCounter((LARGE_INTEGER *)&timeCurrent);
    }
}


}  // namespace qtng

