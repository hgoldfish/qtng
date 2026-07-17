#ifndef QTNG_KCP_H
#define QTNG_KCP_H

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

#include "qtng/socket.h"
#include "qtng/utils/platform.h"

namespace qtng {

class KcpSocketPrivate;
class KcpSocket
{
public:
    enum Mode {
        LargeDelayInternet,
        Internet,
        FastInternet,
        Ethernet,
        Loopback,
    };
public:
    explicit KcpSocket(HostAddress::NetworkLayerProtocol protocol = HostAddress::IPv4Protocol);
    explicit KcpSocket(std::intptr_t socketDescriptor);
    explicit KcpSocket(std::shared_ptr<Socket> rawSocket);
    virtual ~KcpSocket();
public:
    void setMode(Mode mode);
    Mode mode() const;
    void setSendQueueSize(std::uint32_t sendQueueSize);
    std::uint32_t sendQueueSize() const;
    void setUdpPacketSize(std::uint32_t udpPacketSize);
    std::uint32_t udpPacketSize() const;
    std::uint32_t payloadSizeHint() const;
    void setTearDownTime(float secs);
    float tearDownTime() const;
    Event busy;
    Event notBusy;
public:
    Socket::SocketError error() const;
    std::string errorString() const;
    bool isValid() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    HostAddress peerAddress() const;
    std::string peerName() const;
    std::uint16_t peerPort() const;
    Socket::SocketType type() const;
    Socket::SocketState state() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    std::string localAddressURI() const;
    std::string peerAddressURI() const;

    KcpSocket *accept();
    KcpSocket *accept(const HostAddress &addr, std::uint16_t port);
    KcpSocket *accept(const std::string &hostName, std::uint16_t port,
                      std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>());

    bool bind(const HostAddress &address, std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool bind(std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool connect(const HostAddress &addr, std::uint16_t port);
    bool connect(const std::string &hostName, std::uint16_t port,
                 std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>());
    void close();
    void abort();
    bool listen(int backlog);
    bool setOption(Socket::SocketOption option, int value);
    int option(Socket::SocketOption option) const;

    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    NetworkInterface multicastInterface() const;
    bool setMulticastInterface(const NetworkInterface &iface);

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t peekRaw(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);

    virtual bool filter(char *data, std::int32_t *len, HostAddress *addr, std::uint16_t *port);
    std::int32_t udpSend(const char *data, std::int32_t size, const HostAddress &addr, std::uint16_t port);
    std::int32_t udpSend(const std::string &packet, const HostAddress &addr, std::uint16_t port)
    {
        return udpSend(packet.data(), packet.size(), addr, port);
    }

    static KcpSocket *createConnection(const HostAddress &host, std::uint16_t port, Socket::SocketError *error = nullptr,
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static KcpSocket *createConnection(const std::string &hostName, std::uint16_t port, Socket::SocketError *error = nullptr,
                                       std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>(),
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    // if backlog == 0, do not bind and listen.
    static KcpSocket *createServer(const HostAddress &host, std::uint16_t port, int backlog = 50);
private:
    // for create SlaveKcpSocket.
    KcpSocket(KcpSocketPrivate *d, const HostAddress &addr, const std::uint16_t port, KcpSocket::Mode mode);
    friend class SlaveKcpSocketPrivate;
private:
    KcpSocketPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(KcpSocket)
};

std::shared_ptr<class SocketLike> asSocketLike(std::shared_ptr<KcpSocket> s);

inline std::shared_ptr<class SocketLike> asSocketLike(KcpSocket *s)
{
    return asSocketLike(std::shared_ptr<KcpSocket>(s));
}

std::shared_ptr<KcpSocket> convertSocketLikeToKcpSocket(std::shared_ptr<class SocketLike> socket);

}  // namespace qtng
#endif
