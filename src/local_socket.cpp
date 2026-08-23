#include <cstdint>
#include <string>

#include "qtng/private/local_socket_p.h"
#include "qtng/local_socket.h"
#include "qtng/socket.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.local_socket");

namespace qtng {

LocalSocketPrivate::LocalSocketPrivate(LocalSocket::LocalSocketType type, LocalSocket *parent)
    : q_ptr(parent)
    , type(type)
    , error(Socket::NoError)
    , state(LocalSocket::UnconnectedState)
    , bound(false)
    , fd(-1)
{
    createLocalSocket();
}

LocalSocketPrivate::LocalSocketPrivate(intptr_t fd, LocalSocket *parent)
    : q_ptr(parent)
    , error(Socket::NoError)
    , state(LocalSocket::ConnectedState)
    , type(LocalSocket::StreamSocket)
    , bound(false)
{
    this->fd = fd;
    fetchConnectionParameters();
}

LocalSocketPrivate::~LocalSocketPrivate()
{
    abort();
}

void LocalSocketPrivate::setError(Socket::SocketError error, const string &errorString)
{
    this->error = error;
    this->errorString = errorString;
}

LocalSocket::LocalSocket(LocalSocketType type)
    : d_ptr(new LocalSocketPrivate(type, this))
{
}

LocalSocket::LocalSocket(intptr_t socketDescriptor)
    : d_ptr(new LocalSocketPrivate(socketDescriptor, this))
{
}

LocalSocket::~LocalSocket()
{
    NG_D(LocalSocket);
    d->abort();
    if (d->readLock.isLocked() || d->writeLock.isLocked()) {
        ngWarning() << "local socket is deleted while receiving or sending.";
    }
    delete d_ptr;
}

Socket::SocketError LocalSocket::error() const
{
    NG_D(const LocalSocket);
    return d->error;
}

string LocalSocket::errorString() const
{
    NG_D(const LocalSocket);
    return d->errorString;
}

bool LocalSocket::isValid() const
{
    NG_D(const LocalSocket);
    return d->state != LocalSocket::UnconnectedState;
}

LocalSocket::LocalSocketType LocalSocket::type() const
{
    NG_D(const LocalSocket);
    return d->type;
}

LocalSocket::LocalSocketState LocalSocket::state() const
{
    NG_D(const LocalSocket);
    return d->state;
}

intptr_t LocalSocket::fileno() const
{
    NG_D(const LocalSocket);
    return static_cast<intptr_t>(d->fd);
}

string LocalSocket::localName() const
{
    NG_D(const LocalSocket);
    return d->localName;
}

string LocalSocket::peerName() const
{
    NG_D(const LocalSocket);
    return d->peerName;
}

LocalSocket *LocalSocket::accept()
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return nullptr;
    }
    return d->accept();
}

bool LocalSocket::bind(const string &name)
{
    NG_D(LocalSocket);
    return d->bind(name);
}

bool LocalSocket::connect(const string &name)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return false;
    }
    return d->connect(name);
}

void LocalSocket::close()
{
    NG_D(LocalSocket);
    d->close();
    if (d->readLock.isLocked()) {
        d->readLock.tryAcquire();
        d->readLock.release();
    }
    if (d->writeLock.isLocked()) {
        d->writeLock.tryAcquire();
        d->writeLock.release();
    }
}

void LocalSocket::abort()
{
    NG_D(LocalSocket);
    d->abort();
    if (d->readLock.isLocked()) {
        d->readLock.release();
    }
    if (d->writeLock.isLocked()) {
        d->writeLock.release();
    }
}

bool LocalSocket::listen(int backlog)
{
    NG_D(LocalSocket);
    return d->listen(backlog);
}

int32_t LocalSocket::peek(char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->peek(data, size);
}

int32_t LocalSocket::recv(char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recv(data, size, false);
}

int32_t LocalSocket::recvall(char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recv(data, size, true);
}

int32_t LocalSocket::send(const char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    int32_t bytesSent = d->send(data, size, false);
    if (bytesSent == 0 && d->error != Socket::NoError) {
        return -1;
    }
    return bytesSent;
}

int32_t LocalSocket::sendall(const char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->send(data, size, true);
}

int32_t LocalSocket::recvfrom(char *data, int32_t size, string *addr)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recvfrom(data, size, addr);
}

int32_t LocalSocket::sendto(const char *data, int32_t size, const string &addr)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->sendto(data, size, addr);
}

string LocalSocket::recv(int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess() || size <= 0) {
        return string();
    }
    string bs(static_cast<size_t>(size), '\0');
    int32_t bytes = d->recv(&bs[0], static_cast<int32_t>(bs.size()), false);
    if (bytes > 0) {
        bs.resize(static_cast<size_t>(bytes));
        return bs;
    }
    return string();
}

string LocalSocket::recvall(int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess() || size <= 0) {
        return string();
    }
    string bs(static_cast<size_t>(size), '\0');
    int32_t bytes = d->recv(&bs[0], static_cast<int32_t>(bs.size()), true);
    if (bytes > 0) {
        bs.resize(static_cast<size_t>(bytes));
        return bs;
    }
    return string();
}

int32_t LocalSocket::send(const string &data)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    int32_t bytesSent = d->send(data.data(), static_cast<int32_t>(data.size()), false);
    if (bytesSent == 0 && d->error != Socket::NoError) {
        return -1;
    }
    return bytesSent;
}

int32_t LocalSocket::sendall(const string &data)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->send(data.data(), static_cast<int32_t>(data.size()), true);
}

string LocalSocket::recvfrom(int32_t size, string *addr)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess() || size <= 0) {
        return string();
    }
    string bs(static_cast<size_t>(size), '\0');
    int32_t bytes = d->recvfrom(&bs[0], size, addr);
    if (bytes > 0) {
        bs.resize(static_cast<size_t>(bytes));
        return bs;
    }
    return string();
}

int32_t LocalSocket::sendto(const string &data, const string &addr)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->sendto(data.data(), static_cast<int32_t>(data.size()), addr);
}

}  // namespace qtng
