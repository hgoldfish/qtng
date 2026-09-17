#ifndef QTNG_UTP_H
#define QTNG_UTP_H

#include <QtCore/qsharedpointer.h>
#include <QtCore/qvariant.h>
#include "udp.h"
#include "locks.h"

QTNETWORKNG_NAMESPACE_BEGIN

class UtpStreamPrivate;
class UtpStream
{
public:
    explicit UtpStream(QSharedPointer<DatagramLink> link);
    virtual ~UtpStream();
public:
    QSharedPointer<DatagramLink> link() const;

    void setDelayTarget(float milliseconds);
    float delayTarget() const;
    void setMaxWindow(quint32 bytes);
    quint32 maxWindow() const;
    void setPacketSize(quint32 bytes);
    quint32 packetSize() const;
    quint32 payloadSizeHint() const;
    void setReceiveBufferSize(quint32 bytes);
    quint32 receiveBufferSize() const;
    void setIdleTimeout(float seconds);
    float idleTimeout() const;

    Event busy;
    Event notBusy;
public:
    Socket::SocketError error() const;
    QString errorString() const;
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

    qint32 peek(char *data, qint32 size);
    qint32 recv(char *data, qint32 size);
    qint32 recvall(char *data, qint32 size);
    qint32 send(const char *data, qint32 size);
    qint32 sendall(const char *data, qint32 size);
    QByteArray recv(qint32 size);
    QByteArray recvall(qint32 size);
    qint32 send(const QByteArray &data);
    qint32 sendall(const QByteArray &data);

    bool feedDatagram(const char *data, qint32 len, const DatagramPath &remote);
private:
    explicit UtpStream(UtpStreamPrivate *d);
    void attachEvents();
    friend class UtpStreamPrivate;
private:
    UtpStreamPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(UtpStream)
    Q_DISABLE_COPY(UtpStream)
    UtpStream(UtpStream &&) = delete;
    UtpStream &operator=(UtpStream &&) = delete;
};

class UtpSocketPrivate;
class UtpSocket
{
public:
    explicit UtpSocket(HostAddress::NetworkLayerProtocol protocol = HostAddress::IPv4Protocol);
    explicit UtpSocket(qintptr socketDescriptor);
    explicit UtpSocket(QSharedPointer<Socket> rawSocket);
    virtual ~UtpSocket();
public:
    void setDelayTarget(float milliseconds);
    float delayTarget() const;
    void setMaxWindow(quint32 bytes);
    quint32 maxWindow() const;
    void setPacketSize(quint32 bytes);
    quint32 packetSize() const;
    quint32 payloadSizeHint() const;
    void setReceiveBufferSize(quint32 bytes);
    quint32 receiveBufferSize() const;
    void setIdleTimeout(float seconds);
    float idleTimeout() const;
public:
    Socket::SocketError error() const;
    QString errorString() const;
    bool isValid() const;
    HostAddress localAddress() const;
    quint16 localPort() const;
    HostAddress peerAddress() const;
    QString peerName() const;
    quint16 peerPort() const;
    Socket::SocketType type() const;
    Socket::SocketState state() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    QString localAddressURI() const;
    QString peerAddressURI() const;

    UtpSocket *accept();
    UtpSocket *accept(const HostAddress &addr, quint16 port);
    UtpSocket *accept(const QString &hostName, quint16 port,
                      QSharedPointer<SocketDnsCache> dnsCache = QSharedPointer<SocketDnsCache>());

    bool bind(const HostAddress &address, quint16 port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool bind(quint16 port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool connect(const HostAddress &addr, quint16 port);
    bool connect(const QString &hostName, quint16 port,
                 QSharedPointer<SocketDnsCache> dnsCache = QSharedPointer<SocketDnsCache>());
    void close();
    void abort();
    bool listen(int backlog);
    bool setOption(Socket::SocketOption option, const QVariant &value);
    QVariant option(Socket::SocketOption option) const;

    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    NetworkInterface multicastInterface() const;
    bool setMulticastInterface(const NetworkInterface &iface);

    qint32 peek(char *data, qint32 size);
    qint32 peekRaw(char *data, qint32 size);
    qint32 recv(char *data, qint32 size);
    qint32 recvall(char *data, qint32 size);
    qint32 send(const char *data, qint32 size);
    qint32 sendall(const char *data, qint32 size);
    QByteArray recv(qint32 size);
    QByteArray recvall(qint32 size);
    qint32 send(const QByteArray &data);
    qint32 sendall(const QByteArray &data);

    virtual bool filter(char *data, qint32 *len, HostAddress *addr, quint16 *port);
    qint32 udpSend(const char *data, qint32 size, const HostAddress &addr, quint16 port);
    qint32 udpSend(const QByteArray &packet, const HostAddress &addr, quint16 port)
    {
        return udpSend(packet.constData(), packet.size(), addr, port);
    }

    static UtpSocket *createConnection(const HostAddress &host, quint16 port, Socket::SocketError *error = nullptr,
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static UtpSocket *createConnection(const QString &hostName, quint16 port, Socket::SocketError *error = nullptr,
                                       QSharedPointer<SocketDnsCache> dnsCache = QSharedPointer<SocketDnsCache>(),
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static UtpSocket *createServer(const HostAddress &host, quint16 port, int backlog = 50);
private:
    UtpSocket(UtpSocketPrivate *d);
    friend class UtpSocketPrivate;
    UtpSocketPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(UtpSocket)
    Q_DISABLE_COPY(UtpSocket)
    UtpSocket(UtpSocket &&) = delete;
    UtpSocket &operator=(UtpSocket &&) = delete;
};

QSharedPointer<class SocketLike> asSocketLike(QSharedPointer<UtpSocket> s);

inline QSharedPointer<class SocketLike> asSocketLike(UtpSocket *s)
{
    return asSocketLike(QSharedPointer<UtpSocket>(s));
}

QSharedPointer<UtpSocket> convertSocketLikeToUtpSocket(QSharedPointer<class SocketLike> socket);

QSharedPointer<class SocketLike> createUtpConnection(const HostAddress &host, quint16 port,
                                                     Socket::SocketError *error = nullptr,
                                                     int allowProtocol = HostAddress::IPv4Protocol
                                                             | HostAddress::IPv6Protocol);
QSharedPointer<class SocketLike> createUtpConnection(const QString &hostName, quint16 port,
                                                     Socket::SocketError *error = nullptr,
                                                     QSharedPointer<SocketDnsCache> dnsCache = QSharedPointer<SocketDnsCache>(),
                                                     int allowProtocol = HostAddress::IPv4Protocol
                                                             | HostAddress::IPv6Protocol);
QSharedPointer<class SocketLike> createUtpServer(const HostAddress &host, quint16 port, int backlog = 50);

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_UTP_H
