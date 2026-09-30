#include <memory>
#include <utility>

#include "bridge/core_access.h"
#include "bridge/local_socket_access.h"
#include "bridge/stream_bridge.h"
#include "local_socket.h"
#include "socket_utils.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class LocalSocketPrivate
{
public:
    LocalSocketPrivate()
        : core(make_shared<qtng_core::LocalSocket>())
    {
    }
    explicit LocalSocketPrivate(shared_ptr<qtng_core::LocalSocket> c)
        : core(std::move(c))
    {
    }
    explicit LocalSocketPrivate(qintptr socketDescriptor)
        : core(make_shared<qtng_core::LocalSocket>(static_cast<intptr_t>(socketDescriptor)))
    {
    }

    shared_ptr<qtng_core::LocalSocket> core;

    static shared_ptr<qtng_core::LocalSocket> &coreOf(LocalSocket *s) { return s->d_func()->core; }
    static const shared_ptr<qtng_core::LocalSocket> &coreOf(const LocalSocket *s) { return s->d_func()->core; }
};

LocalSocket::LocalSocket()
    : d_ptr(new LocalSocketPrivate())
{
}

LocalSocket::LocalSocket(qintptr socketDescriptor)
    : d_ptr(new LocalSocketPrivate(socketDescriptor))
{
}

LocalSocket::LocalSocket(LocalSocketPrivate *d)
    : d_ptr(d)
{
}

LocalSocket::~LocalSocket()
{
    delete d_ptr;
}

Socket::SocketError LocalSocket::error() const
{
    return static_cast<Socket::SocketError>(d_ptr->core->error());
}

QString LocalSocket::errorString() const
{
    return toQString(d_ptr->core->errorString());
}

bool LocalSocket::isValid() const
{
    return d_ptr->core->isValid();
}

HostAddress LocalSocket::localAddress() const
{
    return toQtAddress(d_ptr->core->localAddress());
}

quint16 LocalSocket::localPort() const
{
    return d_ptr->core->localPort();
}

HostAddress LocalSocket::peerAddress() const
{
    return toQtAddress(d_ptr->core->peerAddress());
}

QString LocalSocket::peerName() const
{
    return toQString(d_ptr->core->peerName());
}

quint16 LocalSocket::peerPort() const
{
    return d_ptr->core->peerPort();
}

qintptr LocalSocket::fileno() const
{
    return static_cast<qintptr>(d_ptr->core->fileno());
}

Socket::SocketType LocalSocket::type() const
{
    return static_cast<Socket::SocketType>(d_ptr->core->type());
}

Socket::SocketState LocalSocket::state() const
{
    return static_cast<Socket::SocketState>(d_ptr->core->state());
}

HostAddress::NetworkLayerProtocol LocalSocket::protocol() const
{
    return static_cast<HostAddress::NetworkLayerProtocol>(d_ptr->core->protocol());
}

QString LocalSocket::localAddressURI() const
{
    return toQString(d_ptr->core->localAddressURI());
}

QString LocalSocket::peerAddressURI() const
{
    return toQString(d_ptr->core->peerAddressURI());
}

QString LocalSocket::serverName() const
{
    return toQString(d_ptr->core->serverName());
}

QString LocalSocket::fullServerName() const
{
    return toQString(d_ptr->core->fullServerName());
}

LocalSocket *LocalSocket::accept()
{
    qtng_core::LocalSocket *raw = d_ptr->core->accept();
    if (!raw) {
        return nullptr;
    }
    return new LocalSocket(new LocalSocketPrivate(shared_ptr<qtng_core::LocalSocket>(raw)));
}

bool LocalSocket::bind(const QString &name, Socket::BindMode mode)
{
    return d_ptr->core->bind(toStdString(name), static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
}

bool LocalSocket::connect(const QString &name)
{
    return d_ptr->core->connect(toStdString(name));
}

void LocalSocket::close()
{
    d_ptr->core->close();
}

void LocalSocket::abort()
{
    d_ptr->core->abort();
}

bool LocalSocket::listen(int backlog)
{
    return d_ptr->core->listen(backlog);
}

bool LocalSocket::setOption(Socket::SocketOption option, const QVariant &value)
{
    return d_ptr->core->setOption(static_cast<qtng_core::Socket::SocketOption>(option), value.toInt());
}

QVariant LocalSocket::option(Socket::SocketOption option) const
{
    return d_ptr->core->option(static_cast<qtng_core::Socket::SocketOption>(option));
}

qint32 LocalSocket::peek(char *data, qint32 size)
{
    return d_ptr->core->peek(data, size);
}

qint32 LocalSocket::peekRaw(char *data, qint32 size)
{
    return d_ptr->core->peekRaw(data, size);
}

qint32 LocalSocket::recv(char *data, qint32 size)
{
    return d_ptr->core->recv(data, size);
}

qint32 LocalSocket::recvall(char *data, qint32 size)
{
    return d_ptr->core->recvall(data, size);
}

qint32 LocalSocket::send(const char *data, qint32 size)
{
    return d_ptr->core->send(data, size);
}

qint32 LocalSocket::sendall(const char *data, qint32 size)
{
    return d_ptr->core->sendall(data, size);
}

QByteArray LocalSocket::recv(qint32 size)
{
    return toQByteArray(d_ptr->core->recv(size));
}

QByteArray LocalSocket::recvall(qint32 size)
{
    return toQByteArray(d_ptr->core->recvall(size));
}

qint32 LocalSocket::send(const QByteArray &data)
{
    return d_ptr->core->send(toStdString(data));
}

qint32 LocalSocket::sendall(const QByteArray &data)
{
    return d_ptr->core->sendall(toStdString(data));
}

LocalSocket *LocalSocket::createConnection(const QString &name, Socket::SocketError *error)
{
    qtng_core::Socket::SocketError coreError = qtng_core::Socket::NoError;
    qtng_core::LocalSocket *raw =
            qtng_core::LocalSocket::createConnection(toStdString(name), error ? &coreError : nullptr);
    if (error) {
        *error = static_cast<Socket::SocketError>(coreError);
    }
    if (!raw) {
        return nullptr;
    }
    return new LocalSocket(new LocalSocketPrivate(shared_ptr<qtng_core::LocalSocket>(raw)));
}

LocalSocket *LocalSocket::createServer(const QString &name, int backlog)
{
    qtng_core::LocalSocket *raw = qtng_core::LocalSocket::createServer(toStdString(name), backlog);
    if (!raw) {
        return nullptr;
    }
    return new LocalSocket(new LocalSocketPrivate(shared_ptr<qtng_core::LocalSocket>(raw)));
}

namespace {

class LocalSocketLikeImpl : public SocketLike
{
public:
    explicit LocalSocketLikeImpl(QSharedPointer<LocalSocket> s)
        : s(std::move(s))
    {
    }

    Socket::SocketError error() const override { return s->error(); }
    QString errorString() const override { return s->errorString(); }
    bool isValid() const override { return s->isValid(); }
    HostAddress localAddress() const override { return s->localAddress(); }
    quint16 localPort() const override { return s->localPort(); }
    HostAddress peerAddress() const override { return s->peerAddress(); }
    QString peerName() const override { return s->peerName(); }
    quint16 peerPort() const override { return s->peerPort(); }
    qintptr fileno() const override { return s->fileno(); }
    Socket::SocketType type() const override { return s->type(); }
    Socket::SocketState state() const override { return s->state(); }
    HostAddress::NetworkLayerProtocol protocol() const override { return s->protocol(); }
    QString localAddressURI() const override { return s->localAddressURI(); }
    QString peerAddressURI() const override { return s->peerAddressURI(); }

    QSharedPointer<SocketLike> accept() override
    {
        if (LocalSocket *raw = s->accept()) {
            return asSocketLike(QSharedPointer<LocalSocket>(raw));
        }
        return QSharedPointer<SocketLike>();
    }

    Socket *acceptRaw() override { return nullptr; }
    bool bind(const HostAddress &, quint16, Socket::BindMode) override { return false; }
    bool bind(quint16, Socket::BindMode) override { return false; }
    bool connect(const HostAddress &, quint16) override { return false; }
    // A local socket has no host:port peer; callers must use LocalSocket::connect(name).
    bool connect(const QString &, quint16, QSharedPointer<SocketDnsCache>) override { return false; }
    void close() override { s->close(); }
    void abort() override { s->abort(); }
    bool listen(int backlog) override { return s->listen(backlog); }
    bool setOption(Socket::SocketOption option, const QVariant &value) override
    {
        return s->setOption(option, value);
    }
    QVariant option(Socket::SocketOption option) const override { return s->option(option); }
    qint32 peek(char *data, qint32 size) override { return s->peek(data, size); }
    qint32 peekRaw(char *data, qint32 size) override { return s->peekRaw(data, size); }
    qint32 recv(char *data, qint32 size) override { return s->recv(data, size); }
    qint32 recvall(char *data, qint32 size) override { return s->recvall(data, size); }
    qint32 send(const char *data, qint32 size) override { return s->send(data, size); }
    qint32 sendall(const char *data, qint32 size) override { return s->sendall(data, size); }
    QByteArray recv(qint32 size) override { return s->recv(size); }
    QByteArray recvall(qint32 size) override { return s->recvall(size); }
    qint32 send(const QByteArray &data) override { return s->send(data); }
    qint32 sendall(const QByteArray &data) override { return s->sendall(data); }

    QSharedPointer<LocalSocket> localSocket() const { return s; }

    QSharedPointer<LocalSocket> s;
};

}  // namespace

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<LocalSocket> s)
{
    if (s.isNull()) {
        return QSharedPointer<SocketLike>();
    }
    return QSharedPointer<SocketLike>(new LocalSocketLikeImpl(s));
}

QSharedPointer<LocalSocket> convertSocketLikeToLocalSocket(QSharedPointer<SocketLike> socket)
{
    if (QSharedPointer<LocalSocketLikeImpl> impl = socket.dynamicCast<LocalSocketLikeImpl>()) {
        return impl->localSocket();
    }
    return QSharedPointer<LocalSocket>();
}

}  // namespace QTNETWORKNG_NAMESPACE

namespace qtng_bridge {

std::shared_ptr<qtng_core::LocalSocket> localSocketCoreOf(QTNETWORKNG_NAMESPACE::LocalSocket *socket)
{
    if (!socket) {
        return std::shared_ptr<qtng_core::LocalSocket>();
    }
    return QTNETWORKNG_NAMESPACE::LocalSocketPrivate::coreOf(socket);
}

void assignLocalSocketCore(QTNETWORKNG_NAMESPACE::LocalSocket *socket, std::shared_ptr<qtng_core::LocalSocket> core)
{
    if (socket) {
        QTNETWORKNG_NAMESPACE::LocalSocketPrivate::coreOf(socket) = std::move(core);
    }
}

std::shared_ptr<qtng_core::SocketLike> localSocketToCoreSocketLike(const QSharedPointer<SocketLike> &socket)
{
    if (socket.isNull()) {
        return std::shared_ptr<qtng_core::SocketLike>();
    }
    QSharedPointer<LocalSocketLikeImpl> impl = socket.dynamicCast<LocalSocketLikeImpl>();
    if (impl.isNull()) {
        return std::shared_ptr<qtng_core::SocketLike>();
    }
    std::shared_ptr<qtng_core::LocalSocket> core = localSocketCoreOf(impl->localSocket().data());
    if (!core) {
        return std::shared_ptr<qtng_core::SocketLike>();
    }
    return qtng_core::asSocketLike(core);
}

QSharedPointer<SocketLike> localSocketFromCoreSocketLike(const std::shared_ptr<qtng_core::SocketLike> &core)
{
    if (!core || core->type() != qtng_core::Socket::LocalSocket) {
        return QSharedPointer<SocketLike>();
    }
    std::shared_ptr<qtng_core::LocalSocket> coreLocal = qtng_core::convertSocketLikeToLocalSocket(core);
    if (!coreLocal) {
        return QSharedPointer<SocketLike>();
    }
    QSharedPointer<LocalSocket> wrapper = QSharedPointer<LocalSocket>::create();
    assignLocalSocketCore(wrapper.data(), coreLocal);
    return asSocketLike(wrapper);
}

}  // namespace qtng_bridge
