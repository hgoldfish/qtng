#define NOMINMAX 1
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "qtng/coroutine_utils.h"
#include "qtng/private/eventloop_p.h"
#include "qtng/private/local_socket_p.h"

using namespace std;

namespace qtng {

namespace {

// Named pipe names travel as UTF-8 through the public API (std::string) and
// have to become UTF-16 for the W entry points.
static wstring toWide(const string &s)
{
    if (s.empty()) {
        return wstring();
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) {
        return wstring();
    }
    wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], len);
    return w;
}

struct PipeOverlappedOp
{
    OVERLAPPED overlapped;
    shared_ptr<Event> done;
    DWORD bytesTransferred;
    DWORD errorCode;
    // Written by the APC on the thread that issued the I/O, read by
    // waitOverlapped()/cancelAndDrain() there and - since rememberOrphanedOp()
    // can prune an op parked by another thread - by other event loop threads as
    // well. volatile is not synchronization: as a plain bool this is a data
    // race, and it would not publish errorCode/bytesTransferred written just
    // before it either. Atomic with the default release/acquire does both.
    //
    // Invariant: errorCode/bytesTransferred are only read after this flag has
    // been observed true (or after the completion was signalled through `done`).
    // A caller that has not seen it true yet must not read them - cancelAndDrain()
    // returns exactly that fact so its callers cannot get it wrong.
    std::atomic<bool> finished;

    PipeOverlappedOp()
        : bytesTransferred(0)
        , errorCode(0)
        , finished(false)
    {
        memset(&overlapped, 0, sizeof(overlapped));
        done = make_shared<Event>();
    }
};

VOID CALLBACK localSocketCompletionRoutine(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered,
                                           LPOVERLAPPED lpOverlapped)
{
    PipeOverlappedOp *op = reinterpret_cast<PipeOverlappedOp *>(lpOverlapped);
    op->errorCode = dwErrorCode;
    op->bytesTransferred = dwNumberOfBytesTransfered;
    op->finished = true;
    shared_ptr<Event> done = op->done;
    EventLoopCoroutine *el = EventLoopCoroutine::get();
    if (done && el) {
        // Keep Event alive via shared_ptr even if the waiter coroutine was killed.
        el->callLater(0, new LambdaFunctor([done] { done->set(); }));
    }
}

// CancelIoEx() only exists on Vista and later, and the header does not declare
// it below _WIN32_WINNT 0x0600 - a stock MinGW build and the XP target the
// examples build both fail to compile without this gate.
//
// The XP fallback is CancelIo(), which is weaker in one way worth knowing:
// it only cancels I/O issued by the calling thread, while CancelIoEx() is
// per-handle across threads. A local socket keeps at most one operation in
// flight per direction and the coroutine holding that direction's lock issues
// it, so the fallback covers the common case; a close() from a different event
// loop thread than the one parked in the I/O may leave that I/O to time out on
// its own. That is still strictly better than not compiling, which is what the
// pre-fix code did on every default MinGW and XP build.
BOOL cancelPendingIo(HANDLE h, LPOVERLAPPED overlapped)
{
#if _WIN32_WINNT >= 0x0600
    return CancelIoEx(h, overlapped);
#else
    (void) overlapped;
    return CancelIo(h);
#endif
}

// State of a ConnectNamedPipe() that is queued on the listening instance.
// One of these stays alive across accept() calls so that the pipe is never left
// unattended while the server is handling a request. Whether a wait is really
// queued is tracked by LocalSocketPrivate::acceptArmed, not by the mere
// existence of this object: it survives a discard() so that a coroutine already
// parked on done->tryWait() keeps a valid object to look at.
struct AcceptWait
{
    OVERLAPPED overlapped;
    HANDLE waitHandle;
    shared_ptr<Event> done;
    EventLoopCoroutine *eventLoop;
    // True while the ConnectNamedPipe is still in flight. When the pipe was
    // already connected the OVERLAPPED never saw any I/O and
    // GetOverlappedResult() must not be consulted.
    bool pending;

    AcceptWait()
        : waitHandle(nullptr)
        , eventLoop(nullptr)
        , pending(false)
    {
        memset(&overlapped, 0, sizeof(overlapped));
    }

    ~AcceptWait() { reset(); }

    // Releases every resource of one accept cycle. The object itself survives so
    // that a coroutine parked on done->tryWait() can still look at it.
    void reset()
    {
        if (waitHandle != nullptr) {
            UnregisterWaitEx(waitHandle, INVALID_HANDLE_VALUE);
            waitHandle = nullptr;
        }
        if (overlapped.hEvent != nullptr) {
            CloseHandle(overlapped.hEvent);
        }
        memset(&overlapped, 0, sizeof(overlapped));
        eventLoop = nullptr;
        pending = false;
        done.reset();
    }

    NG_DISABLE_COPY(AcceptWait)
};

// The wait is registered with INFINITE, so timedOut is never true. Waking the
// waiter is all this has to do: the outcome of the I/O is read from the pipe
// with GetOverlappedResult() afterwards.
VOID CALLBACK connectWaitCallback(PVOID context, BOOLEAN timedOut)
{
    (void) timedOut;
    AcceptWait *wait = static_cast<AcceptWait *>(context);
    shared_ptr<Event> done = wait->done;
    EventLoopCoroutine *el = wait->eventLoop;
    if (done && el) {
        // The waiter coroutine may already be gone; keep Event alive anyway.
        el->callLaterThreadSafe(0, new LambdaFunctor([done] { done->set(); }));
    }
}

bool waitOverlapped(shared_ptr<PipeOverlappedOp> op, DWORD *bytesTransferred)
{
    if (op->finished) {
        if (bytesTransferred) {
            *bytesTransferred = op->bytesTransferred;
        }
        return op->errorCode == ERROR_SUCCESS || op->errorCode == ERROR_MORE_DATA;
    }
    try {
        if (!op->done->tryWait()) {
            return false;
        }
    } catch (...) {
        return false;
    }
    if (bytesTransferred) {
        *bytesTransferred = op->bytesTransferred;
    }
    return op->errorCode == ERROR_SUCCESS || op->errorCode == ERROR_MORE_DATA;
}

// The ops whose completion never arrived. A PipeOverlappedOp has to outlive the
// I/O it describes: localSocketCompletionRoutine() writes into it, and an APC
// that finds its op freed is a use-after-free. cancelAndDrain() therefore waits
// for the completion, and when it stops waiting it parks the op here instead of
// freeing it. Entries whose APC eventually runs are pruned on the next park so
// the list cannot grow without bound under sustained cancel pressure.
//
// The list is process-wide but cancelAndDrain() runs on whichever event loop
// thread hit the failed I/O, so two sockets on two threads can park at the same
// time. The critical section is a handful of vector operations with no yield
// point, so no other coroutine on the same thread can ever contend for the
// mutex (which would block the OS thread and hang the event loop); contention
// can only be cross-thread, where blocking briefly is the correct answer.
vector<shared_ptr<PipeOverlappedOp>> &orphanedOps()
{
    static vector<shared_ptr<PipeOverlappedOp>> ops;
    return ops;
}

mutex &orphanedOpsMutex()
{
    static mutex m;
    return m;
}

void rememberOrphanedOp(const shared_ptr<PipeOverlappedOp> &op)
{
    lock_guard<mutex> guard(orphanedOpsMutex());
    vector<shared_ptr<PipeOverlappedOp>> &ops = orphanedOps();
    for (size_t i = ops.size(); i > 0; --i) {
        if (ops[i - 1]->finished) {
            ops.erase(ops.begin() + static_cast<ptrdiff_t>(i - 1));
        }
    }
    ops.push_back(op);
}

// Returns true when the cancelled I/O finally completed, i.e. op->errorCode
// holds the real result. False means the completion never arrived and op has
// been parked in orphanedOps(): its errorCode is then still the constructor's
// zero, and reading it would both invent an error and race with the APC that
// may write it later.
bool cancelAndDrain(HANDLE h, const shared_ptr<PipeOverlappedOp> &op)
{
    cancelPendingIo(h, &op->overlapped);
    // The completion routine only runs while this thread is in an alertable
    // wait, so pump APCs until it has been here - that is the only point at
    // which op is guaranteed to be unreferenced by the kernel. SleepEx(0, TRUE)
    // drains any already-queued APC without sleeping; Coroutine::msleep(1) then
    // yields so other coroutines on this thread can run instead of blocking the
    // OS thread for up to 1s. The budget below only limits how long we keep
    // trying; giving up hands op to orphanedOps() rather than freeing it.
    const int maxAlertableWaits = 1000;  // 1000 * 1ms
    for (int i = 0; i < maxAlertableWaits && !op->finished; ++i) {
        SleepEx(0, TRUE);
        if (op->finished) {
            break;
        }
        Coroutine::msleep(1);
    }
    if (!op->finished) {
        rememberOrphanedOp(op);
        return false;
    }
    return true;
}

}  // namespace

void LocalSocketPrivate::setSocketDescriptor(intptr_t socketDescriptor)
{
    fd = socketDescriptor;
    if (fd == 0 || fd == -1) {
        setError(Socket::UnsupportedSocketOperationError, InvalidSocketErrorString);
        return;
    }
    state = Socket::ConnectedState;
    error = Socket::NoError;
}

bool LocalSocketPrivate::createSocket()
{
    // Named pipes are created in bind()/connect(), not here.
    return true;
}

bool LocalSocketPrivate::createPipeInstance(bool firstInstance)
{
    DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (firstInstance) {
        openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    }
    DWORD pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
    // Every handle counts against nMaxInstances, including the instances already
    // handed over to accepted connections, so a small value would break the
    // server as soon as enough connections are open at once. There is no accept
    // queue to size either, hence the unlimited value.
    const DWORD maxInstances = PIPE_UNLIMITED_INSTANCES;

    const wstring fullName = toWide(fullServerName);
    HANDLE h = CreateNamedPipeW(fullName.c_str(), openMode, pipeMode, maxInstances, 64 * 1024, 64 * 1024, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY) {
            setError(Socket::AddressInUseError, AddressInuseErrorString);
        } else if (err == ERROR_INVALID_NAME || err == ERROR_BAD_PATHNAME) {
            setError(Socket::SocketAddressNotAvailableError, AddressNotAvailableErrorString);
        } else {
            setError(Socket::UnknownSocketError, UnknownSocketErrorString);
        }
        return false;
    }
    fd = reinterpret_cast<intptr_t>(h);
    return true;
}

AcceptWait *ensureAcceptWait(LocalSocketPrivate *d)
{
    AcceptWait *wait = static_cast<AcceptWait *>(d->acceptWait);
    if (wait == nullptr) {
        wait = new AcceptWait();
        d->acceptWait = wait;
    }
    return wait;
}

void LocalSocketPrivate::releaseAcceptWait()
{
    AcceptWait *wait = static_cast<AcceptWait *>(acceptWait);
    if (wait == nullptr) {
        return;
    }
    acceptWait = nullptr;
    acceptArmed = false;
    ++acceptGeneration;
    delete wait;
}

int LocalSocketPrivate::startAcceptWait()
{
    if (acceptArmed) {
        return 1;  // already queued
    }
    const HANDLE pipe = reinterpret_cast<HANDLE>(fd);
    AcceptWait *wait = ensureAcceptWait(this);
    wait->reset();
    wait->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (wait->overlapped.hEvent == nullptr) {
        setError(Socket::OutOfMemoryError, OutOfMemoryErrorString);
        return -1;
    }
    wait->done = make_shared<Event>();
    wait->eventLoop = EventLoopCoroutine::get();
    // This cycle has its own identity from here on.
    ++acceptGeneration;

    const BOOL ok = ConnectNamedPipe(pipe, &wait->overlapped);
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    // ERROR_PIPE_CONNECTED: a client was already waiting for us.
    // ERROR_NO_DATA: it connected and hung up again before we got here. Its
    // connection - and whatever it managed to send - is still on this
    // instance, so deliver it just like a synchronous connect. Dropping it
    // (DisconnectNamedPipe + re-arm) used to lose the request for good and
    // then wait for a client that would never come back; a Unix peer gets such
    // a connection from the accept queue, so this keeps both sides alike.
    if (ok || err == ERROR_PIPE_CONNECTED || err == ERROR_NO_DATA) {
        wait->pending = false;
        wait->done->set();
        acceptArmed = true;
        return 1;
    }
    if (err != ERROR_IO_PENDING) {
        wait->reset();
        setError(Socket::UnknownSocketError, UnknownSocketErrorString);
        return -1;
    }
    if (!RegisterWaitForSingleObject(&wait->waitHandle, wait->overlapped.hEvent, connectWaitCallback, wait, INFINITE,
                                     WT_EXECUTEONLYONCE)) {
        // The I/O is already in flight; drain it before releasing the OVERLAPPED.
        cancelPendingIo(pipe, &wait->overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &wait->overlapped, &ignored, TRUE);
        wait->reset();
        setError(Socket::UnknownSocketError, UnknownSocketErrorString);
        return -1;
    }
    wait->pending = true;
    acceptArmed = true;
    return 1;
}

void LocalSocketPrivate::discardAcceptWait(bool cancelIo)
{
    AcceptWait *wait = static_cast<AcceptWait *>(acceptWait);
    if (wait == nullptr) {
        return;
    }
    const bool wasArmed = acceptArmed;
    // Cleared and re-stamped before the waiter is woken: accept() compares the
    // generation it captured before sleeping to tell a client that arrived from
    // a wait that was taken away.
    acceptArmed = false;
    ++acceptGeneration;
    if (!wasArmed) {
        return;
    }
    const HANDLE pipe = reinterpret_cast<HANDLE>(fd);
    // Only a truly in-flight ConnectNamedPipe may be cancelled and drained.
    // When the connect completed synchronously (pending == false) the
    // OVERLAPPED never saw any I/O, and GetOverlappedResult() on it is not
    // legal.
    if (cancelIo && wait->pending && pipe != nullptr && pipe != INVALID_HANDLE_VALUE) {
        cancelPendingIo(pipe, &wait->overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &wait->overlapped, &ignored, TRUE);
    }
    // A coroutine may already be parked on done->tryWait(). It keeps its own
    // shared_ptr<Event> alive, so waking it here is all it needs.
    if (wait->done) {
        wait->done->set();
    }
    wait->reset();
}

// Give up a listening instance that can no longer serve anybody: the queued
// ConnectNamedPipe is cancelled, the handle is closed and the socket stops
// claiming to listen, so neither fd nor the pipe name stays occupied by a
// handle nothing is serving.
void LocalSocketPrivate::dropListeningInstance()
{
    discardAcceptWait(true);
    if (fd != 0 && fd != -1) {
        const HANDLE h = reinterpret_cast<HANDLE>(fd);
        fd = 0;
        CloseHandle(h);
    }
    state = Socket::UnconnectedState;
}

bool LocalSocketPrivate::bind(const string &name, Socket::BindMode mode)
{
    // Named pipes have no inode leftover to reclaim and FIRST_PIPE_INSTANCE
    // already makes the name exclusive, so BindMode (incl. DontShareAddress)
    // has no effect on Windows. See LocalSocket::bind() for the contract.
    (void) mode;
    if (state != Socket::UnconnectedState) {
        setError(Socket::UnsupportedSocketOperationError, OperationUnsupportedErrorString);
        return false;
    }
    if (name.empty()) {
        setError(Socket::SocketAddressNotAvailableError, AddressNotAvailableErrorString);
        return false;
    }
    serverName = name;
    fullServerName = makeFullServerName(name);
    if (!createPipeInstance(true)) {
        return false;
    }
    state = Socket::BoundState;
    error = Socket::NoError;
    return true;
}

bool LocalSocketPrivate::listen(int backlog)
{
    // A named pipe has no accept queue: every client connection is served by its
    // own instance, and accept() replaces the listening one right away. The
    // backlog therefore has nothing to bound here and is ignored, unlike on
    // Unix where it reaches ::listen().
    (void) backlog;
    // Do not use checkState(): a prior wrong-state bind() may have set
    // UnsupportedSocketOperationError while leaving a perfectly usable Bound
    // socket, and that sticky error must not block listen().
    if (fd == 0 || fd == -1) {
        setError(Socket::UnsupportedSocketOperationError, InvalidSocketErrorString);
        return false;
    }
    if (state != Socket::BoundState) {
        setError(Socket::UnsupportedSocketOperationError, OperationUnsupportedErrorString);
        return false;
    }
    state = Socket::ListeningState;
    error = Socket::NoError;
    return true;
}

LocalSocket *LocalSocketPrivate::accept()
{
    if (!checkState() || state != Socket::ListeningState) {
        return nullptr;
    }

    while (true) {
        if (!checkState() || state != Socket::ListeningState) {
            return nullptr;
        }

        if (startAcceptWait() < 0) {
            dropListeningInstance();
            return nullptr;
        }

        AcceptWait *wait = static_cast<AcceptWait *>(acceptWait);
        // Hold our own reference: another coroutine may close the socket (and
        // release the wait) while we are parked on this event. The generation
        // tells whether this is still the wait we queued.
        const uint32_t generation = acceptGeneration;
        shared_ptr<Event> done = wait->done;
        bool waited = false;
        try {
            waited = done->tryWait();
        } catch (...) {
            discardAcceptWait(true);
            throw;
        }
        // Only a wait that was not replaced or taken away means a client
        // showed up: close() and discardAcceptWait() re-stamp the generation
        // before waking us, so a wake-up caused by them can never be read as a
        // connection.
        if (!waited || acceptGeneration != generation) {
            if (!checkState() || state != Socket::ListeningState) {
                return nullptr;
            }
            continue;
        }

        const HANDLE pipe = reinterpret_cast<HANDLE>(fd);
        if (wait->pending) {
            DWORD transferred = 0;
            if (!GetOverlappedResult(pipe, &wait->overlapped, &transferred, FALSE)) {
                const DWORD e = GetLastError();
                discardAcceptWait(false);
                if (e != ERROR_NO_DATA && e != ERROR_PIPE_CONNECTED) {
                    // The instance cannot serve anybody anymore; drop it and
                    // report instead of leaving a dead handle on fd.
                    dropListeningInstance();
                    setError(Socket::UnknownSocketError, UnknownSocketErrorString);
                    return nullptr;
                }
                // ERROR_NO_DATA is the same situation as in startAcceptWait():
                // the client is gone again, but its connection and payload are
                // still on this instance. ERROR_PIPE_CONNECTED means it is still
                // there. Either way the instance carries a real connection, so
                // fall through and hand it over instead of dropping it.
            }
        }
        // Final ownership check immediately before claiming the handle. close()
        // leaves ListeningState (and makes checkState() false) before it ever
        // DisconnectNamedPipe's — that waits until after the lock drain — so a
        // mismatch here means the pipe is no longer ours. Do not compare
        // acceptGeneration: the ERROR_NO_DATA path above discards its own wait
        // (and bumps the generation) as part of a successful connect.
        // The remaining window to fd = 0 is then safe: a racing close() sees fd
        // already claimed and must not disconnect a connection we took.
        if (state != Socket::ListeningState || !checkState()) {
            discardAcceptWait(false);
            return nullptr;
        }

        // Hand the connected pipe over, then put a fresh instance in place and
        // queue its ConnectNamedPipe right away. In the normal case the name is
        // served again before this call returns, so a client that shows up
        // meanwhile finds an instance waiting instead of a busy one. When the
        // replacement instance cannot be created the name is left unserved and
        // such a client simply fails to connect - which is the honest answer
        // for a server that just lost its pipe.
        const intptr_t acceptedFd = fd;
        fd = 0;
        discardAcceptWait(false);
        if (createPipeInstance(false)) {
            state = Socket::ListeningState;
            if (startAcceptWait() < 0) {
                // Keep the accepted connection usable even if we cannot keep
                // listening, but do not leave a half-initialized instance on fd:
                // it would still occupy the pipe name and one instance slot.
                dropListeningInstance();
            }
        } else {
            // Keep the accepted connection usable even if we cannot keep listening.
            state = Socket::UnconnectedState;
        }

        LocalSocket *conn = new LocalSocket(acceptedFd);
        conn->d_func()->serverName = serverName;
        conn->d_func()->fullServerName = fullServerName;
        return conn;
    }
}

bool LocalSocketPrivate::connect(const string &name)
{
    if (state != Socket::UnconnectedState && state != Socket::BoundState) {
        setError(Socket::UnsupportedSocketOperationError, OperationUnsupportedErrorString);
        return false;
    }
    if (name.empty()) {
        setError(Socket::SocketAddressNotAvailableError, AddressNotAvailableErrorString);
        return false;
    }
    // bind() owns a pipe instance under this name. The client handle is about
    // to replace fd, so that instance has to be closed first. Otherwise the
    // name stays taken after this socket is destroyed.
    if (state == Socket::BoundState && fd != 0 && fd != -1) {
        const HANDLE bound = reinterpret_cast<HANDLE>(fd);
        fd = 0;
        DisconnectNamedPipe(bound);
        CloseHandle(bound);
    }
    serverName = name;
    fullServerName = makeFullServerName(name);
    state = Socket::ConnectingState;

    // Every accepted connection is served by the listening instance itself, so
    // the server has to create a replacement and queue a new ConnectNamedPipe
    // before it can serve the next client. A client that arrives inside that
    // window gets ERROR_PIPE_BUSY. WaitNamedPipeW() would block the whole OS
    // thread - and with it every coroutine scheduled on it - so yield to the
    // scheduler instead.
    const int maxRetries = 1000;  // 1000 * 5ms = 5s
    const wstring fullName = toWide(fullServerName);
    for (int i = 0; i < maxRetries; ++i) {
        // close()/abort() runs while this coroutine is in msleep and sets
        // Unconnected before it waits for the write lock. Leave before
        // CreateFile so a connect that was cancelled cannot still succeed.
        // Those two do not record an error; this failure belongs to connect().
        if (state != Socket::ConnectingState) {
            setError(Socket::UnfinishedSocketOperationError, "The socket operation was canceled");
            return false;
        }
        HANDLE h = CreateFileW(fullName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            // The pipe was created with PIPE_TYPE_BYTE, but a message mode
            // client may have left the instance in message mode, and every
            // read would then return one message at a time instead of the byte
            // stream both sides agreed on. Refuse the connection rather than
            // silently speaking the wrong dialect.
            DWORD mode = PIPE_READMODE_BYTE;
            if (!SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) {
                setError(Socket::UnknownSocketError, UnknownSocketErrorString);
                CloseHandle(h);
                state = Socket::UnconnectedState;
                return false;
            }
            fd = reinterpret_cast<intptr_t>(h);
            state = Socket::ConnectedState;
            error = Socket::NoError;
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            Coroutine::msleep(5);
            continue;
        }
        if (err == ERROR_FILE_NOT_FOUND) {
            setError(Socket::SocketAddressNotAvailableError, AddressNotAvailableErrorString);
        } else if (err == ERROR_ACCESS_DENIED) {
            setError(Socket::SocketAccessError, AccessErrorString);
        } else {
            setError(Socket::ConnectionRefusedError, ConnectionRefusedErrorString);
        }
        state = Socket::UnconnectedState;
        return false;
    }
    if (state != Socket::ConnectingState) {
        setError(Socket::UnfinishedSocketOperationError, "The socket operation was canceled");
        return false;
    }
    setError(Socket::SocketTimeoutError, TimeOutErrorString);
    state = Socket::UnconnectedState;
    return false;
}

void LocalSocketPrivate::close()
{
    // Cancel the queued ConnectNamedPipe and every pending read/write so that
    // accept()/recv()/send() wake up, but keep the handle open until those
    // coroutines have left: a woken waiter still calls GetOverlappedResult()
    // or cancels I/O on it, and a handle value that had already been closed
    // could by then belong to somebody else entirely.
    //
    // DisconnectNamedPipe waits until after the lock drain on purpose. Doing it
    // up front raced accept()'s handoff: state was already Unconnected while
    // accept() still claimed fd and returned a pipe whose server end was gone.
    // After drain, either accept() took fd (connection survives, we must not
    // disconnect) or it bailed on state/checkState (handle still ours here).
    discardAcceptWait(true);
    const bool disconnectPipe = (state == Socket::ListeningState || state == Socket::BoundState);
    if (fd != 0 && fd != -1) {
        cancelPendingIo(reinterpret_cast<HANDLE>(fd), nullptr);
    }
    state = Socket::UnconnectedState;

    if (readLock.isLocked()) {
        readLock.tryAcquire();
        readLock.release();
    }
    if (writeLock.isLocked()) {
        writeLock.tryAcquire();
        writeLock.release();
    }

    if (fd != 0 && fd != -1) {
        const HANDLE h = reinterpret_cast<HANDLE>(fd);
        fd = 0;
        if (disconnectPipe) {
            DisconnectNamedPipe(h);
        }
        CloseHandle(h);
    }
}

void LocalSocketPrivate::abort()
{
    // Not a synonym for close(): wake waiters and release the handle at once,
    // without draining readLock/writeLock. Safe from recv()/send() while the
    // current coroutine still holds a lock; unsafe as "graceful server stop"
    // if another coroutine is mid-accept — that path must use close().
    discardAcceptWait(true);
    if (fd != 0 && fd != -1) {
        const HANDLE h = reinterpret_cast<HANDLE>(fd);
        cancelPendingIo(h, nullptr);
        if (state == Socket::ListeningState || state == Socket::BoundState) {
            DisconnectNamedPipe(h);
        }
        fd = 0;
        CloseHandle(h);
    }
    state = Socket::UnconnectedState;
}

int32_t LocalSocketPrivate::peek(char *data, int32_t size)
{
    if (!checkState() || size <= 0) {
        return -1;
    }
    HANDLE h = reinterpret_cast<HANDLE>(fd);
    DWORD available = 0;
    DWORD bytesRead = 0;
    if (!PeekNamedPipe(h, data, static_cast<DWORD>(size), &bytesRead, &available, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            return -1;
        }
        return 0;
    }
    if (bytesRead == 0 && available == 0) {
        return 0;
    }
    return static_cast<int32_t>(bytesRead);
}

int32_t LocalSocketPrivate::recv(char *data, int32_t size, bool all)
{
    if (!checkState() || size <= 0) {
        return -1;
    }
    HANDLE h = reinterpret_cast<HANDLE>(fd);
    int32_t total = 0;
    while (total < size) {
        if (!checkState()) {
            return total == 0 ? -1 : total;
        }

        shared_ptr<PipeOverlappedOp> op = make_shared<PipeOverlappedOp>();
        BOOL ok = ReadFileEx(h, data + total, static_cast<DWORD>(size - total), &op->overlapped,
                             localSocketCompletionRoutine);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
                setError(Socket::RemoteHostClosedError, RemoteHostClosedErrorString);
                // Same as SocketPrivate::recv(): EOF also marks the socket dead,
                // so state()/isValid() report the hangup the way a Socket does.
                abort();
                return total;
            }
            setError(Socket::NetworkError, ReadErrorString);
            abort();
            return total == 0 ? -1 : total;
        }

        DWORD transferred = 0;
        if (!waitOverlapped(op, &transferred)) {
            if (!cancelAndDrain(h, op)) {
                // Whether this read took effect is unknowable once the completion
                // is gone (see cancelAndDrain()); call it failed rather than
                // report bytes nobody counted.
                setError(Socket::UnfinishedSocketOperationError, "The socket operation was canceled");
                return total == 0 ? -1 : total;
            }
            if (op->errorCode == ERROR_OPERATION_ABORTED) {
                return total == 0 ? -1 : total;
            }
            if (op->errorCode == ERROR_BROKEN_PIPE || op->errorCode == ERROR_HANDLE_EOF) {
                setError(Socket::RemoteHostClosedError, RemoteHostClosedErrorString);
                abort();
                return total;
            }
            setError(Socket::NetworkError, ReadErrorString);
            abort();
            return total == 0 ? -1 : total;
        }

        if (op->errorCode == ERROR_BROKEN_PIPE || op->errorCode == ERROR_HANDLE_EOF
            || (op->errorCode == ERROR_SUCCESS && transferred == 0)) {
            setError(Socket::RemoteHostClosedError, RemoteHostClosedErrorString);
            abort();
            return total;
        }
        if (op->errorCode != ERROR_SUCCESS && op->errorCode != ERROR_MORE_DATA) {
            setError(Socket::NetworkError, ReadErrorString);
            abort();
            return total == 0 ? -1 : total;
        }

        total += static_cast<int32_t>(transferred);
        if (!all) {
            return total;
        }
    }
    return total;
}

int32_t LocalSocketPrivate::send(const char *data, int32_t size, bool all)
{
    if (!checkState() || size <= 0) {
        return -1;
    }
    HANDLE h = reinterpret_cast<HANDLE>(fd);
    int32_t sent = 0;
    while (sent < size) {
        if (!checkState()) {
            // SocketPrivate::send() contract: a single send() reports the bytes
            // that went out, while a partial sendall() is an error.
            return all ? -1 : sent;
        }
        DWORD chunk = static_cast<DWORD>(std::min<int32_t>(size - sent, 64 * 1024));
        shared_ptr<PipeOverlappedOp> op = make_shared<PipeOverlappedOp>();
        BOOL ok = WriteFileEx(h, data + sent, chunk, &op->overlapped, localSocketCompletionRoutine);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                setError(Socket::RemoteHostClosedError, RemoteHostClosedErrorString);
                return -1;
            }
            setError(Socket::UnknownSocketError, WriteErrorString);
            abort();
            return -1;
        }

        DWORD transferred = 0;
        if (!waitOverlapped(op, &transferred)) {
            if (!cancelAndDrain(h, op)) {
                // Whether this write took effect is unknowable once the completion
                // is gone (see cancelAndDrain()); call it failed rather than
                // report bytes nobody counted.
                setError(Socket::UnfinishedSocketOperationError, "The socket operation was canceled");
                return -1;
            }
            if (op->errorCode == ERROR_BROKEN_PIPE || op->errorCode == ERROR_NO_DATA) {
                setError(Socket::RemoteHostClosedError, RemoteHostClosedErrorString);
                return -1;
            }
            setError(Socket::UnknownSocketError, WriteErrorString);
            abort();
            return -1;
        }
        if (op->errorCode != ERROR_SUCCESS) {
            if (op->errorCode == ERROR_BROKEN_PIPE || op->errorCode == ERROR_NO_DATA) {
                setError(Socket::RemoteHostClosedError, RemoteHostClosedErrorString);
                return -1;
            }
            setError(Socket::UnknownSocketError, WriteErrorString);
            abort();
            return -1;
        }
        // A write that reports success but moved nothing means the peer is
        // gone. Without this, sendall() would retry the same offset forever.
        if (transferred == 0) {
            setError(Socket::RemoteHostClosedError, RemoteHostClosedErrorString);
            return -1;
        }
        sent += static_cast<int32_t>(transferred);
        if (!all) {
            return sent;
        }
    }
    return sent;
}

}  // namespace qtng
