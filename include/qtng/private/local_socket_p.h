#ifndef QTNG_LOCAL_SOCKET_P_H
#define QTNG_LOCAL_SOCKET_P_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/local_socket.h"
#include "qtng/locks.h"
#include "qtng/socket.h"
#include "qtng/private/eventloop_p.h"
#include "qtng/utils/platform.h"

namespace qtng {

class LocalSocketPrivate
{
public:
    LocalSocketPrivate(LocalSocket::LocalSocketType type, LocalSocket *parent);
    LocalSocketPrivate(std::intptr_t fd, LocalSocket *parent);
    virtual ~LocalSocketPrivate();
public:
    LocalSocket *accept();
    bool bind(const std::string &name);
    bool connect(const std::string &name);
    void close();
    void abort();
    bool listen(int backlog);
    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size, bool all);
    std::int32_t send(const char *data, std::int32_t size, bool all);
    std::int32_t recvfrom(char *data, std::int32_t size, std::string *addr);
    std::int32_t sendto(const char *data, std::int32_t size, const std::string &addr);
public:
    void setError(Socket::SocketError error, const std::string &errorString);
    bool createLocalSocket();
    bool setNonblocking();
    bool fetchConnectionParameters();
public:
    LocalSocket *q_ptr;
public:
    LocalSocket::LocalSocketType type;
    Socket::SocketError error;
    std::string errorString;
    LocalSocket::LocalSocketState state;
    std::string localName;
    std::string peerName;
#ifdef NG_OS_WIN
    std::intptr_t fd;
    std::shared_ptr<struct LocalPipeData> pipeData = nullptr;
#else
    int fd;
#endif
    bool bound;
    Lock readLock;
    Lock writeLock;

    NG_DECLARE_PUBLIC(LocalSocket)
};

}  // namespace qtng

#endif  // QTNG_LOCAL_SOCKET_P_H
