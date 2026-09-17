#ifndef QTNG_KCP_BASE_H
#define QTNG_KCP_BASE_H

#include <functional>
#include "udp.h"
#include "socket_utils.h"

QTNETWORKNG_NAMESPACE_BEGIN

class KcpSocketLikeHelper
{
public:
    explicit KcpSocketLikeHelper(QSharedPointer<SocketLike> socket = QSharedPointer<SocketLike>());
public:
    bool isValid() const;
    void setSocket(QSharedPointer<SocketLike> socket);
    quint32 payloadSizeHint() const;
    void setSendBufferLimit(quint64 bytes);
    void setUdpPacketSize(quint32 udpPacketSize);
    void setTearDownTime(float secs);
    bool setFilter(std::function<bool(char *, qint32 *, HostAddress *, quint16 *)> callback);
    qint32 udpSend(const char *data, qint32 size, const HostAddress &addr, quint16 port);
    QSharedPointer<SocketLike> accept(const HostAddress &addr, quint16 port);
    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    bool setOption(Socket::SocketOption option, const QVariant &value);
    QVariant option(Socket::SocketOption option) const;
protected:
    QSharedPointer<SocketLike> socket;
};

QSharedPointer<SocketLike> createKcpConnection(const HostAddress &host, quint16 port, Socket::SocketError *error = nullptr,
                                               int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
QSharedPointer<SocketLike> createKcpConnection(const QString &hostName, quint16 port, Socket::SocketError *error = nullptr,
                                               QSharedPointer<SocketDnsCache> dnsCache = QSharedPointer<SocketDnsCache>(),
                                               int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
QSharedPointer<SocketLike> createKcpServer(const HostAddress &host, quint16 port, int backlog = 50);

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_KCP_BASE_H
