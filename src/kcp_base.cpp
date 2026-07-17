#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kcp_base_p.h"
#include "qtng/kcp_base.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

class SinglePathUdpLinkId
{
public:
    SinglePathUdpLinkId();

    bool operator==(const SinglePathUdpLinkId &other) const;
    bool operator<(const SinglePathUdpLinkId &other) const;
    bool isNull() const;
    string toString() const;
public:
    HostAddress addr;
    uint16_t port;
};

ostream &operator<<(ostream &out, const SinglePathUdpLinkId &t)
{
    return out << t.toString();
}

class SinglePathUdpLinkManager
{
public:
    typedef SinglePathUdpLinkId PathID;
    SinglePathUdpLinkManager(HostAddress::NetworkLayerProtocol protocol);
    SinglePathUdpLinkManager(intptr_t socketDescriptor);
    SinglePathUdpLinkManager(shared_ptr<Socket> rawSocket);
    ~SinglePathUdpLinkManager();
public:
    // template
    int32_t recvfrom(char *data, int32_t size, SinglePathUdpLinkId &who);
    int32_t sendto(const char *data, int32_t size, const SinglePathUdpLinkId &who);
    bool filter(char *data, int32_t *size, SinglePathUdpLinkId *who);
    void close();
    void abort();
    void closeSlave(const SinglePathUdpLinkId &who);
    void abortSlave(const SinglePathUdpLinkId &who);
    bool addSlave(const SinglePathUdpLinkId &who, uint32_t connectionId) { return true; };
public:
    shared_ptr<Socket> rawSocket;
    function<bool(char *data, int32_t *size, HostAddress *add, uint16_t *port)> filterCallback;
};

SinglePathUdpLinkId::SinglePathUdpLinkId()
    : port(0)
{
}

bool SinglePathUdpLinkId::operator==(const SinglePathUdpLinkId &other) const
{
    return addr == other.addr && port == other.port;
}

bool SinglePathUdpLinkId::operator<(const SinglePathUdpLinkId &other) const
{
    HostAddress::NetworkLayerProtocol a = addr.protocol();
    HostAddress::NetworkLayerProtocol b = other.addr.protocol();
    if (a != b) {
        return a < b;
    }
    switch (a) {
    case HostAddress::IPv4Protocol: {
        IPv4Address A = addr.toIPv4Address();
        IPv4Address B = other.addr.toIPv4Address();
        if (A != B) {
            return A < B;
        }
        break;
    }
    case HostAddress::IPv6Protocol: {
        IPv6Address A = addr.toIPv6Address();
        IPv6Address B = other.addr.toIPv6Address();
        int result = memcmp(A.c, B.c, sizeof(IPv6Address));
        if (result != 0) {
            return result < 0;
        }
        break;
    }
    default:
        break;
    }
    return port < other.port;
}

bool SinglePathUdpLinkId::isNull() const
{
    return port == 0 || addr.isNull();
}

string SinglePathUdpLinkId::toString() const
{
    return addr.toString() + ":" + utils::number(port);
}

SinglePathUdpLinkManager::SinglePathUdpLinkManager(HostAddress::NetworkLayerProtocol protocol)
    : rawSocket(new Socket(protocol, Socket::UdpSocket))
{
}

SinglePathUdpLinkManager::SinglePathUdpLinkManager(shared_ptr<Socket> rawSocket)
    : rawSocket(rawSocket)
{
}

SinglePathUdpLinkManager::SinglePathUdpLinkManager(intptr_t socketDescriptor)
    : rawSocket(new Socket(socketDescriptor))
{
}

SinglePathUdpLinkManager::~SinglePathUdpLinkManager() { }

int32_t SinglePathUdpLinkManager::recvfrom(char *data, int32_t size, SinglePathUdpLinkId &who)
{
    return rawSocket->recvfrom(data, size, &who.addr, &who.port);
}

int32_t SinglePathUdpLinkManager::sendto(const char *data, int32_t size, const SinglePathUdpLinkId &who)
{
    return rawSocket->sendto(data, size, who.addr, who.port);
}

bool SinglePathUdpLinkManager::filter(char *data, int32_t *size, SinglePathUdpLinkId *who)
{
    if (filterCallback) {
        return filterCallback(data, size, &(who->addr), &(who->port));
    }
    return false;
}

void SinglePathUdpLinkManager::close()
{
    rawSocket->close();
}

void SinglePathUdpLinkManager::abort()
{
    rawSocket->abort();
}

void SinglePathUdpLinkManager::closeSlave(const SinglePathUdpLinkId &who) { }

void SinglePathUdpLinkManager::abortSlave(const SinglePathUdpLinkId &who) { }

class SinglePathUdpLinkSocketLike : public KcpBaseSocketLike<SinglePathUdpLinkManager>
{
public:
    SinglePathUdpLinkSocketLike(HostAddress::NetworkLayerProtocol protocol);
    SinglePathUdpLinkSocketLike(intptr_t socketDescriptor);
    SinglePathUdpLinkSocketLike(shared_ptr<Socket> rawSocket);
    ~SinglePathUdpLinkSocketLike();
protected:
    // interval
    SinglePathUdpLinkSocketLike(shared_ptr<KcpBase<SinglePathUdpLinkManager>> slave);
public:
    virtual Socket::SocketError error() const override;
    virtual string errorString() const override;
    virtual bool isValid() const override;
    virtual HostAddress localAddress() const override;
    virtual uint16_t localPort() const override;
    virtual HostAddress peerAddress() const override;
    virtual uint16_t peerPort() const override;
    virtual string peerName() const override;
    virtual intptr_t fileno() const override;
    virtual HostAddress::NetworkLayerProtocol protocol() const override;
    virtual string localAddressURI() const override;
    virtual string peerAddressURI() const override;
    virtual shared_ptr<SocketLike> accept() override;
    virtual bool bind(const HostAddress &address, uint16_t port,
                      Socket::BindMode mode = Socket::DefaultForPlatform) override;
    virtual bool bind(uint16_t port, Socket::BindMode mode = Socket::DefaultForPlatform) override;
    virtual bool connect(const HostAddress &addr, uint16_t port) override;
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;

    virtual bool setOption(Socket::SocketOption option, int value) override;
    virtual int option(Socket::SocketOption option) const override;
public:
    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    NetworkInterface multicastInterface() const;
    bool setMulticastInterface(const NetworkInterface &iface);
    bool setFilter(function<bool(char *, int32_t *, HostAddress *, uint16_t *)> callback);
    int32_t udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port);
    shared_ptr<SocketLike> accept(const HostAddress &addr, uint16_t port);
protected:
    shared_ptr<Socket> socket() const;
};

SinglePathUdpLinkSocketLike::SinglePathUdpLinkSocketLike(HostAddress::NetworkLayerProtocol protocol)
    : KcpBaseSocketLike<SinglePathUdpLinkManager>(shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>>(new MasterKcpBase<SinglePathUdpLinkManager>(
            shared_ptr<SinglePathUdpLinkManager>(new SinglePathUdpLinkManager(protocol)))))
{
}

SinglePathUdpLinkSocketLike::SinglePathUdpLinkSocketLike(intptr_t socketDescriptor)
    : KcpBaseSocketLike<SinglePathUdpLinkManager>(shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>>(new MasterKcpBase<SinglePathUdpLinkManager>(
            shared_ptr<SinglePathUdpLinkManager>(new SinglePathUdpLinkManager(socketDescriptor)))))
{
}

SinglePathUdpLinkSocketLike::SinglePathUdpLinkSocketLike(shared_ptr<Socket> rawSocket)
    : KcpBaseSocketLike<SinglePathUdpLinkManager>(shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>>(new MasterKcpBase<SinglePathUdpLinkManager>(
            shared_ptr<SinglePathUdpLinkManager>(new SinglePathUdpLinkManager(rawSocket)))))
{
}

SinglePathUdpLinkSocketLike::SinglePathUdpLinkSocketLike(shared_ptr<KcpBase<SinglePathUdpLinkManager>> slave)
    : KcpBaseSocketLike<SinglePathUdpLinkManager>(slave)
{
}

SinglePathUdpLinkSocketLike::~SinglePathUdpLinkSocketLike() { }

Socket::SocketError SinglePathUdpLinkSocketLike::error() const
{
    if (kcpBase->error != Socket::NoError) {
        return kcpBase->error;
    }
    shared_ptr<Socket> rawSocket = socket();
    if (rawSocket) {
        return rawSocket->error();
    }
    return Socket::NoError;
}

string SinglePathUdpLinkSocketLike::errorString() const
{
    if (!kcpBase->errorString.empty()) {
        return kcpBase->errorString;
    }
    shared_ptr<Socket> rawSocket = socket();
    if (rawSocket) {
        return rawSocket->errorString();
    }
    return string();
}

bool SinglePathUdpLinkSocketLike::isValid() const
{
    if (!kcpBase->isValid()) {
        return false;
    }
    shared_ptr<Socket> rawSocket = socket();
    if (rawSocket) {
        return rawSocket->isValid();
    }
    return false;
}

HostAddress SinglePathUdpLinkSocketLike::localAddress() const
{
    shared_ptr<Socket> rawSocket = socket();
    if (rawSocket) {
        return rawSocket->localAddress();
    }
    return HostAddress();
}

uint16_t SinglePathUdpLinkSocketLike::localPort() const
{
    shared_ptr<Socket> rawSocket = socket();
    if (rawSocket) {
        return rawSocket->localPort();
    }
    return 0;
}

HostAddress SinglePathUdpLinkSocketLike::peerAddress() const
{
    return kcpBase->remoteId.addr;
}

uint16_t SinglePathUdpLinkSocketLike::peerPort() const
{
    return kcpBase->remoteId.port;
}

string SinglePathUdpLinkSocketLike::peerName() const
{
    return kcpBase->remoteId.addr.toString();
}

intptr_t SinglePathUdpLinkSocketLike::fileno() const
{
    shared_ptr<Socket> rawSocket = socket();
    if (rawSocket) {
        return rawSocket->fileno();
    }
    return -1;
}

HostAddress::NetworkLayerProtocol SinglePathUdpLinkSocketLike::protocol() const
{
    shared_ptr<Socket> rawSocket = socket();
    if (rawSocket) {
        return rawSocket->protocol();
    }
    return HostAddress::UnknownNetworkLayerProtocol;
}

string SinglePathUdpLinkSocketLike::localAddressURI() const
{
    const HostAddress &addr = localAddress();
    uint16_t port = localPort();
    if (addr.protocol() == HostAddress::IPv6Protocol) {
        return utils::formatMessage("kcp://[%1]:%2", {addr.toString(), utils::number(port)});
    }
    return utils::formatMessage("kcp://%1:%2", {addr.toString(), utils::number(port)});
}

string SinglePathUdpLinkSocketLike::peerAddressURI() const
{
    const HostAddress &addr = peerAddress();
    uint16_t port = peerPort();
    if (addr.protocol() == HostAddress::IPv6Protocol) {
        return utils::formatMessage("kcp://[%1]:%2", {addr.toString(), utils::number(port)});
    }
    return utils::formatMessage("kcp://%1:%2", {addr.toString(), utils::number(port)});
}

shared_ptr<SocketLike> SinglePathUdpLinkSocketLike::accept()
{
    shared_ptr<KcpBase<SinglePathUdpLinkManager>> slave = kcpBase->accept();
    if (!slave) {
        return shared_ptr<SocketLike>();
    }
    return shared_ptr<SinglePathUdpLinkSocketLike>(new SinglePathUdpLinkSocketLike(slave));
}

bool SinglePathUdpLinkSocketLike::bind(const HostAddress &address, uint16_t port,
                                       Socket::BindMode mode /*= Socket::DefaultForPlatform*/)
{
    if (!kcpBase->canBind()) {
        return false;
    }
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (!master) {
        return false;
    }
    shared_ptr<Socket> rawSocket = master->link->rawSocket;
    if (mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    if (!rawSocket->bind(address, port, mode)) {
        return false;
    }
    kcpBase->setState(Socket::BoundState);
    return true;
}

bool SinglePathUdpLinkSocketLike::bind(uint16_t port, Socket::BindMode mode /*= Socket::DefaultForPlatform*/)
{
    if (!kcpBase->canBind()) {
        return false;
    }
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (!master) {
        return false;
    }
    shared_ptr<Socket> rawSocket = master->link->rawSocket;
    if (mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    if (!rawSocket->bind(port, mode)) {
        return false;
    }
    kcpBase->setState(Socket::BoundState);
    return true;
}

bool SinglePathUdpLinkSocketLike::connect(const HostAddress &addr, uint16_t port)
{
    if (addr.isNull() || port == 0) {
        return false;
    }
    if (!kcpBase->canConnect()) {
        return false;
    }
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (!master) {
        return false;
    }
    shared_ptr<Socket> rawSocket = master->link->rawSocket;
    if (rawSocket && rawSocket->protocol() == addr.protocol()) {
        kcpBase->remoteId.addr = addr;
        kcpBase->remoteId.port = port;
        kcpBase->setState(Socket::ConnectedState);
        return true;
    }
    return false;
}

bool SinglePathUdpLinkSocketLike::connect(const string &hostName, uint16_t port,
                                          shared_ptr<SocketDnsCache> dnsCache)
{
    vector<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.push_back(t);
    } else {
        if (!dnsCache) {
            addresses = Socket::resolve(hostName);
        } else {
            addresses = dnsCache->resolve(hostName);
        }
    }
    for (int i = 0; i < addresses.size(); ++i) {
        const HostAddress &addr = addresses[i];
        if (connect(addr, port)) {
            return true;
        }
    }
    return false;
}

bool SinglePathUdpLinkSocketLike::setOption(Socket::SocketOption option, int value)
{
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (master) {
        return master->link->rawSocket->setOption(option, value);
    }
    return false;
}

int SinglePathUdpLinkSocketLike::option(Socket::SocketOption option) const
{
    shared_ptr<Socket> rawSocket = socket();
    if (rawSocket) {
        return rawSocket->option(option);
    }
    return -1;
}

bool SinglePathUdpLinkSocketLike::joinMulticastGroup(const HostAddress &groupAddress,
                                                     const NetworkInterface &iface /*= NetworkInterface()*/)
{
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (master) {
        return master->link->rawSocket->joinMulticastGroup(groupAddress, iface);
    }
    return false;
}

bool SinglePathUdpLinkSocketLike::leaveMulticastGroup(const HostAddress &groupAddress,
                                                      const NetworkInterface &iface /*= NetworkInterface()*/)
{
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (master) {
        return master->link->rawSocket->leaveMulticastGroup(groupAddress, iface);
    }
    return false;
}

NetworkInterface SinglePathUdpLinkSocketLike::multicastInterface() const
{
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (master) {
        return master->link->rawSocket->multicastInterface();
    }
    return NetworkInterface();
}

bool SinglePathUdpLinkSocketLike::setMulticastInterface(const NetworkInterface &iface)
{
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (master) {
        return master->link->rawSocket->setMulticastInterface(iface);
    }
    return false;
}

bool SinglePathUdpLinkSocketLike::setFilter(function<bool(char *, int32_t *, HostAddress *, uint16_t *)> callback)
{
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (master) {
        master->link->filterCallback = callback;
        return true;
    }
    return false;
}

int32_t SinglePathUdpLinkSocketLike::udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port)
{
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (master) {
        return master->link->rawSocket->sendto(data, size, addr, port);
    }
    return -1;
}

shared_ptr<SocketLike> SinglePathUdpLinkSocketLike::accept(const HostAddress &addr, uint16_t port)
{
    SinglePathUdpLinkId remote;
    remote.addr = addr;
    remote.port = port;
    shared_ptr<KcpBase<SinglePathUdpLinkManager>> slave = kcpBase->accept(remote);
    if (!slave) {
        return shared_ptr<SocketLike>();
    }
    return shared_ptr<SinglePathUdpLinkSocketLike>(new SinglePathUdpLinkSocketLike(slave));
}

shared_ptr<Socket> SinglePathUdpLinkSocketLike::socket() const
{
    shared_ptr<MasterKcpBase<SinglePathUdpLinkManager>> master = dynamic_pointer_cast<MasterKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (master) {
        return master->link->rawSocket;
    }
    shared_ptr<SlaveKcpBase<SinglePathUdpLinkManager>> slave = dynamic_pointer_cast<SlaveKcpBase<SinglePathUdpLinkManager>>(kcpBase);
    if (slave && slave->parent) {
        return slave->parent->link->rawSocket;
    }
    return shared_ptr<Socket>();
}

shared_ptr<SocketLike>
createKcpConnection(const HostAddress &host, uint16_t port, Socket::SocketError *error /*= nullptr*/,
                    int allowProtocol /*= HostAddress::IPv4Protocol | HostAddress::IPv6Protocol*/,
                    KcpMode mode /*= AsymmetricInternet*/)
{
    SinglePathUdpLinkSocketLike *socket = createConnection<SinglePathUdpLinkSocketLike>(
            host, port, error, allowProtocol, MakeSocketType<SinglePathUdpLinkSocketLike>);
    if (socket) {
        socket->kcpBase->setMode(mode);
    }
    return shared_ptr<SocketLike>(socket);
}

shared_ptr<SocketLike>
createKcpConnection(const string &hostName, uint16_t port, Socket::SocketError *error /*= nullptr*/,
                    shared_ptr<SocketDnsCache> dnsCache /*= shared_ptr<SocketDnsCache>()*/,
                    int allowProtocol /*= HostAddress::IPv4Protocol | HostAddress::IPv6Protocol*/,
                    KcpMode mode /*= AsymmetricInternet*/)
{
    SinglePathUdpLinkSocketLike *socket = createConnection<SinglePathUdpLinkSocketLike>(
            hostName, port, error, dnsCache, allowProtocol, MakeSocketType<SinglePathUdpLinkSocketLike>);
    if (socket) {
        socket->kcpBase->setMode(mode);
    }
    return shared_ptr<SocketLike>(socket);
}

shared_ptr<SocketLike> createKcpServer(const HostAddress &host, uint16_t port, int backlog,
                                           KcpMode mode /*= Internet*/)
{
    SinglePathUdpLinkSocketLike *socket =
            createServer<SinglePathUdpLinkSocketLike>(host, port, backlog, MakeSocketType<SinglePathUdpLinkSocketLike>);
    if (socket) {
        socket->kcpBase->setMode(mode);
    }
    return shared_ptr<SocketLike>(socket);
}

KcpSocketLikeHelper::KcpSocketLikeHelper(shared_ptr<SocketLike> socket)
    : socket(socket)
{
}

bool KcpSocketLikeHelper::isValid() const
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    return !!kcp;
}

void KcpSocketLikeHelper::setSocket(shared_ptr<SocketLike> socket)
{
    this->socket = socket;
}

uint32_t KcpSocketLikeHelper::payloadSizeHint() const
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    return kcp->kcpBase->payloadSizeHint();
}

void KcpSocketLikeHelper::setDebugLevel(int level)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        kcp->kcpBase->setDebugLevel(level);
    }
}

void KcpSocketLikeHelper::setMode(KcpMode mode)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        kcp->kcpBase->setMode(mode);
    }
}

void KcpSocketLikeHelper::setSendQueueSize(uint32_t sendQueueSize)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        kcp->kcpBase->setSendQueueSize(sendQueueSize);
    }
}

void KcpSocketLikeHelper::setUdpPacketSize(uint32_t udpPacketSize)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        kcp->kcpBase->setUdpPacketSize(udpPacketSize);
    }
}

void KcpSocketLikeHelper::setTearDownTime(float secs)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        kcp->kcpBase->setTearDownTime(secs);
    }
}

bool KcpSocketLikeHelper::setFilter(function<bool(char *, int32_t *, HostAddress *, uint16_t *)> callback)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        return kcp->setFilter(callback);
    }
    return false;
}

int32_t KcpSocketLikeHelper::udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        return kcp->udpSend(data, size, addr, port);
    }
    return -1;
}

shared_ptr<SocketLike> KcpSocketLikeHelper::accept(const HostAddress &addr, uint16_t port)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        return kcp->accept(addr, port);
    }
    return shared_ptr<SocketLike>();
}

bool KcpSocketLikeHelper::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface /*= NetworkInterface()*/)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        return kcp->joinMulticastGroup(groupAddress, iface);
    }
    return false;
}

bool KcpSocketLikeHelper::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface /*= NetworkInterface()*/)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        return kcp->leaveMulticastGroup(groupAddress, iface);
    }
    return false;
}

bool KcpSocketLikeHelper::setOption(Socket::SocketOption option, int value)
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        return kcp->setOption(option, value);
    }
    return false;
}

int KcpSocketLikeHelper::option(Socket::SocketOption option) const
{
    shared_ptr<SinglePathUdpLinkSocketLike> kcp =  dynamic_pointer_cast<SinglePathUdpLinkSocketLike>(socket);
    if (kcp) {
        return kcp->option(option);
    }
    return -1;
}

}  // namespace qtng
