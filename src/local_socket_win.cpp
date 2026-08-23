#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "qtng/private/local_socket_p.h"
#include "qtng/local_socket.h"
#include "qtng/locks.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/platform.h"

using namespace std;

NG_LOGGER("qtng.local_socket_win");

namespace qtng {

static wstring utf8ToWide(const string &utf8)
{
    if (utf8.empty()) {
        return wstring();
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &wide[0], len);
    return wide;
}

static wstring fullPipeName(const string &name)
{
    wstring wide = utf8ToWide(name);
    if (wide.rfind(L"\\\\?\\pipe\\", 0) == 0 || wide.rfind(L"\\\\.\\pipe\\", 0) == 0) {
        return wide;
    }
    return wstring(L"\\\\.\\pipe\\") + wide;
}

enum class PipeOp {
    Accept,
    Connect,
    Peek,
    Read,
    Write,
};

// A single blocking operation executed on the I/O thread. Ownership rules for
// handle:
// - Connect: the thread creates a handle on success and hands it to the caller;
//   a cancelled Connect closes it here.
// - Accept: the handle belongs to the caller (server instance); never closed.
// - Peek/Read/Write: borrowed from the caller; never closed.
struct PipeIoContext
{
    PipeOp op;
    HANDLE handle = INVALID_HANDLE_VALUE;
    // read / write
    char *buffer = nullptr;
    int32_t size = 0;
    bool all = false;
    // connect / accept: the pipe name to open or create a listening instance for
    wstring pipeName;
    // connect only
    wstring connectName;
    // results
    bool success = false;
    DWORD errorCode = ERROR_SUCCESS;
    int32_t result = 0;
    std::atomic<bool> cancelled{false};
    // completion: delivered to the event loop thread that started this operation
    shared_ptr<Event> done;
    shared_ptr<EventLoopCoroutine> loop;
};

namespace {

DWORD pipeErrorCode()
{
    return GetLastError();
}

bool performAccept(PipeIoContext *ctx)
{
    if (!ConnectNamedPipe(ctx->handle, nullptr)) {
        DWORD err = pipeErrorCode();
        if (err != ERROR_PIPE_CONNECTED) {
            ctx->errorCode = err;
            return false;
        }
    }
    return true;
}

bool performConnect(PipeIoContext *ctx)
{
    for (int attempt = 0; attempt < 200 && !ctx->cancelled; ++attempt) {
        if (!WaitNamedPipeW(ctx->connectName.c_str(), 100)) {
            DWORD err = pipeErrorCode();
            if (err == ERROR_FILE_NOT_FOUND) {
                // no server for this name; nothing to retry
                ctx->errorCode = err;
                return false;
            }
            // the server exists but has no instance ready yet; fall through and
            // let CreateFileW tell us whether to retry
        }
        HANDLE h = CreateFileW(ctx->connectName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ctx->handle = h;
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            return true;
        }
        DWORD err = pipeErrorCode();
        if (err != ERROR_PIPE_BUSY) {
            ctx->errorCode = err;
            return false;
        }
    }
    ctx->errorCode = ERROR_PIPE_BUSY;
    return false;
}

bool performPeek(PipeIoContext *ctx)
{
    DWORD bytesRead = 0;
    DWORD bytesLeft = 0;
    if (!PeekNamedPipe(ctx->handle, ctx->buffer, static_cast<DWORD>(ctx->size), &bytesRead, nullptr, &bytesLeft)) {
        DWORD err = pipeErrorCode();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_INVALID_HANDLE) {
            ctx->errorCode = err;
            return false;
        }
        ctx->result = 0;
        return true;
    }
    ctx->result = static_cast<int32_t>(bytesRead);
    return true;
}

bool performRead(PipeIoContext *ctx)
{
    int32_t total = 0;
    while (total < ctx->size && !ctx->cancelled) {
        DWORD bytesRead = 0;
        if (!ReadFile(ctx->handle, ctx->buffer + total, static_cast<DWORD>(ctx->size - total), &bytesRead, nullptr)) {
            DWORD err = pipeErrorCode();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA
                || err == ERROR_INVALID_HANDLE) {
                ctx->errorCode = err;
                return false;
            }
            ctx->errorCode = err;
            return false;
        }
        total += static_cast<int32_t>(bytesRead);
        if (!ctx->all) {
            break;
        }
    }
    ctx->result = total;
    return true;
}

bool performWrite(PipeIoContext *ctx)
{
    int32_t total = 0;
    while (total < ctx->size && !ctx->cancelled) {
        DWORD bytesWritten = 0;
        if (!WriteFile(ctx->handle, ctx->buffer + total, static_cast<DWORD>(ctx->size - total), &bytesWritten,
                       nullptr)) {
            DWORD err = pipeErrorCode();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA
                || err == ERROR_INVALID_HANDLE) {
                ctx->errorCode = err;
                return false;
            }
            if (err == ERROR_PIPE_BUSY && !ctx->cancelled) {
                Sleep(10);
                continue;  // transient; retry same byte
            }
            ctx->errorCode = err;
            return false;
        }
        total += static_cast<int32_t>(bytesWritten);
        if (!ctx->all) {
            break;
        }
    }
    ctx->result = total;
    return true;
}

}  // namespace

// Each LocalSocketPrivate owns one LocalPipeData: a single I/O thread plus a
// queue of blocking operations. The thread never touches a handle it does not
// own; the private-side handle is only ever used under m, so quit() cannot race
// an in-flight ReadFile/WriteFile.
struct LocalPipeData
{
    explicit LocalPipeData(HANDLE h = INVALID_HANDLE_VALUE)
        : handle(h)
    {
        ioThread = thread(&LocalPipeData::ioLoop, this);
    }

    ~LocalPipeData()
    {
        shutdown();
    }

    void shutdown()
    {
        {
            lock_guard<mutex> lock(m);
            if (quit) {
                if (ioThread.joinable()) {
                    ioThread.join();
                }
                return;
            }
            quit = true;
            if (handle != INVALID_HANDLE_VALUE) {
                // Cancel the blocking calls the I/O thread may be stuck in.
                // CancelIo() only cancels calls made by the *calling* thread,
                // which here is the event-loop thread, so it would not interrupt
                // the I/O thread. Vista+ has CancelIoEx, which cancels every
                // thread's I/O on the handle. On XP the I/O thread's blocking
                // calls cannot be interrupted; the pipe peer closing is what
                // normally lets them return (a stuck peer can block close()).
#if _WIN32_WINNT >= 0x0600
                CancelIoEx(handle, nullptr);
#else
                CancelIo(handle);
#endif
            }
            for (const shared_ptr<PipeIoContext> &ctx : active) {
                ctx->cancelled = true;
                wake(ctx);
            }
            for (const shared_ptr<PipeIoContext> &ctx : pending) {
                ctx->cancelled = true;
                wake(ctx);
            }
            pending.clear();
        }
        cv.notify_all();
        if (ioThread.joinable()) {
            ioThread.join();
        }
        // The I/O thread is gone, so nobody can be using the handle. Closing it
        // here (instead of in ioLoop's exit branch) also serialises with the
        // accept()/bind() paths that take m, so a handle is never closed twice.
        lock_guard<mutex> lock(m);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }

    void submit(const shared_ptr<PipeIoContext> &ctx)
    {
        {
            lock_guard<mutex> lock(m);
            if (!ioThread.joinable()) {
                quit = false;
                ioThread = thread(&LocalPipeData::ioLoop, this);
            }
            pending.push_back(ctx);
        }
        cv.notify_one();
    }

    HANDLE currentHandle()
    {
        lock_guard<mutex> lock(m);
        return handle;
    }

    void ioLoop()
    {
        while (true) {
            shared_ptr<PipeIoContext> ctx;
            {
                unique_lock<mutex> lock(m);
                cv.wait(lock, [this] { return quit || !pending.empty(); });
                if (quit) {
                    return;
                }
                ctx = pending.front();
                pending.pop_front();
                active.insert(ctx);
            }
            bool ok = false;
            switch (ctx->op) {
            case PipeOp::Accept:
                ok = performAccept(ctx.get());
                break;
            case PipeOp::Connect:
                ok = performConnect(ctx.get());
                break;
            case PipeOp::Peek:
                ok = performPeek(ctx.get());
                break;
            case PipeOp::Read:
                ok = performRead(ctx.get());
                break;
            case PipeOp::Write:
                ok = performWrite(ctx.get());
                break;
            }
            if (ctx->cancelled) {
                ok = false;
            }
            ctx->success = ok;
            {
                lock_guard<mutex> lock(m);
                active.erase(ctx);
            }
            if (ctx->op == PipeOp::Connect && ctx->cancelled && ctx->handle != INVALID_HANDLE_VALUE) {
                CloseHandle(ctx->handle);
                ctx->handle = INVALID_HANDLE_VALUE;
            }
            wake(ctx);
        }
    }

    static void wake(const shared_ptr<PipeIoContext> &ctx)
    {
        shared_ptr<Event> done = ctx->done;
        shared_ptr<EventLoopCoroutine> loop = ctx->loop;
        if (loop) {
            loop->callLaterThreadSafe(0, new LambdaFunctor([done]() { done->set(); }));
        }
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    wstring pipeName;
    thread ioThread;
    mutex m;
    condition_variable cv;
    deque<shared_ptr<PipeIoContext>> pending;
    set<shared_ptr<PipeIoContext>> active;
    bool quit = false;
};

bool LocalSocketPrivate::createLocalSocket()
{
    if (type == LocalSocket::DatagramSocket) {
        setError(Socket::UnsupportedSocketOperationError, "Windows named pipe does not support datagram sockets");
        return false;
    }
    pipeData = make_shared<LocalPipeData>();
    fd = reinterpret_cast<intptr_t>(INVALID_HANDLE_VALUE);
    return true;
}

bool LocalSocketPrivate::setNonblocking()
{
    return true;
}

bool LocalSocketPrivate::bind(const string &name)
{
    if (state != LocalSocket::UnconnectedState) {
        return false;
    }
    wstring pipeName = fullPipeName(name);
    HANDLE h = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0,
                                nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        switch (err) {
        case ERROR_ACCESS_DENIED:
            setError(Socket::SocketAccessError, "Permission denied");
            break;
        case ERROR_PIPE_BUSY:
        case ERROR_ALREADY_EXISTS:
            setError(Socket::AddressInUseError, "The bound address is already in use");
            break;
        default:
            setError(Socket::UnknownSocketError, "Unknown error");
            break;
        }
        return false;
    }
    {
        lock_guard<mutex> lock(pipeData->m);
        pipeData->handle = h;
    }
    pipeData->pipeName = pipeName;
    fd = reinterpret_cast<intptr_t>(h);
    localName = name;
    state = LocalSocket::BoundState;
    return true;
}

bool LocalSocketPrivate::connect(const string &name)
{
    if (state != LocalSocket::UnconnectedState && state != LocalSocket::BoundState) {
        return false;
    }
    shared_ptr<PipeIoContext> ctx = make_shared<PipeIoContext>();
    ctx->op = PipeOp::Connect;
    ctx->connectName = fullPipeName(name);
    ctx->done = make_shared<Event>();
    ctx->loop = currentLoop()->getOrCreate();
    state = LocalSocket::ConnectingState;
    pipeData->submit(ctx);
    ctx->done->tryWait();
    if (ctx->cancelled) {
        state = LocalSocket::UnconnectedState;
        return false;
    }
    if (!ctx->success || ctx->handle == INVALID_HANDLE_VALUE) {
        if (ctx->errorCode == ERROR_FILE_NOT_FOUND || ctx->errorCode == ERROR_SEM_TIMEOUT) {
            setError(Socket::ConnectionRefusedError, "Connection refused");
        } else if (ctx->errorCode == ERROR_ACCESS_DENIED) {
            setError(Socket::SocketAccessError, "Permission denied");
        } else {
            setError(Socket::UnknownSocketError, "Unknown error");
        }
        state = LocalSocket::UnconnectedState;
        return false;
    }
    // The I/O thread handed us a connected handle and has stopped touching it.
    {
        lock_guard<mutex> lock(pipeData->m);
        pipeData->handle = ctx->handle;
    }
    fd = reinterpret_cast<intptr_t>(ctx->handle);
    localName = name;
    state = LocalSocket::ConnectedState;
    return true;
}

void LocalSocketPrivate::close()
{
    if (pipeData) {
        pipeData->shutdown();
    }
    fd = reinterpret_cast<intptr_t>(INVALID_HANDLE_VALUE);
    state = LocalSocket::UnconnectedState;
    localName.clear();
    peerName.clear();
}

void LocalSocketPrivate::abort()
{
    if (pipeData) {
        pipeData->shutdown();
    }
    fd = reinterpret_cast<intptr_t>(INVALID_HANDLE_VALUE);
    state = LocalSocket::UnconnectedState;
    localName.clear();
    peerName.clear();
}

bool LocalSocketPrivate::listen(int backlog)
{
    if (state != LocalSocket::BoundState && state != LocalSocket::UnconnectedState) {
        return false;
    }
    if (type != LocalSocket::StreamSocket) {
        setError(Socket::UnsupportedSocketOperationError, "Datagram socket cannot listen");
        return false;
    }
    state = LocalSocket::ListeningState;
    return true;
}

LocalSocket *LocalSocketPrivate::accept()
{
    if (state != LocalSocket::ListeningState || type != LocalSocket::StreamSocket) {
        return nullptr;
    }
    // Reuse the bind() instance for the first connection; afterwards create a
    // fresh listening instance per connection, like QLocalServer does.
    HANDLE listeningHandle;
    {
        lock_guard<mutex> lock(pipeData->m);
        if (pipeData->handle == INVALID_HANDLE_VALUE) {
            pipeData->handle = CreateNamedPipeW(pipeData->pipeName.c_str(), PIPE_ACCESS_DUPLEX,
                                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
        }
        listeningHandle = pipeData->handle;
    }
    if (listeningHandle == INVALID_HANDLE_VALUE) {
        setError(Socket::SocketResourceError, "Out of resources");
        return nullptr;
    }
    shared_ptr<PipeIoContext> ctx = make_shared<PipeIoContext>();
    ctx->op = PipeOp::Accept;
    ctx->pipeName = pipeData->pipeName;
    ctx->handle = listeningHandle;
    ctx->done = make_shared<Event>();
    ctx->loop = currentLoop()->getOrCreate();
    pipeData->submit(ctx);
    ctx->done->tryWait();
    if (ctx->cancelled || !ctx->success) {
        bool closed = false;
        {
            lock_guard<mutex> lock(pipeData->m);
            if (pipeData->handle == listeningHandle) {
                pipeData->handle = INVALID_HANDLE_VALUE;
                closed = true;
            }
        }
        // If a concurrent close() shut the server down, shutdown() already
        // closed the instance; only close it when we took ownership of it here.
        if (closed) {
            CloseHandle(listeningHandle);
        }
        if (ctx->cancelled) {
            return nullptr;
        }
        setError(Socket::UnknownSocketError, "Unknown error");
        return nullptr;
    }
    {
        lock_guard<mutex> lock(pipeData->m);
        pipeData->handle = INVALID_HANDLE_VALUE;  // the connected instance now belongs to the peer
    }
    LocalSocket *conn = new LocalSocket(reinterpret_cast<intptr_t>(listeningHandle));
    LocalSocketPrivate *pd = conn->d_func();
    // the fd constructor already wrapped listeningHandle in a fresh LocalPipeData
    // and set state to ConnectedState; just hand it the server-side bookkeeping.
    pd->localName = localName;
    pd->pipeData->pipeName = pipeData->pipeName;
    return conn;
}

int32_t LocalSocketPrivate::peek(char *data, int32_t size)
{
    HANDLE h = pipeData->currentHandle();
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    shared_ptr<PipeIoContext> ctx = make_shared<PipeIoContext>();
    ctx->op = PipeOp::Peek;
    ctx->handle = h;
    ctx->buffer = data;
    ctx->size = size;
    ctx->done = make_shared<Event>();
    ctx->loop = currentLoop()->getOrCreate();
    pipeData->submit(ctx);
    ctx->done->tryWait();
    if (ctx->cancelled) {
        return -1;
    }
    if (!ctx->success) {
        return -1;
    }
    return ctx->result;
}

int32_t LocalSocketPrivate::recv(char *data, int32_t size, bool all)
{
    HANDLE h = pipeData->currentHandle();
    if (h == INVALID_HANDLE_VALUE || size <= 0) {
        return -1;
    }
    shared_ptr<PipeIoContext> ctx = make_shared<PipeIoContext>();
    ctx->op = PipeOp::Read;
    ctx->handle = h;
    ctx->buffer = data;
    ctx->size = size;
    ctx->all = all;
    ctx->done = make_shared<Event>();
    ctx->loop = currentLoop()->getOrCreate();
    pipeData->submit(ctx);
    ctx->done->tryWait();
    if (ctx->cancelled) {
        return -1;
    }
    if (!ctx->success) {
        setError(Socket::RemoteHostClosedError, "The remote host closed the connection");
        return ctx->result > 0 ? ctx->result : -1;
    }
    return ctx->result;
}

int32_t LocalSocketPrivate::send(const char *data, int32_t size, bool all)
{
    HANDLE h = pipeData->currentHandle();
    if (h == INVALID_HANDLE_VALUE || size <= 0) {
        return -1;
    }
    shared_ptr<PipeIoContext> ctx = make_shared<PipeIoContext>();
    ctx->op = PipeOp::Write;
    ctx->handle = h;
    ctx->buffer = const_cast<char *>(data);
    ctx->size = size;
    ctx->all = all;
    ctx->done = make_shared<Event>();
    ctx->loop = currentLoop()->getOrCreate();
    pipeData->submit(ctx);
    ctx->done->tryWait();
    if (ctx->cancelled) {
        return -1;
    }
    if (!ctx->success) {
        setError(Socket::RemoteHostClosedError, "The remote host closed the connection");
        return ctx->result > 0 ? ctx->result : -1;
    }
    return ctx->result;
}

int32_t LocalSocketPrivate::recvfrom(char *data, int32_t size, string *addr)
{
    setError(Socket::UnsupportedSocketOperationError, "Windows named pipe does not support datagram sockets");
    return -1;
}

int32_t LocalSocketPrivate::sendto(const char *data, int32_t size, const string &addr)
{
    setError(Socket::UnsupportedSocketOperationError, "Windows named pipe does not support datagram sockets");
    return -1;
}

bool LocalSocketPrivate::fetchConnectionParameters()
{
    if (!pipeData) {
        pipeData = make_shared<LocalPipeData>(reinterpret_cast<HANDLE>(fd));
    }
    return true;
}

}  // namespace qtng
