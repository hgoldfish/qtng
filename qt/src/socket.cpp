#include <cstring>
#include <map>
#include <unordered_set>

#include "bridge/core_access.h"
#include "bridge/hostaddress_access.h"
#include "bridge/qt_socket_bridge.h"
#include "socket.h"
#include "network_interface.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class SocketPrivate
{
public:
    explicit SocketPrivate(shared_ptr<qtng_core::Socket> c)
        : core(std::move(c))
    {
    }
    SocketPrivate(HostAddress::NetworkLayerProtocol protocol, Socket::SocketType type)
        : core(make_shared<qtng_core::Socket>(static_cast<qtng_core::HostAddress::NetworkLayerProtocol>(protocol),
                                              static_cast<qtng_core::Socket::SocketType>(type)))
    {
    }
    explicit SocketPrivate(qintptr socketDescriptor)
        : core(make_shared<qtng_core::Socket>(static_cast<intptr_t>(socketDescriptor)))
    {
    }

    shared_ptr<qtng_core::Socket> core;

    static shared_ptr<qtng_core::Socket> &coreOf(Socket *s) { return s->d_func()->core; }
    static const shared_ptr<qtng_core::Socket> &coreOf(const Socket *s) { return s->d_func()->core; }
};

class SocketDnsCachePrivate
{
public:
    SocketDnsCachePrivate()
        : core(make_shared<qtng_core::SocketDnsCache>())
    {
    }

    shared_ptr<qtng_core::SocketDnsCache> core;

    static shared_ptr<qtng_core::SocketDnsCache> coreOf(SocketDnsCache *cache)
    {
        return cache ? cache->d_func()->core : shared_ptr<qtng_core::SocketDnsCache>();
    }
};

Socket::Socket(HostAddress::NetworkLayerProtocol protocol, SocketType type)
    : d_ptr(new SocketPrivate(protocol, type))
{
}

Socket::Socket(qintptr socketDescriptor)
    : d_ptr(new SocketPrivate(socketDescriptor))
{
}

Socket::~Socket()
{
    delete d_ptr;
}

Socket::SocketError Socket::error() const
{
    return static_cast<SocketError>(d_ptr->core->error());
}

QString Socket::errorString() const
{
    return toQString(d_ptr->core->errorString());
}

bool Socket::isValid() const
{
    return d_ptr->core->isValid();
}

HostAddress Socket::localAddress() const
{
    return toQtAddress(d_ptr->core->localAddress());
}

quint16 Socket::localPort() const
{
    return d_ptr->core->localPort();
}

HostAddress Socket::peerAddress() const
{
    return toQtAddress(d_ptr->core->peerAddress());
}

QString Socket::peerName() const
{
    return toQString(d_ptr->core->peerName());
}

quint16 Socket::peerPort() const
{
    return d_ptr->core->peerPort();
}

qintptr Socket::fileno() const
{
    return static_cast<qintptr>(d_ptr->core->fileno());
}

Socket::SocketType Socket::type() const
{
    return static_cast<SocketType>(d_ptr->core->type());
}

Socket::SocketState Socket::state() const
{
    return static_cast<SocketState>(d_ptr->core->state());
}

HostAddress::NetworkLayerProtocol Socket::protocol() const
{
    return static_cast<HostAddress::NetworkLayerProtocol>(d_ptr->core->protocol());
}

QString Socket::localAddressURI() const
{
    return toQString(d_ptr->core->localAddressURI());
}

QString Socket::peerAddressURI() const
{
    return toQString(d_ptr->core->peerAddressURI());
}

Socket *Socket::accept()
{
    qtng_core::Socket *raw = d_ptr->core->accept();
    if (!raw) {
        return nullptr;
    }
    Socket *wrapper = new Socket(HostAddress::IPv4Protocol, TcpSocket);
    wrapper->d_ptr->core.reset(raw);
    return wrapper;
}

bool Socket::bind(const HostAddress &address, quint16 port, BindMode mode)
{
    return d_ptr->core->bind(toCoreAddress(address), port, static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
}

bool Socket::bind(quint16 port, BindMode mode)
{
    return d_ptr->core->bind(port, static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
}

bool Socket::connect(const HostAddress &host, quint16 port)
{
    return d_ptr->core->connect(toCoreAddress(host), port);
}

bool Socket::connect(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache)
{
    return d_ptr->core->connect(toStdString(hostName), port, SocketDnsCachePrivate::coreOf(dnsCache.data()));
}

void Socket::close()
{
    d_ptr->core->close();
}

void Socket::abort()
{
    d_ptr->core->abort();
}

bool Socket::listen(int backlog)
{
    return d_ptr->core->listen(backlog);
}

bool Socket::setTcpKeepalive(bool keepalve, int keepaliveTimeoutSesc, int keepaliveIntervalSesc)
{
    return d_ptr->core->setTcpKeepalive(keepalve, keepaliveTimeoutSesc, keepaliveIntervalSesc);
}

bool Socket::setOption(SocketOption option, const QVariant &value)
{
    return d_ptr->core->setOption(static_cast<qtng_core::Socket::SocketOption>(option), value.toInt());
}

QVariant Socket::option(SocketOption option) const
{
    return d_ptr->core->option(static_cast<qtng_core::Socket::SocketOption>(option));
}

bool Socket::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->core->joinMulticastGroup(toCoreAddress(groupAddress), toCoreInterface(iface));
}

bool Socket::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->core->leaveMulticastGroup(toCoreAddress(groupAddress), toCoreInterface(iface));
}

NetworkInterface Socket::multicastInterface() const
{
    return toQtInterface(d_ptr->core->multicastInterface());
}

bool Socket::setMulticastInterface(const NetworkInterface &iface)
{
    return d_ptr->core->setMulticastInterface(toCoreInterface(iface));
}

qint32 Socket::peek(char *data, qint32 size)
{
    return d_ptr->core->peek(data, size);
}

qint32 Socket::recv(char *data, qint32 size)
{
    return d_ptr->core->recv(data, size);
}

qint32 Socket::recvall(char *data, qint32 size)
{
    return d_ptr->core->recvall(data, size);
}

qint32 Socket::send(const char *data, qint32 size)
{
    return d_ptr->core->send(data, size);
}

qint32 Socket::sendall(const char *data, qint32 size)
{
    return d_ptr->core->sendall(data, size);
}

qint32 Socket::recvfrom(char *data, qint32 size, HostAddress *addr, quint16 *port)
{
    qtng_core::HostAddress coreAddr;
    qint32 n = d_ptr->core->recvfrom(data, size, addr ? &coreAddr : nullptr, port);
    if (addr && n >= 0) {
        *addr = toQtAddress(coreAddr);
    }
    return n;
}

qint32 Socket::sendto(const char *data, qint32 size, const HostAddress &addr, quint16 port)
{
    return d_ptr->core->sendto(data, size, toCoreAddress(addr), port);
}

QByteArray Socket::recvall(qint32 size)
{
    return toQByteArray(d_ptr->core->recvall(size));
}

QByteArray Socket::recv(qint32 size)
{
    return toQByteArray(d_ptr->core->recv(size));
}

qint32 Socket::send(const QByteArray &data)
{
    return d_ptr->core->send(toStdString(data));
}

qint32 Socket::sendall(const QByteArray &data)
{
    return d_ptr->core->sendall(toStdString(data));
}

QByteArray Socket::recvfrom(qint32 size, HostAddress *addr, quint16 *port)
{
    qtng_core::HostAddress coreAddr;
    string data = d_ptr->core->recvfrom(size, addr ? &coreAddr : nullptr, port);
    if (addr) {
        *addr = toQtAddress(coreAddr);
    }
    return toQByteArray(data);
}

qint32 Socket::sendto(const QByteArray &data, const HostAddress &addr, quint16 port)
{
    return d_ptr->core->sendto(toStdString(data), toCoreAddress(addr), port);
}

QList<HostAddress> Socket::resolve(const QString &hostName)
{
    const vector<qtng_core::HostAddress> addrs = qtng_core::Socket::resolve(toStdString(hostName));
    QList<HostAddress> result;
    result.reserve(static_cast<int>(addrs.size()));
    for (const qtng_core::HostAddress &a : addrs) {
        result.append(toQtAddress(a));
    }
    return result;
}

Socket *Socket::createConnection(const HostAddress &host, quint16 port, Socket::SocketError *error, int allowProtocol)
{
    qtng_core::Socket::SocketError coreError = qtng_core::Socket::NoError;
    qtng_core::Socket *raw =
            qtng_core::Socket::createConnection(toCoreAddress(host), port, error ? &coreError : nullptr, allowProtocol);
    if (error) {
        *error = static_cast<SocketError>(coreError);
    }
    if (!raw) {
        return nullptr;
    }
    Socket *wrapper = new Socket(HostAddress::IPv4Protocol, TcpSocket);
    wrapper->d_ptr->core.reset(raw);
    return wrapper;
}

Socket *Socket::createConnection(const QString &hostName, quint16 port, Socket::SocketError *error,
                                 QSharedPointer<SocketDnsCache> dnsCache, int allowProtocol)
{
    qtng_core::Socket::SocketError coreError = qtng_core::Socket::NoError;
    qtng_core::Socket *raw =
            qtng_core::Socket::createConnection(toStdString(hostName), port, error ? &coreError : nullptr,
                                               SocketDnsCachePrivate::coreOf(dnsCache.data()), allowProtocol);
    if (error) {
        *error = static_cast<SocketError>(coreError);
    }
    if (!raw) {
        return nullptr;
    }
    Socket *wrapper = new Socket(HostAddress::IPv4Protocol, TcpSocket);
    wrapper->d_ptr->core.reset(raw);
    return wrapper;
}

Socket *Socket::createServer(const HostAddress &host, quint16 port, int backlog)
{
    qtng_core::Socket *raw = qtng_core::Socket::createServer(toCoreAddress(host), port, backlog);
    if (!raw) {
        return nullptr;
    }
    Socket *wrapper = new Socket(HostAddress::IPv4Protocol, TcpSocket);
    wrapper->d_ptr->core.reset(raw);
    return wrapper;
}

// ---- Poll ----

class PollPrivate
{
public:
    PollPrivate() = default;
    ~PollPrivate() = default;

    void add(QSharedPointer<Socket> socket, Poll::EventType event)
    {
        shared_ptr<qtng_core::Socket> coreSock = SocketPrivate::coreOf(socket.data());
        // Keep Qt socket alive for the lifetime of the watcher.
        qtToCore[socket] = coreSock;
        coreToQt[coreSock.get()] = socket;
        core.add(coreSock, static_cast<qtng_core::Poll::EventType>(event));
    }

    void remove(QSharedPointer<Socket> socket)
    {
        auto it = qtToCore.find(socket);
        if (it == qtToCore.end()) {
            return;
        }
        core.remove(it.value());
        coreToQt.remove(it.value().get());
        qtToCore.erase(it);
    }

    QSharedPointer<Socket> wait(float msecs)
    {
        shared_ptr<qtng_core::Socket> ready = core.wait(msecs);
        if (!ready) {
            return QSharedPointer<Socket>();
        }
        return coreToQt.value(ready.get());
    }

    qtng_core::Poll core;
    QHash<QSharedPointer<Socket>, shared_ptr<qtng_core::Socket>> qtToCore;
    QHash<qtng_core::Socket *, QSharedPointer<Socket>> coreToQt;
};

Poll::Poll()
    : d_ptr(new PollPrivate)
{
}

Poll::~Poll()
{
    delete d_ptr;
}

void Poll::add(QSharedPointer<Socket> socket, EventType event)
{
    Q_D(Poll);
    d->add(socket, event);
}

void Poll::remove(QSharedPointer<Socket> socket)
{
    Q_D(Poll);
    d->remove(socket);
}

QSharedPointer<Socket> Poll::wait(float msecs)
{
    Q_D(Poll);
    return d->wait(msecs);
}

// ---- SocketDnsCache ----

SocketDnsCache::SocketDnsCache()
    : d_ptr(new SocketDnsCachePrivate)
{
}

SocketDnsCache::~SocketDnsCache()
{
    delete d_ptr;
}

QList<HostAddress> SocketDnsCache::resolve(const QString &hostName)
{
    Q_D(SocketDnsCache);
    const vector<qtng_core::HostAddress> addrs = d->core->resolve(toStdString(hostName));
    QList<HostAddress> result;
    result.reserve(static_cast<int>(addrs.size()));
    for (const qtng_core::HostAddress &a : addrs) {
        result.append(toQtAddress(a));
    }
    return result;
}

bool SocketDnsCache::hasHost(const QString &hostName) const
{
    Q_D(const SocketDnsCache);
    return d->core->hasHost(toStdString(hostName));
}

void SocketDnsCache::addHost(const QString &hostName, const QList<HostAddress> &addrList)
{
    Q_D(SocketDnsCache);
    vector<qtng_core::HostAddress> coreAddrs;
    coreAddrs.reserve(static_cast<size_t>(addrList.size()));
    for (const HostAddress &a : addrList) {
        coreAddrs.push_back(toCoreAddress(a));
    }
    d->core->addHost(toStdString(hostName), coreAddrs);
}

void SocketDnsCache::addHost(const QString &hostName, const HostAddress &addr)
{
    Q_D(SocketDnsCache);
    d->core->addHost(toStdString(hostName), toCoreAddress(addr));
}

quint64 SocketDnsCache::timeToLive() const
{
    Q_D(const SocketDnsCache);
    return d->core->timeToLive();
}

void SocketDnsCache::setTimeToLive(quint64 msecs)
{
    Q_D(SocketDnsCache);
    d->core->setTimeToLive(msecs);
}

}  // namespace QTNETWORKNG_NAMESPACE

#include "bridge/socket_access.h"

namespace qtng_bridge {

std::shared_ptr<qtng_core::Socket> socketCoreOf(QTNETWORKNG_NAMESPACE::Socket *socket)
{
    if (!socket) {
        return std::shared_ptr<qtng_core::Socket>();
    }
    return QTNETWORKNG_NAMESPACE::SocketPrivate::coreOf(socket);
}

void assignSocketCore(QTNETWORKNG_NAMESPACE::Socket *socket, std::shared_ptr<qtng_core::Socket> core)
{
    if (socket) {
        QTNETWORKNG_NAMESPACE::SocketPrivate::coreOf(socket) = std::move(core);
    }
}

std::shared_ptr<qtng_core::SocketDnsCache> dnsCacheCoreOf(QTNETWORKNG_NAMESPACE::SocketDnsCache *cache)
{
    if (!cache) {
        return std::shared_ptr<qtng_core::SocketDnsCache>();
    }
    return QTNETWORKNG_NAMESPACE::SocketDnsCachePrivate::coreOf(cache);
}

QSharedPointer<QTNETWORKNG_NAMESPACE::SocketDnsCache>
dnsCacheFromCore(const std::shared_ptr<qtng_core::SocketDnsCache> &core)
{
    if (!core) {
        return QSharedPointer<QTNETWORKNG_NAMESPACE::SocketDnsCache>();
    }
    QSharedPointer<QTNETWORKNG_NAMESPACE::SocketDnsCache> cache =
            QSharedPointer<QTNETWORKNG_NAMESPACE::SocketDnsCache>::create();
    QTNETWORKNG_NAMESPACE::SocketDnsCachePrivate::coreOf(cache.data()) = core;
    return cache;
}

}  // namespace qtng_bridge
