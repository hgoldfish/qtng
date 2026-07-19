#ifndef QTNG_SOCKET_P_H
#define QTNG_SOCKET_P_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/socket.h"
#include "qtng/utils/platform.h"

namespace qtng {

union qt_sockaddr;

class EventLoopCoroutine;
class HostAddress;

class SocketPrivate
{
public:
    enum ErrorString {
        NonBlockingInitFailedErrorString,
        BroadcastingInitFailedErrorString,
        NoIpV6ErrorString,
        RemoteHostClosedErrorString,
        TimeOutErrorString,
        ResourceErrorString,
        OperationUnsupportedErrorString,
        ProtocolUnsupportedErrorString,
        InvalidSocketErrorString,
        HostUnreachableErrorString,
        NetworkUnreachableErrorString,
        AccessErrorString,
        ConnectionTimeOutErrorString,
        ConnectionRefusedErrorString,
        AddressInuseErrorString,
        AddressNotAvailableErrorString,
        AddressProtectedErrorString,
        DatagramTooLargeErrorString,
        SendDatagramErrorString,
        ReceiveDatagramErrorString,
        WriteErrorString,
        ReadErrorString,
        PortInuseErrorString,
        NotSocketErrorString,
        InvalidProxyTypeString,
        TemporaryErrorString,
        NetworkDroppedConnectionErrorString,
        ConnectionResetErrorString,
        OutOfMemoryErrorString,

        UnknownSocketErrorString = -1
    };
public:
    SocketPrivate(HostAddress::NetworkLayerProtocol protocol, Socket::SocketType type, Socket *parent);
    SocketPrivate(std::intptr_t socketDescriptor, Socket *parent);
    virtual ~SocketPrivate();
public:
    std::string getErrorString() const;
    void setError(Socket::SocketError error, const std::string &errorString);
    void setError(Socket::SocketError error, ErrorString errorString);
    bool checkState() const
    {
        return fd > 0 && (error == Socket::NoError || type != Socket::TcpSocket || error == Socket::RemoteHostClosedError);
    }  // not very accurate
    bool isValid() const;

    Socket *accept();
    bool bind(const HostAddress &address, std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool bind(std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool connect(const HostAddress &host, std::uint16_t port);
    bool connect(const std::string &hostName, std::uint16_t port, std::shared_ptr<SocketDnsCache> dnsCache);
    void close();
    void abort();
    bool listen(int backlog);
    bool setTcpKeepalive(bool keepalve, int keepaliveTimeoutSesc, int keepaliveIntervalSesc);
    bool setOption(Socket::SocketOption option, int value);
    bool setNonblocking();
    int option(Socket::SocketOption option) const;

    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface);
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface);
    NetworkInterface multicastInterface() const;
    bool setMulticastInterface(const NetworkInterface &iface);
    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size, bool all);
    std::int32_t send(const char *data, std::int32_t size, bool all);
    std::int32_t recvfrom(char *data, std::int32_t size, HostAddress *addr, std::uint16_t *port);
    std::int32_t sendto(const char *data, std::int32_t size, const HostAddress &addr, std::uint16_t port);
    bool fetchConnectionParameters();
public:
    bool setPortAndAddress(std::uint16_t port, const HostAddress &address, qt_sockaddr *aa, int *sockAddrSize);
    bool createSocket();
public:
    Socket *q_ptr;
public:
    HostAddress::NetworkLayerProtocol protocol;
    Socket::SocketType type;
    Socket::SocketError error;
    std::string errorString;
    Socket::SocketState state;
    HostAddress localAddress;
    std::uint16_t localPort;
    HostAddress peerAddress;
    std::uint16_t peerPort;
#ifdef NG_OS_WIN
    std::intptr_t fd;
#else
    int fd;
#endif
    Lock readLock;
    Lock writeLock;

    NG_DECLARE_PUBLIC(Socket)
};

}  // namespace qtng

#endif  // QTNG_SOCKET_P_H
