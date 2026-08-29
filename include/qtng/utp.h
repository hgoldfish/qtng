#ifndef QTNG_UTP_H
#define QTNG_UTP_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "qtng/network_interface.h"
#include "qtng/socket.h"
#include "qtng/udp.h"
#include "qtng/utils/platform.h"

namespace qtng {

class UtpStreamPrivate;
class MasterUtpStreamPrivate;
class UtpStream
{
public:
    explicit UtpStream(std::shared_ptr<DatagramLink> link);
    virtual ~UtpStream();
public:
    std::shared_ptr<DatagramLink> link() const;

    void setDelayTarget(float milliseconds);
    float delayTarget() const;
    void setMaxWindow(std::uint32_t bytes);
    std::uint32_t maxWindow() const;
    void setPacketSize(std::uint32_t bytes);
    std::uint32_t packetSize() const;
    std::uint32_t payloadSizeHint() const;
    void setReceiveBufferSize(std::uint32_t bytes);
    std::uint32_t receiveBufferSize() const;
    void setIdleTimeout(float seconds);
    float idleTimeout() const;

    Event busy;
    Event notBusy;
public:
    Socket::SocketError error() const;
    std::string errorString() const;
    bool isValid() const;
    DatagramPath peerPath() const;
    Socket::SocketState state() const;

    UtpStream *accept();
    UtpStream *accept(const DatagramPath &remote);

    bool connect(const DatagramPath &remote);
    bool markBound();
    void close();
    void abort();
    bool listen(int backlog);

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);

    // Used by MasterUtpStreamPrivate demux (private header only).
    bool feedDatagram(const char *data, std::int32_t len, const DatagramPath &remote);
private:
    explicit UtpStream(UtpStreamPrivate *master, const DatagramPath &remote, std::uint16_t synConnId,
                        std::uint16_t synSeq);
    friend class MasterUtpStreamPrivate;
    friend class UtpStreamPrivate;
private:
    UtpStreamPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(UtpStream)
};

class UtpSocketPrivate;
class UtpSocket
{
public:
    explicit UtpSocket(HostAddress::NetworkLayerProtocol protocol = HostAddress::IPv4Protocol);
    explicit UtpSocket(std::intptr_t socketDescriptor);
    explicit UtpSocket(std::shared_ptr<Socket> rawSocket);
    virtual ~UtpSocket();
public:
    void setDelayTarget(float milliseconds);
    float delayTarget() const;
    void setMaxWindow(std::uint32_t bytes);
    std::uint32_t maxWindow() const;
    void setPacketSize(std::uint32_t bytes);
    std::uint32_t packetSize() const;
    std::uint32_t payloadSizeHint() const;
    void setReceiveBufferSize(std::uint32_t bytes);
    std::uint32_t receiveBufferSize() const;
    void setIdleTimeout(float seconds);
    float idleTimeout() const;
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

    UtpSocket *accept();
    UtpSocket *accept(const HostAddress &addr, std::uint16_t port);
    UtpSocket *accept(const std::string &hostName, std::uint16_t port,
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

    static UtpSocket *createConnection(const HostAddress &host, std::uint16_t port, Socket::SocketError *error = nullptr,
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static UtpSocket *createConnection(const std::string &hostName, std::uint16_t port, Socket::SocketError *error = nullptr,
                                       std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>(),
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static UtpSocket *createServer(const HostAddress &host, std::uint16_t port, int backlog = 50);
private:
    explicit UtpSocket(std::shared_ptr<UtpStream> stream);
    friend UtpSocket *wrapUtpStreamAsSocket(std::shared_ptr<UtpStream> stream);
private:
    UtpSocketPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(UtpSocket)
    NG_DISABLE_COPY_MOVE(UtpSocket)
};

UtpSocket *wrapUtpStreamAsSocket(std::shared_ptr<UtpStream> stream);

std::shared_ptr<class SocketLike> asSocketLike(std::shared_ptr<UtpSocket> s);

inline std::shared_ptr<class SocketLike> asSocketLike(UtpSocket *s)
{
    return asSocketLike(std::shared_ptr<UtpSocket>(s));
}

std::shared_ptr<UtpSocket> convertSocketLikeToUtpSocket(std::shared_ptr<class SocketLike> socket);

}  // namespace qtng
#endif  // QTNG_UTP_H
