#include "qtng/udp.h"

#include <functional>
#include <memory>
#include <vector>

#include "qtng/private/kcp.h"
#include "qtng/private/socket_p.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

namespace {

// UDP-specific peer path: HostAddress + port, convertible to opaque DatagramPath.
class UdpDatagramPath
{
public:
    UdpDatagramPath();
    UdpDatagramPath(const HostAddress &addr, uint16_t port);
    explicit UdpDatagramPath(const DatagramPath &path);

    DatagramPath toPath() const;
    HostAddress address() const { return m_addr; }
    uint16_t port() const { return m_port; }
    bool isNull() const;
private:
    HostAddress m_addr;
    uint16_t m_port;
};

UdpDatagramPath::UdpDatagramPath()
    : m_port(0)
{
}

UdpDatagramPath::UdpDatagramPath(const HostAddress &addr, uint16_t port)
    : m_addr(addr)
    , m_port(port)
{
}

UdpDatagramPath::UdpDatagramPath(const DatagramPath &path)
    : m_port(0)
{
    const string &key = path.key();
    if (key.empty()) {
        return;
    }
    size_t pos = key.rfind(':');
    if (pos == string::npos || pos == 0) {
        return;
    }
    string host = key.substr(0, pos);
    string portStr = key.substr(pos + 1);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    unsigned long p = 0;
    try {
        size_t idx = 0;
        p = stoul(portStr, &idx);
        if (idx != portStr.size() || p > 65535) {
            return;
        }
    } catch (...) {
        return;
    }
    if (m_addr.setAddress(host)) {
        m_port = static_cast<uint16_t>(p);
    }
}

DatagramPath UdpDatagramPath::toPath() const
{
    if (isNull()) {
        return DatagramPath();
    }
    string key;
    if (m_addr.protocol() == HostAddress::IPv6Protocol) {
        key = "[" + m_addr.toString() + "]:" + utils::number(m_port);
    } else {
        key = m_addr.toString() + ":" + utils::number(m_port);
    }
    return DatagramPath(key);
}

bool UdpDatagramPath::isNull() const
{
    return m_addr.isNull() || m_port == 0;
}

class UdpDatagramLink : public DatagramLink
{
public:
    explicit UdpDatagramLink(HostAddress::NetworkLayerProtocol protocol);
    explicit UdpDatagramLink(intptr_t socketDescriptor);
    explicit UdpDatagramLink(shared_ptr<Socket> rawSocket);
    ~UdpDatagramLink() override;

    shared_ptr<Socket> socket() const;

    bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode);
    bool bind(uint16_t port, Socket::BindMode mode);
    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface);
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface);
    NetworkInterface multicastInterface() const;
    bool setMulticastInterface(const NetworkInterface &iface);
    void setFilter(function<bool(char *, int32_t *, HostAddress *, uint16_t *)> callback);

    int32_t recvfrom(char *data, int32_t size, DatagramPath *who) override;
    int32_t sendto(const char *data, int32_t size, const DatagramPath &who) override;
    void close() override;
    void abort() override;
    bool isValid() const override;
    Socket::SocketError error() const override;
    string errorString() const override;

    HostAddress localAddress() const;
    uint16_t localPort() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    int32_t peek(char *data, int32_t size);
    bool setOption(Socket::SocketOption option, int value);
    int option(Socket::SocketOption option) const;
private:
    shared_ptr<Socket> rawSocket;
    function<bool(char *, int32_t *, HostAddress *, uint16_t *)> filterCallback;
};

UdpDatagramLink::UdpDatagramLink(HostAddress::NetworkLayerProtocol protocol)
    : rawSocket(new Socket(protocol, Socket::UdpSocket))
{
}

UdpDatagramLink::UdpDatagramLink(intptr_t socketDescriptor)
    : rawSocket(new Socket(socketDescriptor))
{
}

UdpDatagramLink::UdpDatagramLink(shared_ptr<Socket> s)
    : rawSocket(std::move(s))
{
}

UdpDatagramLink::~UdpDatagramLink() {}

shared_ptr<Socket> UdpDatagramLink::socket() const
{
    return rawSocket;
}

bool UdpDatagramLink::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    if (mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    return rawSocket->bind(address, port, mode);
}

bool UdpDatagramLink::bind(uint16_t port, Socket::BindMode mode)
{
    if (mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    return rawSocket->bind(port, mode);
}

bool UdpDatagramLink::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return rawSocket->joinMulticastGroup(groupAddress, iface);
}

bool UdpDatagramLink::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return rawSocket->leaveMulticastGroup(groupAddress, iface);
}

NetworkInterface UdpDatagramLink::multicastInterface() const
{
    return rawSocket->multicastInterface();
}

bool UdpDatagramLink::setMulticastInterface(const NetworkInterface &iface)
{
    return rawSocket->setMulticastInterface(iface);
}

void UdpDatagramLink::setFilter(function<bool(char *, int32_t *, HostAddress *, uint16_t *)> callback)
{
    filterCallback = std::move(callback);
}

int32_t UdpDatagramLink::recvfrom(char *data, int32_t size, DatagramPath *who)
{
    HostAddress addr;
    uint16_t port = 0;
    int32_t len = rawSocket->recvfrom(data, size, &addr, &port);
    if (len < 0) {
        return len;
    }
    if (filterCallback) {
        int32_t filteredLen = len;
        if (filterCallback(data, &filteredLen, &addr, &port)) {
            return 0;
        }
        len = filteredLen;
    }
    if (who) {
        *who = UdpDatagramPath(addr, port).toPath();
    }
    return len;
}

int32_t UdpDatagramLink::sendto(const char *data, int32_t size, const DatagramPath &who)
{
    const UdpDatagramPath udp(who);
    if (udp.isNull()) {
        return -1;
    }
    return rawSocket->sendto(data, size, udp.address(), udp.port());
}

void UdpDatagramLink::close()
{
    rawSocket->close();
}

void UdpDatagramLink::abort()
{
    rawSocket->abort();
}

bool UdpDatagramLink::isValid() const
{
    return rawSocket && rawSocket->isValid();
}

Socket::SocketError UdpDatagramLink::error() const
{
    return rawSocket->error();
}

string UdpDatagramLink::errorString() const
{
    return rawSocket->errorString();
}

HostAddress UdpDatagramLink::localAddress() const
{
    return rawSocket->localAddress();
}

uint16_t UdpDatagramLink::localPort() const
{
    return rawSocket->localPort();
}

HostAddress::NetworkLayerProtocol UdpDatagramLink::protocol() const
{
    return rawSocket->protocol();
}

int32_t UdpDatagramLink::peek(char *data, int32_t size)
{
    return rawSocket->peek(data, size);
}

bool UdpDatagramLink::setOption(Socket::SocketOption option, int value)
{
    return rawSocket->setOption(option, value);
}

int UdpDatagramLink::option(Socket::SocketOption option) const
{
    return rawSocket->option(option);
}

}  // namespace

class KcpSocketPrivate
{
public:
    KcpSocketPrivate(shared_ptr<DatagramLink> link, shared_ptr<UdpDatagramLink> udp, shared_ptr<KcpStream> stream)
        : link(std::move(link))
        , udp(std::move(udp))
        , stream(std::move(stream))
    {
    }

    shared_ptr<DatagramLink> link;
    shared_ptr<UdpDatagramLink> udp;  // may be null for non-UDP DatagramLink
    shared_ptr<KcpStream> stream;
};

static void installFilter(KcpSocket *socket, UdpDatagramLink *udp)
{
    if (!udp) {
        return;
    }
    udp->setFilter([socket](char *data, int32_t *len, HostAddress *addr, uint16_t *port) {
        return socket->filter(data, len, addr, port);
    });
}

static KcpSocketPrivate *makePrivateRaw(shared_ptr<UdpDatagramLink> udp)
{
    shared_ptr<KcpStream> stream(new KcpStream(udp));
    return new KcpSocketPrivate(udp, udp, stream);
}

static KcpStream::Mode toStreamMode(KcpSocket::Mode mode)
{
    return static_cast<KcpStream::Mode>(mode);
}

static KcpSocket::Mode toSocketMode(KcpStream::Mode mode)
{
    return static_cast<KcpSocket::Mode>(mode);
}

KcpSocket::KcpSocket(HostAddress::NetworkLayerProtocol protocol)
    : d_ptr(makePrivateRaw(make_shared<UdpDatagramLink>(protocol)))
{
    installFilter(this, d_ptr->udp.get());
}

KcpSocket::KcpSocket(intptr_t socketDescriptor)
    : d_ptr(makePrivateRaw(make_shared<UdpDatagramLink>(socketDescriptor)))
{
    installFilter(this, d_ptr->udp.get());
}

KcpSocket::KcpSocket(shared_ptr<Socket> rawSocket)
    : d_ptr(makePrivateRaw(make_shared<UdpDatagramLink>(rawSocket)))
{
    installFilter(this, d_ptr->udp.get());
}

KcpSocket::KcpSocket(shared_ptr<KcpStream> stream)
    : d_ptr(new KcpSocketPrivate(stream->link(), dynamic_pointer_cast<UdpDatagramLink>(stream->link()), stream))
{
    if (d_ptr->udp) {
        installFilter(this, d_ptr->udp.get());
    }
}

KcpSocket *wrapKcpStreamAsSocket(shared_ptr<KcpStream> stream)
{
    if (!stream) {
        return nullptr;
    }
    return new KcpSocket(std::move(stream));
}

KcpSocket::~KcpSocket()
{
    delete d_ptr;
}

void KcpSocket::setMode(Mode mode)
{
    d_ptr->stream->setMode(toStreamMode(mode));
}

KcpSocket::Mode KcpSocket::mode() const
{
    return toSocketMode(d_ptr->stream->mode());
}

void KcpSocket::setUdpPacketSize(uint32_t udpPacketSize)
{
    d_ptr->stream->setPacketSize(udpPacketSize);
}

uint32_t KcpSocket::udpPacketSize() const
{
    return d_ptr->stream->packetSize();
}

void KcpSocket::setSendQueueSize(uint32_t sendQueueSize)
{
    d_ptr->stream->setSendQueueSize(sendQueueSize);
}

uint32_t KcpSocket::sendQueueSize() const
{
    return d_ptr->stream->sendQueueSize();
}

uint32_t KcpSocket::payloadSizeHint() const
{
    return d_ptr->stream->payloadSizeHint();
}

void KcpSocket::setTearDownTime(float secs)
{
    d_ptr->stream->setTearDownTime(secs);
}

float KcpSocket::tearDownTime() const
{
    return d_ptr->stream->tearDownTime();
}

Socket::SocketError KcpSocket::error() const
{
    return d_ptr->stream->error();
}

string KcpSocket::errorString() const
{
    return d_ptr->stream->errorString();
}

bool KcpSocket::isValid() const
{
    return d_ptr->stream->isValid();
}

HostAddress KcpSocket::localAddress() const
{
    return d_ptr->udp ? d_ptr->udp->localAddress() : HostAddress();
}

uint16_t KcpSocket::localPort() const
{
    return d_ptr->udp ? d_ptr->udp->localPort() : 0;
}

HostAddress KcpSocket::peerAddress() const
{
    return UdpDatagramPath(d_ptr->stream->peerPath()).address();
}

string KcpSocket::peerName() const
{
    return UdpDatagramPath(d_ptr->stream->peerPath()).address().toString();
}

uint16_t KcpSocket::peerPort() const
{
    return UdpDatagramPath(d_ptr->stream->peerPath()).port();
}

Socket::SocketType KcpSocket::type() const
{
    return Socket::KcpSocket;
}

Socket::SocketState KcpSocket::state() const
{
    return d_ptr->stream->state();
}

HostAddress::NetworkLayerProtocol KcpSocket::protocol() const
{
    return d_ptr->udp ? d_ptr->udp->protocol() : HostAddress::UnknownNetworkLayerProtocol;
}

string KcpSocket::localAddressURI() const
{
    const HostAddress &addr = localAddress();
    string host = (addr.protocol() == HostAddress::IPv6Protocol)
            ? utils::formatMessage("[%1]", {addr.toString()})
            : addr.toString();
    return utils::formatMessage("%1:%2", {host, utils::number(localPort())});
}

string KcpSocket::peerAddressURI() const
{
    const HostAddress &addr = peerAddress();
    string host = (addr.protocol() == HostAddress::IPv6Protocol)
            ? utils::formatMessage("[%1]", {addr.toString()})
            : addr.toString();
    return utils::formatMessage("%1:%2", {host, utils::number(peerPort())});
}

KcpSocket *KcpSocket::accept()
{
    return wrapKcpStreamAsSocket(shared_ptr<KcpStream>(d_ptr->stream->accept()));
}

KcpSocket *KcpSocket::accept(const HostAddress &addr, uint16_t port)
{
    return wrapKcpStreamAsSocket(
            shared_ptr<KcpStream>(d_ptr->stream->accept(UdpDatagramPath(addr, port).toPath())));
}

KcpSocket *KcpSocket::accept(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    vector<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.push_back(t);
    } else if (!dnsCache) {
        addresses = Socket::resolve(hostName);
    } else {
        addresses = dnsCache->resolve(hostName);
    }
    const HostAddress::NetworkLayerProtocol prefer = protocol();
    for (const HostAddress &addr : addresses) {
        if (prefer == HostAddress::IPv4Protocol && addr.protocol() == HostAddress::IPv6Protocol) {
            continue;
        }
        if (prefer == HostAddress::IPv6Protocol && addr.protocol() == HostAddress::IPv4Protocol) {
            continue;
        }
        return accept(addr, port);
    }
    return nullptr;
}

bool KcpSocket::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    if (!d_ptr->udp || !d_ptr->udp->bind(address, port, mode)) {
        return false;
    }
    return d_ptr->stream->markBound();
}

bool KcpSocket::bind(uint16_t port, Socket::BindMode mode)
{
    if (!d_ptr->udp || !d_ptr->udp->bind(port, mode)) {
        return false;
    }
    return d_ptr->stream->markBound();
}

bool KcpSocket::connect(const HostAddress &addr, uint16_t port)
{
    return d_ptr->stream->connect(UdpDatagramPath(addr, port).toPath());
}

bool KcpSocket::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    vector<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.push_back(t);
    } else if (!dnsCache) {
        addresses = Socket::resolve(hostName);
    } else {
        addresses = dnsCache->resolve(hostName);
    }
    const HostAddress::NetworkLayerProtocol prefer = protocol();
    for (const HostAddress &addr : addresses) {
        if (prefer == HostAddress::IPv4Protocol && addr.protocol() == HostAddress::IPv6Protocol) {
            continue;
        }
        if (prefer == HostAddress::IPv6Protocol && addr.protocol() == HostAddress::IPv4Protocol) {
            continue;
        }
        if (connect(addr, port)) {
            return true;
        }
    }
    return false;
}

void KcpSocket::close()
{
    d_ptr->stream->close();
}

void KcpSocket::abort()
{
    d_ptr->stream->abort();
}

bool KcpSocket::listen(int backlog)
{
    return d_ptr->stream->listen(backlog);
}

bool KcpSocket::setOption(Socket::SocketOption option, int value)
{
    return d_ptr->udp ? d_ptr->udp->setOption(option, value) : false;
}

int KcpSocket::option(Socket::SocketOption option) const
{
    return d_ptr->udp ? d_ptr->udp->option(option) : -1;
}

bool KcpSocket::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->udp ? d_ptr->udp->joinMulticastGroup(groupAddress, iface) : false;
}

bool KcpSocket::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->udp ? d_ptr->udp->leaveMulticastGroup(groupAddress, iface) : false;
}

NetworkInterface KcpSocket::multicastInterface() const
{
    return d_ptr->udp ? d_ptr->udp->multicastInterface() : NetworkInterface();
}

bool KcpSocket::setMulticastInterface(const NetworkInterface &iface)
{
    return d_ptr->udp ? d_ptr->udp->setMulticastInterface(iface) : false;
}

int32_t KcpSocket::peek(char *data, int32_t size)
{
    return d_ptr->stream->peek(data, size);
}

int32_t KcpSocket::peekRaw(char *data, int32_t size)
{
    return d_ptr->udp ? d_ptr->udp->peek(data, size) : -1;
}

int32_t KcpSocket::recv(char *data, int32_t size)
{
    return d_ptr->stream->recv(data, size);
}

int32_t KcpSocket::recvall(char *data, int32_t size)
{
    return d_ptr->stream->recvall(data, size);
}

int32_t KcpSocket::send(const char *data, int32_t size)
{
    return d_ptr->stream->send(data, size);
}

int32_t KcpSocket::sendall(const char *data, int32_t size)
{
    return d_ptr->stream->sendall(data, size);
}

string KcpSocket::recv(int32_t size)
{
    return d_ptr->stream->recv(size);
}

string KcpSocket::recvall(int32_t size)
{
    return d_ptr->stream->recvall(size);
}

int32_t KcpSocket::send(const string &data)
{
    return d_ptr->stream->send(data);
}

int32_t KcpSocket::sendall(const string &data)
{
    return d_ptr->stream->sendall(data);
}

bool KcpSocket::filter(char *data, int32_t *len, HostAddress *addr, uint16_t *port)
{
    (void) data;
    (void) len;
    (void) addr;
    (void) port;
    return false;
}

int32_t KcpSocket::udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port)
{
    return d_ptr->udp ? d_ptr->udp->sendto(data, size, UdpDatagramPath(addr, port).toPath()) : -1;
}

KcpSocket *KcpSocket::createConnection(const HostAddress &host, uint16_t port, Socket::SocketError *error,
                                       int allowProtocol)
{
    return qtng::createConnection<KcpSocket>(host, port, error, allowProtocol, MakeSocketType<KcpSocket>);
}

KcpSocket *KcpSocket::createConnection(const string &hostName, uint16_t port, Socket::SocketError *error,
                                       shared_ptr<SocketDnsCache> dnsCache, int allowProtocol)
{
    return qtng::createConnection<KcpSocket>(hostName, port, error, dnsCache, allowProtocol, MakeSocketType<KcpSocket>);
}

KcpSocket *KcpSocket::createServer(const HostAddress &host, uint16_t port, int backlog)
{
    return qtng::createServer<KcpSocket>(host, port, backlog, MakeSocketType<KcpSocket>);
}

class KcpSocketLikeImpl : public SocketLike
{
public:
    KcpSocketLikeImpl(shared_ptr<KcpSocket> s)
        : s(s)
    {
    }

    virtual Socket::SocketError error() const override { return s->error(); }
    virtual string errorString() const override { return s->errorString(); }
    virtual bool isValid() const override { return s->isValid(); }
    virtual HostAddress localAddress() const override { return s->localAddress(); }
    virtual uint16_t localPort() const override { return s->localPort(); }
    virtual HostAddress peerAddress() const override { return s->peerAddress(); }
    virtual string peerName() const override { return s->peerName(); }
    virtual uint16_t peerPort() const override { return s->peerPort(); }
    virtual intptr_t fileno() const override { return -1; }
    virtual Socket::SocketType type() const override { return s->type(); }
    virtual Socket::SocketState state() const override { return s->state(); }
    virtual HostAddress::NetworkLayerProtocol protocol() const override { return s->protocol(); }
    virtual string localAddressURI() const override { return s->localAddressURI(); }
    virtual string peerAddressURI() const override { return s->peerAddressURI(); }
    virtual Socket *acceptRaw() override { return nullptr; }
    virtual shared_ptr<SocketLike> accept() override { return asSocketLike(s->accept()); }
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override
    {
        return s->bind(address, port, mode);
    }
    virtual bool bind(uint16_t port, Socket::BindMode mode) override { return s->bind(port, mode); }
    virtual bool connect(const HostAddress &addr, uint16_t port) override { return s->connect(addr, port); }
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override
    {
        return s->connect(hostName, port, dnsCache);
    }
    virtual void close() override { s->close(); }
    virtual void abort() override { s->abort(); }
    virtual bool listen(int backlog) override { return s->listen(backlog); }
    virtual bool setOption(Socket::SocketOption option, int value) override { return s->setOption(option, value); }
    virtual int option(Socket::SocketOption option) const override { return s->option(option); }
    virtual int32_t peek(char *data, int32_t size) override { return s->peek(data, size); }
    virtual int32_t peekRaw(char *data, int32_t size) override { return s->peekRaw(data, size); }
    virtual int32_t recv(char *data, int32_t size) override { return s->recv(data, size); }
    virtual int32_t recvall(char *data, int32_t size) override { return s->recvall(data, size); }
    virtual int32_t send(const char *data, int32_t size) override { return s->send(data, size); }
    virtual int32_t sendall(const char *data, int32_t size) override { return s->sendall(data, size); }
    virtual string recv(int32_t size) override { return s->recv(size); }
    virtual string recvall(int32_t size) override { return s->recvall(size); }
    virtual int32_t send(const string &data) override { return s->send(data); }
    virtual int32_t sendall(const string &data) override { return s->sendall(data); }

    shared_ptr<KcpSocket> s;
};

shared_ptr<SocketLike> asSocketLike(shared_ptr<KcpSocket> s)
{
    if (!s) {
        return shared_ptr<SocketLike>();
    }
    return make_shared<KcpSocketLikeImpl>(s);
}

shared_ptr<KcpSocket> convertSocketLikeToKcpSocket(shared_ptr<SocketLike> socket)
{
    shared_ptr<KcpSocketLikeImpl> impl = dynamic_pointer_cast<KcpSocketLikeImpl>(socket);
    if (impl) {
        return impl->s;
    }
    return shared_ptr<KcpSocket>();
}

}  // namespace qtng
