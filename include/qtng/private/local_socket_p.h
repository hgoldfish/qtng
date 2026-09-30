#ifndef QTNG_LOCAL_SOCKET_P_H
#define QTNG_LOCAL_SOCKET_P_H

#include <cstdint>
#include <string>

#include "qtng/local_socket.h"
#include "qtng/locks.h"
#include "qtng/private/socket_p.h"
#include "qtng/utils/platform.h"

namespace qtng {

class LocalSocketPrivate
{
public:
    // Pinned to SocketPrivate's codes. A second enum with the same names but its
    // own numbering is a silent type pun: setError(Socket::ReadError,
    // ReadErrorString) would store whatever code this enum gave that name, and
    // nothing catches it. An alias cannot replace this - before C++20 a type
    // alias does not bring enumerator names into scope, and this tree is C++11.
    enum ErrorString {
        NonBlockingInitFailedErrorString = SocketPrivate::NonBlockingInitFailedErrorString,
        RemoteHostClosedErrorString = SocketPrivate::RemoteHostClosedErrorString,
        TimeOutErrorString = SocketPrivate::TimeOutErrorString,
        ResourceErrorString = SocketPrivate::ResourceErrorString,
        OperationUnsupportedErrorString = SocketPrivate::OperationUnsupportedErrorString,
        InvalidSocketErrorString = SocketPrivate::InvalidSocketErrorString,
        AccessErrorString = SocketPrivate::AccessErrorString,
        ConnectionRefusedErrorString = SocketPrivate::ConnectionRefusedErrorString,
        AddressInuseErrorString = SocketPrivate::AddressInuseErrorString,
        AddressNotAvailableErrorString = SocketPrivate::AddressNotAvailableErrorString,
        WriteErrorString = SocketPrivate::WriteErrorString,
        ReadErrorString = SocketPrivate::ReadErrorString,
        OutOfMemoryErrorString = SocketPrivate::OutOfMemoryErrorString,
        UnknownSocketErrorString = SocketPrivate::UnknownSocketErrorString
    };
public:
    LocalSocketPrivate(LocalSocket *parent);
    explicit LocalSocketPrivate(std::intptr_t socketDescriptor, LocalSocket *parent);
    virtual ~LocalSocketPrivate();
public:
    void setError(Socket::SocketError error, const std::string &errorString);
    void setError(Socket::SocketError error, ErrorString errorString);
    std::string getErrorString() const;
    bool isValid() const;
    bool checkState() const;

    LocalSocket *accept();
    bool bind(const std::string &name, Socket::BindMode mode);
    bool connect(const std::string &name);
    // close(): wake waiters, drain readLock/writeLock, then release the handle.
    // abort(): wake and release immediately without draining — safe to call
    // while the current coroutine already holds a lock (e.g. from recv/send).
    // Not interchangeable: destroying/aborting a listening socket while another
    // coroutine is in accept() is undefined; stop the server with close().
    void close();
    void abort();
    bool listen(int backlog);
    bool setOption(Socket::SocketOption option, int value);
    int option(Socket::SocketOption option) const;

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size, bool all);
    std::int32_t send(const char *data, std::int32_t size, bool all);

    static std::string makeFullServerName(const std::string &name);
    std::string localAddressURI() const;
    std::string peerAddressURI() const;
protected:
    bool createSocket();
    void setSocketDescriptor(std::intptr_t socketDescriptor);
#ifdef NG_OS_WIN
    bool createPipeInstance(bool firstInstance);
    // Queue a ConnectNamedPipe on the current listening instance.
    //  1: queued (or a client was already waiting),  -1: hard error (error set).
    int startAcceptWait();
    void discardAcceptWait(bool cancelIo);
    void releaseAcceptWait();
    // Drop a listening instance that can serve nobody anymore: cancel the queued
    // wait, close the handle and leave the listening state.
    void dropListeningInstance();
#endif
public:
    LocalSocket *q_ptr;
    Socket::SocketError error;
    std::string errorString;
    Socket::SocketState state;
    std::string serverName;
    std::string fullServerName;
#ifdef NG_OS_WIN
    std::intptr_t fd;  // HANDLE
    // Pending ConnectNamedPipe state, owned by local_socket_win.cpp (Windows
    // types are kept out of this header on purpose). It exists while a pipe
    // instance has ever been armed; `acceptArmed` is what tells whether a
    // ConnectNamedPipe is queued right now. Together they keep the pipe name
    // attended between two accept() calls.
    void *acceptWait;
    bool acceptArmed;
    // Bumped every time the queued wait is replaced or taken away. accept()
    // captures it before sleeping and compares afterwards, so a wake-up caused
    // by a discard can never be mistaken for a client - not even when the socket
    // has been closed and bound again meanwhile.
    std::uint32_t acceptGeneration;
#else
    int fd;
    // Path of the socket file this object bound. connect() replaces
    // fullServerName with the peer, so close() must not unlink that.
    std::string unlinkPath;
    bool shouldUnlink;
#endif
    Lock readLock;
    Lock writeLock;

    NG_DECLARE_PUBLIC(LocalSocket)
};

}  // namespace qtng

#endif  // QTNG_LOCAL_SOCKET_P_H
