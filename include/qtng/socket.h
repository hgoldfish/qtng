#ifndef QTNG_SOCKET_H
#define QTNG_SOCKET_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


#include "qtng/hostaddress.h"
#include "qtng/network_interface.h"
#include "qtng/private/eventloop_p.h"
#include "qtng/locks.h"
#include "qtng/utils/platform.h"

#ifdef fileno  // android define fileno() function as macro
#  undef fileno
#endif

namespace qtng {

class SocketPrivate;
class SocketDnsCache;
class Socket
{
public:
    enum SocketType {
        TcpSocket = 1,
        UdpSocket = 2,
        // SctpSocket = QAbstractSocket::SctpSocket,
        // define for other XXXSocket types. not used here.
        KcpSocket = 3,
        LocalSocket = 4,
        UnknownSocketType = -1
    };
    enum SocketError {
        ConnectionRefusedError = 1,
        RemoteHostClosedError = 2,
        HostNotFoundError = 3,
        SocketAccessError = 4,
        SocketResourceError = 5,
        SocketTimeoutError = 6,
        DatagramTooLargeError = 7,
        NetworkError = 8,
        AddressInUseError = 9,
        SocketAddressNotAvailableError = 10,
        UnsupportedSocketOperationError = 11,
        UnfinishedSocketOperationError = 12,
        OutOfMemoryError = 13,

        // define for proxy and ssl, not used here.
        ProxyAuthenticationRequiredError = 101,
        SslHandshakeFailedError = 102,
        ProxyConnectionRefusedError = 103,
        ProxyConnectionClosedError = 104,
        ProxyConnectionTimeoutError = 105,
        ProxyNotFoundError = 106,
        ProxyProtocolError = 107,
        OperationError = 108,
        SslInternalError = 109,
        SslInvalidUserDataError = 110,
        TemporaryError = 111,

        UnknownSocketError = -1,
        NoError = -2,
    };
    enum SocketState {
        UnconnectedState = 1,
        HostLookupState = 2,
        ConnectingState = 3,
        ConnectedState = 4,
        BoundState = 5,
        ListeningState = 6,
        ClosingState = 7
    };
    enum SocketOption {
        BroadcastSocketOption = 1,  // SO_BROADCAST
        AddressReusable = 2,  // SO_REUSEADDR
        ReceiveOutOfBandData = 3,  // SO_OOBINLINE
        ReceivePacketInformation = 4,  // IP_PKTINFO
        ReceiveHopLimit = 5,  // IP_RECVTTL
        LowDelayOption = 6,  // TCP_NODELAY
        KeepAliveOption = 7,  // SO_KEEPALIVE
        MulticastTtlOption = 8,  // IP_MULTICAST_TTL
        MulticastLoopbackOption = 9,  // IP_MULTICAST_LOOPBACK
        TypeOfServiceOption = 10,  // IP_TOS
        SendBufferSizeSocketOption = 11,  // SO_SNDBUF
        ReceiveBufferSizeSocketOption = 12,  // SO_RCVBUF
        MaxStreamsSocketOption = 13,  // for sctp
        NonBlockingSocketOption = 14,
        BindExclusively = 15,
        PathMtuSocketOption = 16
    };
    enum BindFlag { DefaultForPlatform = 0x0, ShareAddress = 0x1, DontShareAddress = 0x2, ReuseAddressHint = 0x4 };
    using BindMode = int;
public:
    explicit Socket(HostAddress::NetworkLayerProtocol protocol = HostAddress::IPv4Protocol,
                    SocketType type = TcpSocket);
    explicit Socket(std::intptr_t socketDescriptor);
    virtual ~Socket();
public:
    SocketError error() const;
    std::string errorString() const;
    bool isValid() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    HostAddress peerAddress() const;
    std::string peerName() const;
    std::uint16_t peerPort() const;
    std::intptr_t fileno() const;
    SocketType type() const;
    SocketState state() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    std::string localAddressURI() const;
    std::string peerAddressURI() const;

    Socket *accept();
    bool bind(const HostAddress &address, std::uint16_t port = 0, BindMode mode = DefaultForPlatform);
    bool bind(std::uint16_t port = 0, BindMode mode = DefaultForPlatform);
    bool connect(const HostAddress &host, std::uint16_t port);
    bool connect(const std::string &hostName, std::uint16_t port,
                 std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>());
    void close();
    void abort();
    bool listen(int backlog);
    bool setTcpKeepalive(bool keepalve, int keepaliveTimeoutSesc, int keepaliveIntervalSesc);
    bool setOption(SocketOption option, int value);
    int option(SocketOption option) const;

    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    NetworkInterface multicastInterface() const;
    bool setMulticastInterface(const NetworkInterface &iface);

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::int32_t recvfrom(char *data, std::int32_t size, HostAddress *addr, std::uint16_t *port);
    std::int32_t sendto(const char *data, std::int32_t size, const HostAddress &addr, std::uint16_t port);

    std::string recvall(std::int32_t size);
    std::string recv(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);
    std::string recvfrom(std::int32_t size, HostAddress *addr, std::uint16_t *port);
    std::int32_t sendto(const std::string &data, const HostAddress &addr, std::uint16_t port);

    static std::vector<HostAddress> resolve(const std::string &hostName);
    static Socket *createConnection(const HostAddress &host, std::uint16_t port, Socket::SocketError *error = nullptr,
                                    int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static Socket *createConnection(const std::string &hostName, std::uint16_t port, Socket::SocketError *error = nullptr,
                                    std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>(),
                                    int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static Socket *createServer(const HostAddress &host, std::uint16_t port, int backlog = 50);
private:
    SocketPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(Socket)
    NG_DISABLE_COPY(Socket)
};

class PollPrivate;
class Poll
{
public:
    enum EventType {
        Read = EventLoopCoroutine::Read,
        ReadWrite = EventLoopCoroutine::ReadWrite,
        Write = EventLoopCoroutine::Write,
    };
public:
    Poll();
    virtual ~Poll();
public:
    void add(std::shared_ptr<Socket> socket, EventType event);
    void remove(std::shared_ptr<Socket> socket);
    std::shared_ptr<Socket> wait(float msecs = 0.0);
private:
    PollPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(Poll)
};

class SocketDnsCachePrivate;
class SocketDnsCache
{
public:
    SocketDnsCache();
    virtual ~SocketDnsCache();
public:
    std::vector<HostAddress> resolve(const std::string &hostName);
    bool hasHost(const std::string &hostName) const;
    void addHost(const std::string &hostName, const std::vector<HostAddress> &addrList);
    void addHost(const std::string &hostName, const HostAddress &addr);
    std::uint64_t timeToLive() const;
    void setTimeToLive(std::uint64_t msecs);
private:
    SocketDnsCachePrivate * const d_ptr;
    NG_DECLARE_PRIVATE(SocketDnsCache)
};

template<typename SocketType>
SocketType *createConnection(const HostAddress &addr, std::uint16_t port, Socket::SocketError *error, int allowProtocol,
                             std::function<SocketType *(HostAddress::NetworkLayerProtocol)> func)
{
    std::unique_ptr<SocketType> socket;
    if (addr.isNull() || port == 0) {
        return nullptr;
    }
    bool isIPv4Address = addr.isIPv4();
    if (isIPv4Address && (allowProtocol & HostAddress::IPv4Protocol)) {
        socket.reset(func(HostAddress::IPv4Protocol));
    } else if (!isIPv4Address && (allowProtocol & HostAddress::IPv6Protocol)) {
        socket.reset(func(HostAddress::IPv6Protocol));
    }
    if (socket) {
        bool done = socket->connect(addr, port);
        if (done) {
            if (error) {
                *error = Socket::NoError;
            }
            return socket.release();
        } else {
            if (error) {
                *error = socket->error();
            }
        }
    }
    return nullptr;
}

template<typename SocketType>
SocketType *createConnection(const std::string &hostName, std::uint16_t port, Socket::SocketError *error,
                             std::shared_ptr<SocketDnsCache> dnsCache, int allowProtocol,
                             std::function<SocketType *(HostAddress::NetworkLayerProtocol)> func)
{
    std::vector<HostAddress> addresses;
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

    if (addresses.empty()) {
        if (error) {
            *error = Socket::HostNotFoundError;
        }
        return nullptr;
    }
    for (int i = 0; i < addresses.size(); ++i) {
        const HostAddress &addr = addresses[i];
        SocketType *socket = createConnection<SocketType>(addr, port, error, allowProtocol, func);
        if (socket) {
            return socket;
        }
    }
    if (error && *error == Socket::NoError) {
        *error = Socket::HostNotFoundError;
    }
    return nullptr;
}

template<typename SocketType>
SocketType *createServer(const HostAddress &host, std::uint16_t port, int backlog,
                         std::function<SocketType *(HostAddress::NetworkLayerProtocol)> func)
{
    std::unique_ptr<SocketType> socket;
    if (host == HostAddress::AnyIPv4 || host == HostAddress::Any) {
        socket.reset(func(HostAddress::IPv4Protocol));
    } else if (host == HostAddress::AnyIPv6) {
        socket.reset(func(HostAddress::IPv6Protocol));
    } else {
        if (host.isIPv4()) {
            socket.reset(func(HostAddress::IPv4Protocol));
        } else {
            socket.reset(func(HostAddress::IPv6Protocol));
        }
    }
    if (backlog > 0) {
        socket->setOption(Socket::AddressReusable, true);
        if (!socket->bind(host, port)) {
            return nullptr;
        }
        if (!socket->listen(backlog)) {
            return nullptr;
        }
    }
    return socket.release();
}

template<typename SocketType>
SocketType *MakeSocketType(HostAddress::NetworkLayerProtocol protocol)
{
    return new SocketType(protocol);
}

}  // namespace qtng

#endif  // QTNG_SOCKET_H
