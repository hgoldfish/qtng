#ifndef QTNG_KCP_BASE_H
#define QTNG_KCP_BASE_H

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

#include "qtng/socket_utils.h"

namespace qtng {

enum KcpMode {
    LargeDelayInternet,
    Internet,
    FastInternet,
    Ethernet,
    Loopback,
    AsymmetricInternet,
};

class KcpSocketLikeHelper
{
public:
    explicit KcpSocketLikeHelper(std::shared_ptr<SocketLike> socket = nullptr);
public:
    bool isValid() const;
    void setSocket(std::shared_ptr<SocketLike> socket);
    std::uint32_t payloadSizeHint() const;
    void setMode(KcpMode mode);
    void setDebugLevel(int level);
    void setSendQueueSize(std::uint32_t sendQueueSize);
    void setUdpPacketSize(std::uint32_t udpPacketSize);
    void setTearDownTime(float secs);
    bool setFilter(std::function<bool(char *, std::int32_t *, HostAddress *, std::uint16_t *)> callback);
    std::int32_t udpSend(const char *data, std::int32_t size, const HostAddress &addr, std::uint16_t port);
    std::shared_ptr<SocketLike> accept(const HostAddress &addr, std::uint16_t port);
    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    bool setOption(Socket::SocketOption option, int value);
    int option(Socket::SocketOption option) const;
protected:
    std::shared_ptr<SocketLike> socket;
};

std::shared_ptr<SocketLike> createKcpConnection(const HostAddress &host, std::uint16_t port, Socket::SocketError *error = nullptr,
                                            int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol, KcpMode mode = AsymmetricInternet);
std::shared_ptr<SocketLike> createKcpConnection(const std::string &hostName, std::uint16_t port, Socket::SocketError *error = nullptr,
                                   std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>(),
                    int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol, KcpMode mode = AsymmetricInternet);
// if backlog == 0, do not bind and listen.
std::shared_ptr<SocketLike> createKcpServer(const HostAddress &host, std::uint16_t port, int backlog = 50,
                                           KcpMode mode = Internet);

}  // namespace qtng
#endif // QTNG_KCP_BASE_H
