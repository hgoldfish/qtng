#include "bridge/core_access.h"
#include "bridge/stream_bridge.h"
#include "socket_utils.h"
#include "socket.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

namespace {

class QtSocketLikeImpl : public SocketLike
{
public:
    explicit QtSocketLikeImpl(QSharedPointer<Socket> s)
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
        if (Socket *raw = s->accept()) {
            return asSocketLike(QSharedPointer<Socket>(raw));
        }
        return QSharedPointer<SocketLike>();
    }

    Socket *acceptRaw() override { return s->accept(); }
    bool bind(const HostAddress &address, quint16 port, Socket::BindMode mode) override
    {
        return s->bind(address, port, mode);
    }
    bool bind(quint16 port, Socket::BindMode mode) override { return s->bind(port, mode); }
    bool connect(const HostAddress &addr, quint16 port) override { return s->connect(addr, port); }
    bool connect(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache) override
    {
        return s->connect(hostName, port, dnsCache);
    }
    void close() override { s->close(); }
    void abort() override { s->abort(); }
    bool listen(int backlog) override { return s->listen(backlog); }
    bool setOption(Socket::SocketOption option, const QVariant &value) override
    {
        return s->setOption(option, value);
    }
    QVariant option(Socket::SocketOption option) const override { return s->option(option); }
    qint32 peek(char *data, qint32 size) override { return s->peek(data, size); }
    qint32 peekRaw(char *data, qint32 size) override { return s->peek(data, size); }
    qint32 recv(char *data, qint32 size) override { return s->recv(data, size); }
    qint32 recvall(char *data, qint32 size) override { return s->recvall(data, size); }
    qint32 send(const char *data, qint32 size) override { return s->send(data, size); }
    qint32 sendall(const char *data, qint32 size) override { return s->sendall(data, size); }
    QByteArray recv(qint32 size) override { return s->recv(size); }
    QByteArray recvall(qint32 size) override { return s->recvall(size); }
    qint32 send(const QByteArray &data) override { return s->send(data); }
    qint32 sendall(const QByteArray &data) override { return s->sendall(data); }

    QSharedPointer<Socket> socket() const { return s; }

    QSharedPointer<Socket> s;
};

}  // namespace

SocketLike::SocketLike() { }

SocketLike::~SocketLike() { }

qint32 SocketLike::read(char *data, qint32 size)
{
    return recv(data, size);
}

qint32 SocketLike::write(const char *data, qint32 size)
{
    return sendall(data, size);
}

qint64 SocketLike::size()
{
    return -1;
}

class ExchangerPrivate
{
public:
    ExchangerPrivate(QSharedPointer<SocketLike> request, QSharedPointer<SocketLike> forward, quint32 maxBufferSize)
        : request(std::move(request))
        , forward(std::move(forward))
        , maxBufferSize(maxBufferSize)
    {
    }

    QSharedPointer<SocketLike> request;
    QSharedPointer<SocketLike> forward;
    quint32 maxBufferSize;
};

Exchanger::Exchanger(QSharedPointer<SocketLike> request, QSharedPointer<SocketLike> forward, quint32 maxBufferSize)
    : d_ptr(new ExchangerPrivate(std::move(request), std::move(forward), maxBufferSize))
{
}

Exchanger::~Exchanger()
{
    delete d_ptr;
}

void Exchanger::exchange()
{
    Q_D(Exchanger);
    qtng_core::Exchanger exch(toCoreSocketLike(d->request), toCoreSocketLike(d->forward), d->maxBufferSize);
    exch.exchange();
}

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<Socket> s)
{
    if (s.isNull()) {
        return QSharedPointer<SocketLike>();
    }
    return QSharedPointer<SocketLike>(new QtSocketLikeImpl(s));
}

QSharedPointer<Socket> convertSocketLikeToSocket(QSharedPointer<SocketLike> socket)
{
    if (QSharedPointer<QtSocketLikeImpl> impl = socket.dynamicCast<QtSocketLikeImpl>()) {
        return impl->socket();
    }
    shared_ptr<qtng_core::Socket> coreSocket = qtng_core::convertSocketLikeToSocket(toCoreSocketLike(socket));
    if (!coreSocket) {
        return QSharedPointer<Socket>();
    }
    return QSharedPointer<Socket>(new Socket(static_cast<qintptr>(coreSocket->fileno())));
}

}  // namespace QTNETWORKNG_NAMESPACE
