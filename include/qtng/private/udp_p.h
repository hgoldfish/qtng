#ifndef QTNG_UDP_P_H
#define QTNG_UDP_P_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "qtng/network_interface.h"
#include "qtng/socket.h"
#include "qtng/udp.h"

namespace qtng {

// UDP-specific peer path: HostAddress + port, convertible to opaque DatagramPath.
class UdpDatagramPath
{
public:
    UdpDatagramPath();
    UdpDatagramPath(const HostAddress &addr, std::uint16_t port);
    explicit UdpDatagramPath(const DatagramPath &path);

    DatagramPath toPath() const;
    HostAddress address() const { return m_addr; }
    std::uint16_t port() const { return m_port; }
    bool isNull() const;
private:
    HostAddress m_addr;
    std::uint16_t m_port;
};

// UDP-backed DatagramLink. Kept in a shared private header because both KcpSocket
// (src/kcp.cpp) and UtpSocket (src/utp.cpp) wrap it; implementation lives in src/udp.cpp.
class UdpDatagramLink : public DatagramLink
{
public:
    explicit UdpDatagramLink(HostAddress::NetworkLayerProtocol protocol);
    explicit UdpDatagramLink(std::intptr_t socketDescriptor);
    explicit UdpDatagramLink(std::shared_ptr<Socket> rawSocket);
    ~UdpDatagramLink() override;

    std::shared_ptr<Socket> socket() const;

    bool bind(const HostAddress &address, std::uint16_t port, Socket::BindMode mode);
    bool bind(std::uint16_t port, Socket::BindMode mode);
    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface);
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface);
    NetworkInterface multicastInterface() const;
    bool setMulticastInterface(const NetworkInterface &iface);
    void setFilter(std::function<bool(char *, std::int32_t *, HostAddress *, std::uint16_t *)> callback);

    std::int32_t recvfrom(char *data, std::int32_t size, DatagramPath *who) override;
    std::int32_t sendto(const char *data, std::int32_t size, const DatagramPath &who) override;
    void close() override;
    void abort() override;
    bool isValid() const override;
    Socket::SocketError error() const override;
    std::string errorString() const override;

    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    std::int32_t peek(char *data, std::int32_t size);
    bool setOption(Socket::SocketOption option, int value);
    int option(Socket::SocketOption option) const;
private:
    std::shared_ptr<Socket> rawSocket;
    std::function<bool(char *, std::int32_t *, HostAddress *, std::uint16_t *)> filterCallback;
};

}  // namespace qtng

#endif  // QTNG_UDP_P_H
