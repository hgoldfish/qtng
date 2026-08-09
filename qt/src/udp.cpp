#include "bridge/core_access.h"
#include "bridge/socket_access.h"
#include "bridge/qt_socket_bridge.h"
#include "udp.h"
#include "kcp_base.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

namespace {

class EventPrivate
{
public:
    qtng_core::Event core;
};

void linkSocketEvents(KcpSocket *socket)
{
    Q_UNUSED(socket);
}

void linkSocketEvents(UtpSocket *socket)
{
    Q_UNUSED(socket);
}

class QtKcpSocketCore : public qtng_core::KcpSocket
{
public:
    QtKcpSocketCore(::qtng::KcpSocket *qtSocket, qtng_core::HostAddress::NetworkLayerProtocol protocol)
        : qtng_core::KcpSocket(protocol)
        , qt(qtSocket)
    {
    }

    QtKcpSocketCore(::qtng::KcpSocket *qtSocket, intptr_t socketDescriptor)
        : qtng_core::KcpSocket(socketDescriptor)
        , qt(qtSocket)
    {
    }

    QtKcpSocketCore(::qtng::KcpSocket *qtSocket, shared_ptr<qtng_core::Socket> rawSocket)
        : qtng_core::KcpSocket(rawSocket)
        , qt(qtSocket)
    {
    }

    bool filter(char *data, int32_t *len, qtng_core::HostAddress *addr, uint16_t *port) override
    {
        if (!qt) {
            return false;
        }
        HostAddress qtAddr;
        quint16 qtPort = 0;
        const bool ok = qt->filter(data, len, &qtAddr, &qtPort);
        if (addr) {
            *addr = toCoreAddress(qtAddr);
        }
        if (port) {
            *port = qtPort;
        }
        return ok;
    }

    ::qtng::KcpSocket *qt;
};

class QtUtpSocketCore : public qtng_core::UtpSocket
{
public:
    QtUtpSocketCore(::qtng::UtpSocket *qtSocket, qtng_core::HostAddress::NetworkLayerProtocol protocol)
        : qtng_core::UtpSocket(protocol)
        , qt(qtSocket)
    {
    }

    QtUtpSocketCore(::qtng::UtpSocket *qtSocket, intptr_t socketDescriptor)
        : qtng_core::UtpSocket(socketDescriptor)
        , qt(qtSocket)
    {
    }

    QtUtpSocketCore(::qtng::UtpSocket *qtSocket, shared_ptr<qtng_core::Socket> rawSocket)
        : qtng_core::UtpSocket(rawSocket)
        , qt(qtSocket)
    {
    }

    bool filter(char *data, int32_t *len, qtng_core::HostAddress *addr, uint16_t *port) override
    {
        if (!qt) {
            return false;
        }
        HostAddress qtAddr;
        quint16 qtPort = 0;
        const bool ok = qt->filter(data, len, &qtAddr, &qtPort);
        if (addr) {
            *addr = toCoreAddress(qtAddr);
        }
        if (port) {
            *port = qtPort;
        }
        return ok;
    }

    ::qtng::UtpSocket *qt;
};

}  // namespace

class KcpSocketPrivate
{
public:
    explicit KcpSocketPrivate(KcpSocket *q, qtng_core::HostAddress::NetworkLayerProtocol protocol)
        : core(new QtKcpSocketCore(q, protocol))
    {
    }

    explicit KcpSocketPrivate(KcpSocket *q, qintptr socketDescriptor)
        : core(new QtKcpSocketCore(q, static_cast<intptr_t>(socketDescriptor)))
    {
    }

    KcpSocketPrivate(KcpSocket *q, shared_ptr<qtng_core::Socket> rawSocket)
        : core(new QtKcpSocketCore(q, rawSocket))
    {
    }

    explicit KcpSocketPrivate(qtng_core::KcpSocket *adopted)
        : core(adopted)
        , ownsCore(true)
    {
    }

    static KcpSocketPrivate *adopt(qtng_core::KcpSocket *adopted)
    {
        return new KcpSocketPrivate(adopted);
    }

    static KcpSocket *fromAdopted(qtng_core::KcpSocket *adopted)
    {
        return new KcpSocket(adopt(adopted));
    }

    static shared_ptr<qtng_core::KcpSocket> sharedCore(KcpSocket *socket)
    {
        if (!socket) {
            return shared_ptr<qtng_core::KcpSocket>();
        }
        QSharedPointer<KcpSocket> holder(socket, [](KcpSocket *) {});
        return shared_ptr<qtng_core::KcpSocket>(socket->d_ptr->core, [holder](qtng_core::KcpSocket *) {});
    }

    ~KcpSocketPrivate()
    {
        if (ownsCore) {
            delete core;
        }
    }

    qtng_core::KcpSocket *core;
    bool ownsCore = true;
};

KcpSocket::KcpSocket(HostAddress::NetworkLayerProtocol protocol)
    : d_ptr(new KcpSocketPrivate(this, static_cast<qtng_core::HostAddress::NetworkLayerProtocol>(protocol)))
    , busy()
    , notBusy()
{
    linkSocketEvents(this);
}

KcpSocket::KcpSocket(qintptr socketDescriptor)
    : d_ptr(new KcpSocketPrivate(this, socketDescriptor))
    , busy()
    , notBusy()
{
    linkSocketEvents(this);
}

KcpSocket::KcpSocket(QSharedPointer<Socket> rawSocket)
    : d_ptr(new KcpSocketPrivate(this, socketCoreOf(rawSocket.data())))
    , busy()
    , notBusy()
{
    linkSocketEvents(this);
}

KcpSocket::KcpSocket(KcpSocketPrivate *d)
    : d_ptr(d)
    , busy()
    , notBusy()
{
    linkSocketEvents(this);
}

KcpSocket::~KcpSocket()
{
    delete d_ptr;
}

void KcpSocket::setMode(Mode mode)
{
    d_ptr->core->setMode(static_cast<qtng_core::KcpSocket::Mode>(mode));
}

KcpSocket::Mode KcpSocket::mode() const
{
    return static_cast<Mode>(d_ptr->core->mode());
}

void KcpSocket::setSendQueueSize(quint32 sendQueueSize)
{
    d_ptr->core->setSendQueueSize(sendQueueSize);
}

quint32 KcpSocket::sendQueueSize() const
{
    return d_ptr->core->sendQueueSize();
}

void KcpSocket::setUdpPacketSize(quint32 udpPacketSize)
{
    d_ptr->core->setUdpPacketSize(udpPacketSize);
}

quint32 KcpSocket::udpPacketSize() const
{
    return d_ptr->core->udpPacketSize();
}

quint32 KcpSocket::payloadSizeHint() const
{
    return d_ptr->core->payloadSizeHint();
}

void KcpSocket::setTearDownTime(float secs)
{
    d_ptr->core->setTearDownTime(secs);
}

float KcpSocket::tearDownTime() const
{
    return d_ptr->core->tearDownTime();
}

Socket::SocketError KcpSocket::error() const
{
    return static_cast<Socket::SocketError>(d_ptr->core->error());
}

QString KcpSocket::errorString() const
{
    return toQString(d_ptr->core->errorString());
}

bool KcpSocket::isValid() const
{
    return d_ptr->core->isValid();
}

HostAddress KcpSocket::localAddress() const
{
    return toQtAddress(d_ptr->core->localAddress());
}

quint16 KcpSocket::localPort() const
{
    return d_ptr->core->localPort();
}

HostAddress KcpSocket::peerAddress() const
{
    return toQtAddress(d_ptr->core->peerAddress());
}

QString KcpSocket::peerName() const
{
    return toQString(d_ptr->core->peerName());
}

quint16 KcpSocket::peerPort() const
{
    return d_ptr->core->peerPort();
}

Socket::SocketType KcpSocket::type() const
{
    return static_cast<Socket::SocketType>(d_ptr->core->type());
}

Socket::SocketState KcpSocket::state() const
{
    return static_cast<Socket::SocketState>(d_ptr->core->state());
}

HostAddress::NetworkLayerProtocol KcpSocket::protocol() const
{
    return static_cast<HostAddress::NetworkLayerProtocol>(d_ptr->core->protocol());
}

QString KcpSocket::localAddressURI() const
{
    return toQString(d_ptr->core->localAddressURI());
}

QString KcpSocket::peerAddressURI() const
{
    return toQString(d_ptr->core->peerAddressURI());
}

KcpSocket *KcpSocket::accept()
{
    qtng_core::KcpSocket *accepted = d_ptr->core->accept();
    if (!accepted) {
        return nullptr;
    }
    return new KcpSocket(KcpSocketPrivate::adopt(accepted));
}

KcpSocket *KcpSocket::accept(const HostAddress &addr, quint16 port)
{
    qtng_core::KcpSocket *accepted = d_ptr->core->accept(toCoreAddress(addr), port);
    if (!accepted) {
        return nullptr;
    }
    return new KcpSocket(KcpSocketPrivate::adopt(accepted));
}

KcpSocket *KcpSocket::accept(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache)
{
    qtng_core::KcpSocket *accepted =
            d_ptr->core->accept(toStdString(hostName), port, dnsCacheCoreOf(dnsCache.data()));
    if (!accepted) {
        return nullptr;
    }
    return new KcpSocket(KcpSocketPrivate::adopt(accepted));
}

bool KcpSocket::bind(const HostAddress &address, quint16 port, Socket::BindMode mode)
{
    return d_ptr->core->bind(toCoreAddress(address), port,
                             static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
}

bool KcpSocket::bind(quint16 port, Socket::BindMode mode)
{
    return d_ptr->core->bind(port, static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
}

bool KcpSocket::connect(const HostAddress &addr, quint16 port)
{
    return d_ptr->core->connect(toCoreAddress(addr), port);
}

bool KcpSocket::connect(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache)
{
    return d_ptr->core->connect(toStdString(hostName), port, dnsCacheCoreOf(dnsCache.data()));
}

void KcpSocket::close()
{
    d_ptr->core->close();
}

void KcpSocket::abort()
{
    d_ptr->core->abort();
}

bool KcpSocket::listen(int backlog)
{
    return d_ptr->core->listen(backlog);
}

bool KcpSocket::setOption(Socket::SocketOption option, const QVariant &value)
{
    return d_ptr->core->setOption(static_cast<qtng_core::Socket::SocketOption>(static_cast<int>(option)), value.toInt());
}

QVariant KcpSocket::option(Socket::SocketOption option) const
{
    return d_ptr->core->option(static_cast<qtng_core::Socket::SocketOption>(static_cast<int>(option)));
}

bool KcpSocket::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->core->joinMulticastGroup(toCoreAddress(groupAddress), toCoreInterface(iface));
}

bool KcpSocket::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->core->leaveMulticastGroup(toCoreAddress(groupAddress), toCoreInterface(iface));
}

NetworkInterface KcpSocket::multicastInterface() const
{
    return toQtInterface(d_ptr->core->multicastInterface());
}

bool KcpSocket::setMulticastInterface(const NetworkInterface &iface)
{
    return d_ptr->core->setMulticastInterface(toCoreInterface(iface));
}

qint32 KcpSocket::peek(char *data, qint32 size)
{
    return d_ptr->core->peek(data, size);
}

qint32 KcpSocket::peekRaw(char *data, qint32 size)
{
    return d_ptr->core->peekRaw(data, size);
}

qint32 KcpSocket::recv(char *data, qint32 size)
{
    return d_ptr->core->recv(data, size);
}

qint32 KcpSocket::recvall(char *data, qint32 size)
{
    return d_ptr->core->recvall(data, size);
}

qint32 KcpSocket::send(const char *data, qint32 size)
{
    return d_ptr->core->send(data, size);
}

qint32 KcpSocket::sendall(const char *data, qint32 size)
{
    return d_ptr->core->sendall(data, size);
}

QByteArray KcpSocket::recv(qint32 size)
{
    return toQByteArray(d_ptr->core->recv(size));
}

QByteArray KcpSocket::recvall(qint32 size)
{
    return toQByteArray(d_ptr->core->recvall(size));
}

qint32 KcpSocket::send(const QByteArray &data)
{
    return d_ptr->core->send(toStdString(data));
}

qint32 KcpSocket::sendall(const QByteArray &data)
{
    return d_ptr->core->sendall(toStdString(data));
}

bool KcpSocket::filter(char *data, qint32 *len, HostAddress *addr, quint16 *port)
{
    Q_UNUSED(data);
    Q_UNUSED(len);
    Q_UNUSED(addr);
    Q_UNUSED(port);
    return false;
}

qint32 KcpSocket::udpSend(const char *data, qint32 size, const HostAddress &addr, quint16 port)
{
    return d_ptr->core->udpSend(data, size, toCoreAddress(addr), port);
}

KcpSocket *KcpSocket::createConnection(const HostAddress &host, quint16 port, Socket::SocketError *error,
                                       int allowProtocol, Mode mode)
{
    qtng_core::Socket::SocketError coreError = qtng_core::Socket::UnknownSocketError;
    qtng_core::KcpSocket *raw = qtng_core::KcpSocket::createConnection(
            toCoreAddress(host), port, error ? &coreError : nullptr, allowProtocol,
            static_cast<qtng_core::KcpSocket::Mode>(mode));
    if (error) {
        *error = static_cast<Socket::SocketError>(coreError);
    }
    if (!raw) {
        return nullptr;
    }
    return new KcpSocket(KcpSocketPrivate::adopt(raw));
}

KcpSocket *KcpSocket::createConnection(const QString &hostName, quint16 port, Socket::SocketError *error,
                                       QSharedPointer<SocketDnsCache> dnsCache, int allowProtocol, Mode mode)
{
    qtng_core::Socket::SocketError coreError = qtng_core::Socket::UnknownSocketError;
    qtng_core::KcpSocket *raw = qtng_core::KcpSocket::createConnection(
            toStdString(hostName), port, error ? &coreError : nullptr, dnsCacheCoreOf(dnsCache.data()), allowProtocol,
            static_cast<qtng_core::KcpSocket::Mode>(mode));
    if (error) {
        *error = static_cast<Socket::SocketError>(coreError);
    }
    if (!raw) {
        return nullptr;
    }
    return new KcpSocket(KcpSocketPrivate::adopt(raw));
}

KcpSocket *KcpSocket::createServer(const HostAddress &host, quint16 port, int backlog)
{
    qtng_core::KcpSocket *raw = qtng_core::KcpSocket::createServer(toCoreAddress(host), port, backlog);
    if (!raw) {
        return nullptr;
    }
    return new KcpSocket(KcpSocketPrivate::adopt(raw));
}

class UtpSocketPrivate
{
public:
    explicit UtpSocketPrivate(UtpSocket *q, qtng_core::HostAddress::NetworkLayerProtocol protocol)
        : core(new QtUtpSocketCore(q, protocol))
    {
    }

    explicit UtpSocketPrivate(UtpSocket *q, qintptr socketDescriptor)
        : core(new QtUtpSocketCore(q, static_cast<intptr_t>(socketDescriptor)))
    {
    }

    UtpSocketPrivate(UtpSocket *q, shared_ptr<qtng_core::Socket> rawSocket)
        : core(new QtUtpSocketCore(q, rawSocket))
    {
    }

    explicit UtpSocketPrivate(qtng_core::UtpSocket *adopted)
        : core(adopted)
        , ownsCore(true)
    {
    }

    static UtpSocketPrivate *adopt(qtng_core::UtpSocket *adopted)
    {
        return new UtpSocketPrivate(adopted);
    }

    static UtpSocket *fromAdopted(qtng_core::UtpSocket *adopted)
    {
        return new UtpSocket(adopt(adopted));
    }

    static shared_ptr<qtng_core::UtpSocket> sharedCore(UtpSocket *socket)
    {
        if (!socket) {
            return shared_ptr<qtng_core::UtpSocket>();
        }
        QSharedPointer<UtpSocket> holder(socket, [](UtpSocket *) {});
        return shared_ptr<qtng_core::UtpSocket>(socket->d_ptr->core, [holder](qtng_core::UtpSocket *) {});
    }

    ~UtpSocketPrivate()
    {
        if (ownsCore) {
            delete core;
        }
    }

    qtng_core::UtpSocket *core;
    bool ownsCore = true;
};

UtpSocket::UtpSocket(HostAddress::NetworkLayerProtocol protocol)
    : d_ptr(new UtpSocketPrivate(this, static_cast<qtng_core::HostAddress::NetworkLayerProtocol>(protocol)))
    , busy()
    , notBusy()
{
    linkSocketEvents(this);
}

UtpSocket::UtpSocket(qintptr socketDescriptor)
    : d_ptr(new UtpSocketPrivate(this, socketDescriptor))
    , busy()
    , notBusy()
{
    linkSocketEvents(this);
}

UtpSocket::UtpSocket(QSharedPointer<Socket> rawSocket)
    : d_ptr(new UtpSocketPrivate(this, socketCoreOf(rawSocket.data())))
    , busy()
    , notBusy()
{
    linkSocketEvents(this);
}

UtpSocket::UtpSocket(UtpSocketPrivate *d)
    : d_ptr(d)
    , busy()
    , notBusy()
{
    linkSocketEvents(this);
}

UtpSocket::~UtpSocket()
{
    delete d_ptr;
}

void UtpSocket::setDelayTarget(float milliseconds) { d_ptr->core->setDelayTarget(milliseconds); }
float UtpSocket::delayTarget() const { return d_ptr->core->delayTarget(); }
void UtpSocket::setMaxWindow(quint32 bytes) { d_ptr->core->setMaxWindow(bytes); }
quint32 UtpSocket::maxWindow() const { return d_ptr->core->maxWindow(); }
void UtpSocket::setPacketSize(quint32 bytes) { d_ptr->core->setPacketSize(bytes); }
quint32 UtpSocket::packetSize() const { return d_ptr->core->packetSize(); }
quint32 UtpSocket::payloadSizeHint() const { return d_ptr->core->payloadSizeHint(); }
void UtpSocket::setReceiveBufferSize(quint32 bytes) { d_ptr->core->setReceiveBufferSize(bytes); }
quint32 UtpSocket::receiveBufferSize() const { return d_ptr->core->receiveBufferSize(); }
void UtpSocket::setIdleTimeout(float seconds) { d_ptr->core->setIdleTimeout(seconds); }
float UtpSocket::idleTimeout() const { return d_ptr->core->idleTimeout(); }

Socket::SocketError UtpSocket::error() const { return static_cast<Socket::SocketError>(d_ptr->core->error()); }
QString UtpSocket::errorString() const { return toQString(d_ptr->core->errorString()); }
bool UtpSocket::isValid() const { return d_ptr->core->isValid(); }
HostAddress UtpSocket::localAddress() const { return toQtAddress(d_ptr->core->localAddress()); }
quint16 UtpSocket::localPort() const { return d_ptr->core->localPort(); }
HostAddress UtpSocket::peerAddress() const { return toQtAddress(d_ptr->core->peerAddress()); }
QString UtpSocket::peerName() const { return toQString(d_ptr->core->peerName()); }
quint16 UtpSocket::peerPort() const { return d_ptr->core->peerPort(); }
Socket::SocketType UtpSocket::type() const { return static_cast<Socket::SocketType>(d_ptr->core->type()); }
Socket::SocketState UtpSocket::state() const { return static_cast<Socket::SocketState>(d_ptr->core->state()); }
HostAddress::NetworkLayerProtocol UtpSocket::protocol() const
{
    return static_cast<HostAddress::NetworkLayerProtocol>(d_ptr->core->protocol());
}
QString UtpSocket::localAddressURI() const { return toQString(d_ptr->core->localAddressURI()); }
QString UtpSocket::peerAddressURI() const { return toQString(d_ptr->core->peerAddressURI()); }

UtpSocket *UtpSocket::accept()
{
    qtng_core::UtpSocket *accepted = d_ptr->core->accept();
    return accepted ? new UtpSocket(UtpSocketPrivate::adopt(accepted)) : nullptr;
}

UtpSocket *UtpSocket::accept(const HostAddress &addr, quint16 port)
{
    qtng_core::UtpSocket *accepted = d_ptr->core->accept(toCoreAddress(addr), port);
    return accepted ? new UtpSocket(UtpSocketPrivate::adopt(accepted)) : nullptr;
}

UtpSocket *UtpSocket::accept(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache)
{
    qtng_core::UtpSocket *accepted =
            d_ptr->core->accept(toStdString(hostName), port, dnsCacheCoreOf(dnsCache.data()));
    return accepted ? new UtpSocket(UtpSocketPrivate::adopt(accepted)) : nullptr;
}

bool UtpSocket::bind(const HostAddress &address, quint16 port, Socket::BindMode mode)
{
    return d_ptr->core->bind(toCoreAddress(address), port,
                             static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
}

bool UtpSocket::bind(quint16 port, Socket::BindMode mode)
{
    return d_ptr->core->bind(port, static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
}

bool UtpSocket::connect(const HostAddress &addr, quint16 port)
{
    return d_ptr->core->connect(toCoreAddress(addr), port);
}

bool UtpSocket::connect(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache)
{
    return d_ptr->core->connect(toStdString(hostName), port, dnsCacheCoreOf(dnsCache.data()));
}

void UtpSocket::close() { d_ptr->core->close(); }
void UtpSocket::abort() { d_ptr->core->abort(); }
bool UtpSocket::listen(int backlog) { return d_ptr->core->listen(backlog); }

bool UtpSocket::setOption(Socket::SocketOption option, const QVariant &value)
{
    return d_ptr->core->setOption(static_cast<qtng_core::Socket::SocketOption>(static_cast<int>(option)), value.toInt());
}

QVariant UtpSocket::option(Socket::SocketOption option) const
{
    return d_ptr->core->option(static_cast<qtng_core::Socket::SocketOption>(static_cast<int>(option)));
}

bool UtpSocket::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->core->joinMulticastGroup(toCoreAddress(groupAddress), toCoreInterface(iface));
}

bool UtpSocket::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->core->leaveMulticastGroup(toCoreAddress(groupAddress), toCoreInterface(iface));
}

NetworkInterface UtpSocket::multicastInterface() const
{
    return toQtInterface(d_ptr->core->multicastInterface());
}

bool UtpSocket::setMulticastInterface(const NetworkInterface &iface)
{
    return d_ptr->core->setMulticastInterface(toCoreInterface(iface));
}

qint32 UtpSocket::peek(char *data, qint32 size) { return d_ptr->core->peek(data, size); }
qint32 UtpSocket::peekRaw(char *data, qint32 size) { return d_ptr->core->peekRaw(data, size); }
qint32 UtpSocket::recv(char *data, qint32 size) { return d_ptr->core->recv(data, size); }
qint32 UtpSocket::recvall(char *data, qint32 size) { return d_ptr->core->recvall(data, size); }
qint32 UtpSocket::send(const char *data, qint32 size) { return d_ptr->core->send(data, size); }
qint32 UtpSocket::sendall(const char *data, qint32 size) { return d_ptr->core->sendall(data, size); }
QByteArray UtpSocket::recv(qint32 size) { return toQByteArray(d_ptr->core->recv(size)); }
QByteArray UtpSocket::recvall(qint32 size) { return toQByteArray(d_ptr->core->recvall(size)); }
qint32 UtpSocket::send(const QByteArray &data) { return d_ptr->core->send(toStdString(data)); }
qint32 UtpSocket::sendall(const QByteArray &data) { return d_ptr->core->sendall(toStdString(data)); }

bool UtpSocket::filter(char *data, qint32 *len, HostAddress *addr, quint16 *port)
{
    Q_UNUSED(data);
    Q_UNUSED(len);
    Q_UNUSED(addr);
    Q_UNUSED(port);
    return false;
}

qint32 UtpSocket::udpSend(const char *data, qint32 size, const HostAddress &addr, quint16 port)
{
    return d_ptr->core->udpSend(data, size, toCoreAddress(addr), port);
}

UtpSocket *UtpSocket::createConnection(const HostAddress &host, quint16 port, Socket::SocketError *error,
                                       int allowProtocol)
{
    qtng_core::Socket::SocketError coreError = qtng_core::Socket::UnknownSocketError;
    qtng_core::UtpSocket *raw = qtng_core::UtpSocket::createConnection(
            toCoreAddress(host), port, error ? &coreError : nullptr, allowProtocol);
    if (error) {
        *error = static_cast<Socket::SocketError>(coreError);
    }
    return raw ? new UtpSocket(UtpSocketPrivate::adopt(raw)) : nullptr;
}

UtpSocket *UtpSocket::createConnection(const QString &hostName, quint16 port, Socket::SocketError *error,
                                       QSharedPointer<SocketDnsCache> dnsCache, int allowProtocol)
{
    qtng_core::Socket::SocketError coreError = qtng_core::Socket::UnknownSocketError;
    qtng_core::UtpSocket *raw = qtng_core::UtpSocket::createConnection(
            toStdString(hostName), port, error ? &coreError : nullptr, dnsCacheCoreOf(dnsCache.data()), allowProtocol);
    if (error) {
        *error = static_cast<Socket::SocketError>(coreError);
    }
    return raw ? new UtpSocket(UtpSocketPrivate::adopt(raw)) : nullptr;
}

UtpSocket *UtpSocket::createServer(const HostAddress &host, quint16 port, int backlog)
{
    qtng_core::UtpSocket *raw = qtng_core::UtpSocket::createServer(toCoreAddress(host), port, backlog);
    return raw ? new UtpSocket(UtpSocketPrivate::adopt(raw)) : nullptr;
}

}  // namespace QTNETWORKNG_NAMESPACE

namespace qtng_bridge {

class CoreSocketLikeAdapter : public QTNETWORKNG_NAMESPACE::SocketLike
{
public:
    explicit CoreSocketLikeAdapter(std::shared_ptr<qtng_core::SocketLike> coreLike)
        : core(std::move(coreLike))
    {
    }

    QTNETWORKNG_NAMESPACE::Socket::SocketError error() const override
    {
        return static_cast<QTNETWORKNG_NAMESPACE::Socket::SocketError>(core->error());
    }

    QString errorString() const override
    {
        return toQString(core->errorString());
    }

    bool isValid() const override { return core->isValid(); }
    QTNETWORKNG_NAMESPACE::HostAddress localAddress() const override
    {
        return toQtAddress(core->localAddress());
    }
    quint16 localPort() const override { return core->localPort(); }
    QTNETWORKNG_NAMESPACE::HostAddress peerAddress() const override
    {
        return toQtAddress(core->peerAddress());
    }
    QString peerName() const override { return toQString(core->peerName()); }
    quint16 peerPort() const override { return core->peerPort(); }
    qintptr fileno() const override { return core->fileno(); }
    QTNETWORKNG_NAMESPACE::Socket::SocketType type() const override
    {
        return static_cast<QTNETWORKNG_NAMESPACE::Socket::SocketType>(core->type());
    }
    QTNETWORKNG_NAMESPACE::Socket::SocketState state() const override
    {
        return static_cast<QTNETWORKNG_NAMESPACE::Socket::SocketState>(core->state());
    }
    QTNETWORKNG_NAMESPACE::HostAddress::NetworkLayerProtocol protocol() const override
    {
        return static_cast<QTNETWORKNG_NAMESPACE::HostAddress::NetworkLayerProtocol>(core->protocol());
    }
    QString localAddressURI() const override { return toQString(core->localAddressURI()); }
    QString peerAddressURI() const override { return toQString(core->peerAddressURI()); }
    QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> accept() override
    {
        return fromCoreSocketLike(core->accept());
    }
    QTNETWORKNG_NAMESPACE::Socket *acceptRaw() override { return nullptr; }
    bool bind(const QTNETWORKNG_NAMESPACE::HostAddress &address, quint16 port,
              QTNETWORKNG_NAMESPACE::Socket::BindMode mode) override
    {
        return core->bind(toCoreAddress(address), port,
                          static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
    }
    bool bind(quint16 port, QTNETWORKNG_NAMESPACE::Socket::BindMode mode) override
    {
        return core->bind(port, static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
    }
    bool connect(const QTNETWORKNG_NAMESPACE::HostAddress &addr, quint16 port) override
    {
        return core->connect(toCoreAddress(addr), port);
    }
    bool connect(const QString &hostName, quint16 port,
                 QSharedPointer<QTNETWORKNG_NAMESPACE::SocketDnsCache> dnsCache) override
    {
        return core->connect(toStdString(hostName), port, dnsCacheCoreOf(dnsCache.data()));
    }
    void close() override { core->close(); }
    void abort() override { core->abort(); }
    bool listen(int backlog) override { return core->listen(backlog); }
    bool setOption(QTNETWORKNG_NAMESPACE::Socket::SocketOption option, const QVariant &value) override
    {
        return core->setOption(static_cast<qtng_core::Socket::SocketOption>(static_cast<int>(option)), value.toInt());
    }
    QVariant option(QTNETWORKNG_NAMESPACE::Socket::SocketOption option) const override
    {
        return core->option(static_cast<qtng_core::Socket::SocketOption>(static_cast<int>(option)));
    }
    qint32 peek(char *data, qint32 size) override { return core->peek(data, size); }
    qint32 peekRaw(char *data, qint32 size) override { return core->peekRaw(data, size); }
    qint32 recv(char *data, qint32 size) override { return core->recv(data, size); }
    qint32 recvall(char *data, qint32 size) override { return core->recvall(data, size); }
    qint32 send(const char *data, qint32 size) override { return core->send(data, size); }
    qint32 sendall(const char *data, qint32 size) override { return core->sendall(data, size); }
    QByteArray recv(qint32 size) override { return toQByteArray(core->recv(size)); }
    QByteArray recvall(qint32 size) override { return toQByteArray(core->recvall(size)); }
    qint32 send(const QByteArray &data) override { return core->send(toStdString(data)); }
    qint32 sendall(const QByteArray &data) override { return core->sendall(toStdString(data)); }

    std::shared_ptr<qtng_core::SocketLike> core;
};

class CoreSocketLikeReverseAdapter : public qtng_core::SocketLike
{
public:
    explicit CoreSocketLikeReverseAdapter(const QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> &socketLike)
        : socket(socketLike)
    {
    }

    qtng_core::Socket::SocketError error() const override
    {
        return static_cast<qtng_core::Socket::SocketError>(socket->error());
    }
    std::string errorString() const override { return toStdString(socket->errorString()); }
    bool isValid() const override { return socket->isValid(); }
    qtng_core::HostAddress localAddress() const override { return toCoreAddress(socket->localAddress()); }
    std::uint16_t localPort() const override { return socket->localPort(); }
    qtng_core::HostAddress peerAddress() const override { return toCoreAddress(socket->peerAddress()); }
    std::string peerName() const override { return toStdString(socket->peerName()); }
    std::uint16_t peerPort() const override { return socket->peerPort(); }
    intptr_t fileno() const override { return socket->fileno(); }
    qtng_core::Socket::SocketType type() const override
    {
        return static_cast<qtng_core::Socket::SocketType>(socket->type());
    }
    qtng_core::Socket::SocketState state() const override
    {
        return static_cast<qtng_core::Socket::SocketState>(socket->state());
    }
    qtng_core::HostAddress::NetworkLayerProtocol protocol() const override
    {
        return static_cast<qtng_core::HostAddress::NetworkLayerProtocol>(socket->protocol());
    }
    std::string localAddressURI() const override { return toStdString(socket->localAddressURI()); }
    std::string peerAddressURI() const override { return toStdString(socket->peerAddressURI()); }
    qtng_core::Socket *acceptRaw() override { return nullptr; }
    std::shared_ptr<qtng_core::SocketLike> accept() override { return toCoreSocketLike(socket->accept()); }
    bool bind(const qtng_core::HostAddress &address, std::uint16_t port, qtng_core::Socket::BindMode mode) override
    {
        return socket->bind(toQtAddress(address), port,
                            static_cast<QTNETWORKNG_NAMESPACE::Socket::BindMode>(static_cast<int>(mode)));
    }
    bool bind(std::uint16_t port, qtng_core::Socket::BindMode mode) override
    {
        return socket->bind(port, static_cast<QTNETWORKNG_NAMESPACE::Socket::BindMode>(static_cast<int>(mode)));
    }
    bool connect(const qtng_core::HostAddress &addr, std::uint16_t port) override
    {
        return socket->connect(toQtAddress(addr), port);
    }
    bool connect(const std::string &hostName, std::uint16_t port,
                 std::shared_ptr<qtng_core::SocketDnsCache> dnsCache) override
    {
        QSharedPointer<QTNETWORKNG_NAMESPACE::SocketDnsCache> qtCache;
        Q_UNUSED(dnsCache);
        return socket->connect(toQString(hostName), port, qtCache);
    }
    void close() override { socket->close(); }
    void abort() override { socket->abort(); }
    bool listen(int backlog) override { return socket->listen(backlog); }
    bool setOption(qtng_core::Socket::SocketOption option, int value) override
    {
        return socket->setOption(static_cast<QTNETWORKNG_NAMESPACE::Socket::SocketOption>(static_cast<int>(option)),
                                 value);
    }
    int option(qtng_core::Socket::SocketOption option) const override
    {
        return socket->option(static_cast<QTNETWORKNG_NAMESPACE::Socket::SocketOption>(static_cast<int>(option)))
                .toInt();
    }
    std::int32_t peek(char *data, std::int32_t size) override { return socket->peek(data, size); }
    std::int32_t peekRaw(char *data, std::int32_t size) override { return socket->peekRaw(data, size); }
    std::int32_t recv(char *data, std::int32_t size) override { return socket->recv(data, size); }
    std::int32_t recvall(char *data, std::int32_t size) override { return socket->recvall(data, size); }
    std::int32_t send(const char *data, std::int32_t size) override { return socket->send(data, size); }
    std::int32_t sendall(const char *data, std::int32_t size) override { return socket->sendall(data, size); }
    std::string recv(std::int32_t size) override { return toStdString(socket->recv(size)); }
    std::string recvall(std::int32_t size) override { return toStdString(socket->recvall(size)); }
    std::int32_t send(const std::string &data) override { return socket->send(toQByteArray(data)); }
    std::int32_t sendall(const std::string &data) override { return socket->sendall(toQByteArray(data)); }

    QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> socket;
};

std::shared_ptr<qtng_core::SocketLike> kcpOrUtpToCoreSocketLike(
        const QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> &socket)
{
    if (QSharedPointer<QTNETWORKNG_NAMESPACE::KcpSocket> kcp =
                socket.dynamicCast<QTNETWORKNG_NAMESPACE::KcpSocket>()) {
        return qtng_core::asSocketLike(QTNETWORKNG_NAMESPACE::KcpSocketPrivate::sharedCore(kcp.data()));
    }
    if (QSharedPointer<QTNETWORKNG_NAMESPACE::UtpSocket> utp =
                socket.dynamicCast<QTNETWORKNG_NAMESPACE::UtpSocket>()) {
        return qtng_core::asSocketLike(QTNETWORKNG_NAMESPACE::UtpSocketPrivate::sharedCore(utp.data()));
    }
    return std::shared_ptr<qtng_core::SocketLike>();
}

QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> fromCoreSocketLike(const std::shared_ptr<qtng_core::SocketLike> &socket)
{
    if (!socket) {
        return QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike>();
    }
    return QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike>(new CoreSocketLikeAdapter(socket));
}

}  // namespace qtng_bridge

namespace QTNETWORKNG_NAMESPACE {

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<KcpSocket> s)
{
    if (!s) {
        return QSharedPointer<SocketLike>();
    }
    return ::qtng_bridge::fromCoreSocketLike(qtng_core::asSocketLike(KcpSocketPrivate::sharedCore(s.data())));
}

QSharedPointer<KcpSocket> convertSocketLikeToKcpSocket(QSharedPointer<SocketLike> socket)
{
    shared_ptr<qtng_core::KcpSocket> core =
            qtng_core::convertSocketLikeToKcpSocket(::qtng_bridge::toCoreSocketLike(socket));
    if (!core) {
        return QSharedPointer<KcpSocket>();
    }
    return QSharedPointer<KcpSocket>(KcpSocketPrivate::fromAdopted(core.get()));
}

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<UtpSocket> s)
{
    if (!s) {
        return QSharedPointer<SocketLike>();
    }
    return ::qtng_bridge::fromCoreSocketLike(qtng_core::asSocketLike(UtpSocketPrivate::sharedCore(s.data())));
}

QSharedPointer<UtpSocket> convertSocketLikeToUtpSocket(QSharedPointer<SocketLike> socket)
{
    shared_ptr<qtng_core::UtpSocket> core =
            qtng_core::convertSocketLikeToUtpSocket(::qtng_bridge::toCoreSocketLike(socket));
    if (!core) {
        return QSharedPointer<UtpSocket>();
    }
    return QSharedPointer<UtpSocket>(UtpSocketPrivate::fromAdopted(core.get()));
}

KcpSocketLikeHelper::KcpSocketLikeHelper(QSharedPointer<SocketLike> socket)
    : socket(socket)
{
}

bool KcpSocketLikeHelper::isValid() const
{
    return socket && socket->isValid();
}

void KcpSocketLikeHelper::setSocket(QSharedPointer<SocketLike> s)
{
    socket = s;
}

quint32 KcpSocketLikeHelper::payloadSizeHint() const
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    return kcp ? kcp->payloadSizeHint() : 0;
}

void KcpSocketLikeHelper::setMode(KcpMode mode)
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    if (kcp) {
        kcp->setMode(mode);
    }
}

void KcpSocketLikeHelper::setSendQueueSize(quint32 sendQueueSize)
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    if (kcp) {
        kcp->setSendQueueSize(sendQueueSize);
    }
}

void KcpSocketLikeHelper::setUdpPacketSize(quint32 udpPacketSize)
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    if (kcp) {
        kcp->setUdpPacketSize(udpPacketSize);
    }
}

void KcpSocketLikeHelper::setTearDownTime(float secs)
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    if (kcp) {
        kcp->setTearDownTime(secs);
    }
}

bool KcpSocketLikeHelper::setFilter(std::function<bool(char *, qint32 *, HostAddress *, quint16 *)> callback)
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    Q_UNUSED(callback);
    return kcp != nullptr;
}

qint32 KcpSocketLikeHelper::udpSend(const char *data, qint32 size, const HostAddress &addr, quint16 port)
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    return kcp ? kcp->udpSend(data, size, addr, port) : -1;
}

QSharedPointer<SocketLike> KcpSocketLikeHelper::accept(const HostAddress &addr, quint16 port)
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    if (!kcp) {
        return QSharedPointer<SocketLike>();
    }
    return asSocketLike(QSharedPointer<KcpSocket>(kcp->accept()));
}

bool KcpSocketLikeHelper::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    return kcp && kcp->joinMulticastGroup(groupAddress, iface);
}

bool KcpSocketLikeHelper::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    QSharedPointer<KcpSocket> kcp = convertSocketLikeToKcpSocket(socket);
    return kcp && kcp->leaveMulticastGroup(groupAddress, iface);
}

bool KcpSocketLikeHelper::setOption(Socket::SocketOption option, const QVariant &value)
{
    return socket && socket->setOption(option, value);
}

QVariant KcpSocketLikeHelper::option(Socket::SocketOption option) const
{
    return socket ? socket->option(option) : QVariant();
}

QSharedPointer<SocketLike> createKcpConnection(const HostAddress &host, quint16 port, Socket::SocketError *error,
                                               int allowProtocol, KcpMode mode)
{
    return asSocketLike(QSharedPointer<KcpSocket>(KcpSocket::createConnection(host, port, error, allowProtocol, mode)));
}

QSharedPointer<SocketLike> createKcpConnection(const QString &hostName, quint16 port, Socket::SocketError *error,
                                                 QSharedPointer<SocketDnsCache> dnsCache, int allowProtocol, KcpMode mode)
{
    return asSocketLike(QSharedPointer<KcpSocket>(
            KcpSocket::createConnection(hostName, port, error, dnsCache, allowProtocol, mode)));
}

QSharedPointer<SocketLike> createKcpServer(const HostAddress &host, quint16 port, int backlog, KcpMode mode)
{
    QSharedPointer<KcpSocket> server(QSharedPointer<KcpSocket>(KcpSocket::createServer(host, port, backlog)));
    if (server) {
        server->setMode(mode);
    }
    return asSocketLike(server);
}

}  // namespace QTNETWORKNG_NAMESPACE
