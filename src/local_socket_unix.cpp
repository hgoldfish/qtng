#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "qtng/private/local_socket_p.h"
#include "qtng/local_socket.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/platform.h"

using namespace std;

NG_LOGGER("qtng.local_socket_unix");

namespace qtng {

static void local_ignore_sigpipe()
{
    static bool done = false;
    if (done) {
        return;
    }
    struct sigaction noaction;
    memset(&noaction, 0, sizeof(noaction));
    noaction.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &noaction, nullptr);
    done = true;
}

static bool setLocalAddress(const string &name, sockaddr_un *aa, int *sockAddrSize)
{
    memset(aa, 0, sizeof(sockaddr_un));
    aa->sun_family = AF_UNIX;
    if (name.size() >= sizeof(aa->sun_path)) {
        return false;
    }
    memcpy(aa->sun_path, name.c_str(), name.size() + 1);
    *sockAddrSize = sizeof(sockaddr_un);
    return true;
}

static string localNameFromSockaddr(const sockaddr_un &aa)
{
    if (aa.sun_family != AF_UNIX) {
        return string();
    }
    return string(aa.sun_path, strnlen(aa.sun_path, sizeof(aa.sun_path)));
}

bool LocalSocketPrivate::createLocalSocket()
{
    local_ignore_sigpipe();
    int flags = 0;
#ifdef SOCK_CLOEXEC
    flags |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
    flags |= SOCK_NONBLOCK;
#endif
    if (type == LocalSocket::StreamSocket) {
        flags |= SOCK_STREAM;
    } else {
        flags |= SOCK_DGRAM;
    }
    fd = ::socket(AF_UNIX, flags, 0);
    if (fd < 0) {
        setError(Socket::UnsupportedSocketOperationError, strerror(errno));
        return false;
    }
#ifndef SOCK_CLOEXEC
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
#ifndef SOCK_NONBLOCK
    if (!setNonblocking()) {
        ::close(fd);
        fd = -1;
        setError(Socket::SocketResourceError, "Unable to initialize non-blocking socket");
        return false;
    }
#endif
    state = LocalSocket::UnconnectedState;
    return true;
}

bool LocalSocketPrivate::setNonblocking()
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool LocalSocketPrivate::bind(const string &name)
{
    if (state != LocalSocket::UnconnectedState) {
        return false;
    }
    sockaddr_un aa;
    int sockAddrSize;
    if (!setLocalAddress(name, &aa, &sockAddrSize)) {
        setError(Socket::SocketAddressNotAvailableError, "The address is too long");
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr *>(&aa), static_cast<socklen_t>(sockAddrSize)) < 0) {
        switch (errno) {
        case EADDRINUSE:
            setError(Socket::AddressInUseError, "The bound address is already in use");
            break;
        case EACCES:
            setError(Socket::SocketAccessError, "Permission denied");
            break;
        case EADDRNOTAVAIL:
            setError(Socket::SocketAddressNotAvailableError, "The address is not available");
            break;
        default:
            setError(Socket::UnknownSocketError, strerror(errno));
            break;
        }
        return false;
    }
    state = LocalSocket::BoundState;
    bound = true;
    localName = name;
    peerName.clear();
    return true;
}

bool LocalSocketPrivate::connect(const string &name)
{
    if (state != LocalSocket::UnconnectedState && state != LocalSocket::BoundState) {
        return false;
    }
    sockaddr_un aa;
    int sockAddrSize;
    if (!setLocalAddress(name, &aa, &sockAddrSize)) {
        setError(Socket::SocketAddressNotAvailableError, "The address is too long");
        return false;
    }
    state = LocalSocket::ConnectingState;
    ScopedIoWatcher watcher(EventLoopCoroutine::Write, fd);
    while (true) {
        if (fd < 0 || state != LocalSocket::ConnectingState) {
            return false;
        }
        int result = ::connect(fd, reinterpret_cast<sockaddr *>(&aa), static_cast<socklen_t>(sockAddrSize));
        if (result >= 0) {
            state = LocalSocket::ConnectedState;
            fetchConnectionParameters();
            return true;
        }
        int err = errno;
        switch (err) {
        case EISCONN:
            state = LocalSocket::ConnectedState;
            fetchConnectionParameters();
            return true;
        case EINPROGRESS:
        case EALREADY:
        case EAGAIN:
            break;
        case ECONNREFUSED:
        case ENOENT:
        case EINVAL:
            setError(Socket::ConnectionRefusedError, "Connection refused");
            state = LocalSocket::UnconnectedState;
            return false;
        case EACCES:
        case EPERM:
            setError(Socket::SocketAccessError, "Permission denied");
            state = LocalSocket::UnconnectedState;
            return false;
        case ETIMEDOUT:
            setError(Socket::NetworkError, "Connection timed out");
            state = LocalSocket::UnconnectedState;
            return false;
        case EBADF:
        case EFAULT:
        case ENOTSOCK:
            setError(Socket::UnsupportedSocketOperationError, "Invalid socket descriptor");
            state = LocalSocket::UnconnectedState;
            return false;
        default:
            setError(Socket::UnknownSocketError, strerror(err));
            state = LocalSocket::UnconnectedState;
            return false;
        }
        if (!watcher.start()) {
            setError(Socket::UnknownSocketError, "Unknown error");
            state = LocalSocket::UnconnectedState;
            return false;
        }
    }
}

void LocalSocketPrivate::close()
{
    if (fd > 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        EventLoopCoroutine::get()->triggerIoWatchers(fd);
        fd = -1;
    }
    if (bound && !localName.empty()) {
        // best effort remove the socket file we created with bind()
        ::unlink(localName.c_str());
    }
    state = LocalSocket::UnconnectedState;
    localName.clear();
    peerName.clear();
}

void LocalSocketPrivate::abort()
{
    if (fd > 0) {
        ::close(fd);
        EventLoopCoroutine::get()->triggerIoWatchers(fd);
        fd = -1;
    }
    if (bound && !localName.empty()) {
        ::unlink(localName.c_str());
    }
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
    if (::listen(fd, backlog) < 0) {
        setError(Socket::UnknownSocketError, strerror(errno));
        return false;
    }
    state = LocalSocket::ListeningState;
    return true;
}

static inline int local_safe_accept(int s, sockaddr *addr, socklen_t *addrlen)
{
    int fd;
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK) && !defined(NG_OS_MACOS)
    fd = ::accept4(s, addr, addrlen, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    fd = ::accept(s, addr, addrlen);
    if (fd < 0) {
        return -1;
    }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
#endif
    return fd;
}

LocalSocket *LocalSocketPrivate::accept()
{
    if (state != LocalSocket::ListeningState || type != LocalSocket::StreamSocket) {
        return nullptr;
    }
    ScopedIoWatcher watcher(EventLoopCoroutine::Read, fd);
    while (true) {
        if (fd < 0 || state != LocalSocket::ListeningState) {
            return nullptr;
        }
        int acceptedDescriptor = local_safe_accept(fd, nullptr, nullptr);
        if (acceptedDescriptor == -1) {
            int err = errno;
            switch (err) {
#if EWOULDBLOCK - 0 && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EAGAIN:
                break;
            case ECONNABORTED:
                setError(Socket::NetworkError, "The remote host closed the connection");
                return nullptr;
            case EBADF:
            case ENOTSOCK:
            case EINVAL:
                setError(Socket::UnsupportedSocketOperationError, "Invalid socket descriptor");
                return nullptr;
            case ENFILE:
            case EMFILE:
            case ENOBUFS:
            case ENOMEM:
                setError(Socket::SocketResourceError, "Out of resources");
                return nullptr;
            default:
                setError(Socket::UnknownSocketError, strerror(err));
                return nullptr;
            }
            if (!watcher.start()) {
                setError(Socket::UnknownSocketError, "Unknown error");
                return nullptr;
            }
        } else {
            LocalSocket *conn = new LocalSocket(acceptedDescriptor);
            return conn;
        }
    }
}

int32_t LocalSocketPrivate::peek(char *data, int32_t size)
{
    if (fd <= 0) {
        return -1;
    }
    ssize_t r = ::recv(fd, data, static_cast<size_t>(size), MSG_PEEK);
    if (r < 0) {
        int err = errno;
        if (err == EINPROGRESS || err == EAGAIN || err == EWOULDBLOCK) {
            return 0;
        }
        if (err == ECONNRESET || err == ECONNABORTED || err == ENETDOWN || err == ENETRESET || err == ESHUTDOWN
            || err == ETIMEDOUT || err == ENOTCONN) {
            return -1;
        }
        return -1;
    }
    if (r == 0) {
        return -1;
    }
    return static_cast<int32_t>(r);
}

int32_t LocalSocketPrivate::recv(char *data, int32_t size, bool all)
{
    if (fd <= 0) {
        return -1;
    }
    ScopedIoWatcher watcher(EventLoopCoroutine::Read, fd);
    int32_t total = 0;
    while (total < size) {
        if (fd <= 0) {
            return total == 0 ? -1 : total;
        }
        ssize_t r = 0;
        do {
            r = ::recv(fd, data + total, static_cast<size_t>(size - total), 0);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            int err = errno;
            switch (err) {
#if EWOULDBLOCK - 0 && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EAGAIN:
                break;
            case ECONNRESET:
                setError(Socket::RemoteHostClosedError, "The remote host closed the connection");
                return total;
            case EBADF:
            case EINVAL:
            case EIO:
            default:
                setError(Socket::NetworkError, "Network error");
                abort();
                return total == 0 ? -1 : total;
            }
        } else if (r == 0) {
            setError(Socket::RemoteHostClosedError, "The remote host closed the connection");
            abort();
            return total;
        } else {
            total += static_cast<int32_t>(r);
            if (!all) {
                return total;
            }
            continue;
        }
        if (!watcher.start()) {
            setError(Socket::NetworkError, "Network error");
            abort();
            return total == 0 ? -1 : total;
        }
    }
    return total;
}

int32_t LocalSocketPrivate::send(const char *data, int32_t size, bool all)
{
    if (fd <= 0 || size <= 0) {
        return -1;
    }
    ScopedIoWatcher watcher(EventLoopCoroutine::Write, fd);
    int32_t sent = 0;
    while (sent < size) {
        if (fd <= 0) {
            return sent;
        }
        ssize_t w;
        do {
            w = ::send(fd, data + sent, static_cast<size_t>(size - sent), MSG_NOSIGNAL);
        } while (w < 0 && errno == EINTR);
        if (w > 0) {
            sent += static_cast<int32_t>(w);
            if (!all) {
                return sent;
            }
            continue;
        } else if (w == 0) {
            setError(Socket::RemoteHostClosedError, "The remote host closed the connection");
            return sent;
        } else {
            int err = errno;
            switch (err) {
#if EWOULDBLOCK - 0 && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EAGAIN:
                if (sent > 0 && !all) {
                    return sent;
                }
                break;
            case EPIPE:
            case ECONNRESET:
            case ENOTCONN:
                setError(Socket::RemoteHostClosedError, "The remote host closed the connection");
                return -1;
            case EACCES:
                setError(Socket::SocketAccessError, "Permission denied");
                return -1;
            case EMSGSIZE:
            case ENOBUFS:
                setError(Socket::DatagramTooLargeError, "Datagram was too large to send");
                return -1;
            case EBADF:
            case EFAULT:
            case EINVAL:
            case ENOTSOCK:
                setError(Socket::UnsupportedSocketOperationError, "Invalid socket descriptor");
                return -1;
            default:
                setError(Socket::UnknownSocketError, strerror(err));
                return -1;
            }
        }
        if (!watcher.start()) {
            setError(Socket::UnknownSocketError, "Unknown error");
            return -1;
        }
    }
    return sent;
}

int32_t LocalSocketPrivate::recvfrom(char *data, int32_t size, string *addr)
{
    if (fd <= 0 || size <= 0) {
        return -1;
    }
    sockaddr_un aa;
    memset(&aa, 0, sizeof(aa));
    socklen_t len = sizeof(aa);
    ScopedIoWatcher watcher(EventLoopCoroutine::Read, fd);
    while (true) {
        if (fd <= 0) {
            return -1;
        }
        ssize_t r;
        do {
            r = ::recvfrom(fd, data, static_cast<size_t>(size), 0, reinterpret_cast<sockaddr *>(&aa), &len);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            int err = errno;
            switch (err) {
#if EWOULDBLOCK - 0 && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EAGAIN:
                break;
            case ECONNRESET:
            case ECONNREFUSED:
            case ENOTCONN:
                setError(Socket::RemoteHostClosedError, "The remote host closed the connection");
                return -1;
            case ENOMEM:
                setError(Socket::SocketResourceError, "Out of resources");
                return -1;
            case EBADF:
            case EINVAL:
            case EFAULT:
            default:
                setError(Socket::NetworkError, "Network error");
                return -1;
            }
        } else {
            if (addr) {
                *addr = localNameFromSockaddr(aa);
            }
            return static_cast<int32_t>(r);
        }
        if (!watcher.start()) {
            setError(Socket::NetworkError, "Network error");
            return -1;
        }
    }
}

int32_t LocalSocketPrivate::sendto(const char *data, int32_t size, const string &addr)
{
    if (fd <= 0 || size <= 0) {
        return -1;
    }
    sockaddr_un aa;
    int sockAddrSize;
    if (!setLocalAddress(addr, &aa, &sockAddrSize)) {
        setError(Socket::SocketAddressNotAvailableError, "The address is too long");
        return -1;
    }
    if (state == LocalSocket::UnconnectedState) {
        // unbound datagram socket needs an anonymous address to receive replies
        bind(string());
        if (state != LocalSocket::BoundState) {
            return -1;
        }
    }
    ScopedIoWatcher watcher(EventLoopCoroutine::Write, fd);
    while (true) {
        if (fd <= 0) {
            return -1;
        }
        ssize_t w;
        do {
            w = ::sendto(fd, data, static_cast<size_t>(size), MSG_NOSIGNAL, reinterpret_cast<sockaddr *>(&aa),
                         static_cast<socklen_t>(sockAddrSize));
        } while (w < 0 && errno == EINTR);
        if (w >= 0) {
            if (localName.empty()) {
                fetchConnectionParameters();
            }
            return static_cast<int32_t>(w);
        }
        int err = errno;
        switch (err) {
#if EWOULDBLOCK - 0 && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EAGAIN:
            break;
        case EACCES:
            setError(Socket::SocketAccessError, "Permission denied");
            return -1;
        case EMSGSIZE:
            setError(Socket::DatagramTooLargeError, "Datagram was too large to send");
            return -1;
        case EPIPE:
        case ECONNRESET:
        case ENOTCONN:
        case ENOTSOCK:
            setError(Socket::RemoteHostClosedError, "The remote host closed the connection");
            return -1;
        default:
            setError(Socket::UnknownSocketError, strerror(err));
            return -1;
        }
        if (!watcher.start()) {
            setError(Socket::UnknownSocketError, "Unknown error");
            return -1;
        }
    }
}

bool LocalSocketPrivate::fetchConnectionParameters()
{
    if (fd <= 0) {
        return false;
    }
    sockaddr_un aa;
    socklen_t len = sizeof(aa);
    memset(&aa, 0, sizeof(aa));
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&aa), &len) == 0) {
        localName = localNameFromSockaddr(aa);
    }
    memset(&aa, 0, sizeof(aa));
    len = sizeof(aa);
    if (::getpeername(fd, reinterpret_cast<sockaddr *>(&aa), &len) == 0) {
        peerName = localNameFromSockaddr(aa);
    }
    int sockType = 0;
    socklen_t sockLen = sizeof(sockType);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &sockType, &sockLen) == 0) {
        type = sockType == SOCK_DGRAM ? LocalSocket::DatagramSocket : LocalSocket::StreamSocket;
    }
    return true;
}

}  // namespace qtng
