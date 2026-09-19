#ifndef QTNG_KCP_H
#define QTNG_KCP_H

#include <functional>
#include <QtCore/qsharedpointer.h>
#include <QtCore/qvariant.h>
#include "udp.h"
#include "locks.h"

QTNETWORKNG_NAMESPACE_BEGIN

struct KcpStreamStats {
    quint32 sndWnd;
    quint32 sendBudgetSegs;
    quint32 memoryCapSegs;
    quint32 rtoResends;
    quint32 fastResends;
    quint32 fastresend;
    double lossRate;
    double deliveryBps;
    quint32 waitsnd;
    quint32 sndUna;
    quint32 sndNxt;
    quint32 sentSegs;
    quint32 rmtWnd;
    quint32 rxSrtt;
    quint32 srttMin;
    quint32 mss;
};

class KcpStreamPrivate;
class KcpStream
{
public:
    enum ProtocolVersion : quint8 {
        Version1 = 1,
        Version2 = 2,
    };
public:
    explicit KcpStream(QSharedPointer<DatagramLink> link, quint32 sessionId = 0);
    virtual ~KcpStream();
public:
    QSharedPointer<DatagramLink> link() const;

    quint32 sessionId() const;
    void setSessionId(quint32 id);

    void setProtocolVersion(quint8 version);
    quint8 protocolVersion() const;

    void setLossBasedBudget(bool enabled);
    bool lossBasedBudget() const;

    void setFastResendEnabled(bool enabled);
    bool fastResendEnabled() const;

    void setActivePathCount(quint32 n);
    void setCapacityHintBps(double bitsPerSec);

    void setSendBufferLimit(quint64 bytes);
    quint64 sendBufferLimit() const;
    void setPacketSize(quint32 packetSize);
    quint32 packetSize() const;
    quint32 payloadSizeHint() const;
    void setTearDownTime(float secs);
    float tearDownTime() const;
    KcpStreamStats stats() const;
    Event busy;
    Event notBusy;
public:
    Socket::SocketError error() const;
    QString errorString() const;
    bool isValid() const;
    DatagramPath peerPath() const;
    Socket::SocketState state() const;

    KcpStream *accept();
    KcpStream *accept(const DatagramPath &remote);

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
private:
    explicit KcpStream(KcpStreamPrivate *d);
    void attachEvents();
    friend class KcpStreamPrivate;
private:
    KcpStreamPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(KcpStream)
    Q_DISABLE_COPY(KcpStream)
    KcpStream(KcpStream &&) = delete;
    KcpStream &operator=(KcpStream &&) = delete;
};

class KcpSocketPrivate;
class KcpSocket
{
public:
    explicit KcpSocket(HostAddress::NetworkLayerProtocol protocol = HostAddress::IPv4Protocol);
    explicit KcpSocket(qintptr socketDescriptor);
    explicit KcpSocket(QSharedPointer<Socket> rawSocket);
    virtual ~KcpSocket();
public:
    void setSendBufferLimit(quint64 bytes);
    quint64 sendBufferLimit() const;
    void setUdpPacketSize(quint32 udpPacketSize);
    quint32 udpPacketSize() const;
    quint32 payloadSizeHint() const;
    void setTearDownTime(float secs);
    float tearDownTime() const;
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

    KcpSocket *accept();
    KcpSocket *accept(const HostAddress &addr, quint16 port);
    KcpSocket *accept(const QString &hostName, quint16 port,
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
    void setFilter(std::function<bool(char *, qint32 *, HostAddress *, quint16 *)> callback);
    qint32 udpSend(const char *data, qint32 size, const HostAddress &addr, quint16 port);
    qint32 udpSend(const QByteArray &packet, const HostAddress &addr, quint16 port)
    {
        return udpSend(packet.constData(), packet.size(), addr, port);
    }

    static KcpSocket *createConnection(const HostAddress &host, quint16 port, Socket::SocketError *error = nullptr,
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static KcpSocket *createConnection(const QString &hostName, quint16 port, Socket::SocketError *error = nullptr,
                                       QSharedPointer<SocketDnsCache> dnsCache = QSharedPointer<SocketDnsCache>(),
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static KcpSocket *createServer(const HostAddress &host, quint16 port, int backlog = 50);
private:
    KcpSocket(KcpSocketPrivate *d);
    friend class KcpSocketPrivate;
    KcpSocketPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(KcpSocket)
    Q_DISABLE_COPY(KcpSocket)
    KcpSocket(KcpSocket &&) = delete;
    KcpSocket &operator=(KcpSocket &&) = delete;
};

QSharedPointer<class SocketLike> asSocketLike(QSharedPointer<KcpSocket> s);

inline QSharedPointer<class SocketLike> asSocketLike(KcpSocket *s)
{
    return asSocketLike(QSharedPointer<KcpSocket>(s));
}

QSharedPointer<KcpSocket> convertSocketLikeToKcpSocket(QSharedPointer<class SocketLike> socket);

QSharedPointer<class SocketLike> createKcpConnection(const HostAddress &host, quint16 port,
                                                     Socket::SocketError *error = nullptr,
                                                     int allowProtocol = HostAddress::IPv4Protocol
                                                             | HostAddress::IPv6Protocol);
QSharedPointer<class SocketLike> createKcpConnection(const QString &hostName, quint16 port,
                                                     Socket::SocketError *error = nullptr,
                                                     QSharedPointer<SocketDnsCache> dnsCache = QSharedPointer<SocketDnsCache>(),
                                                     int allowProtocol = HostAddress::IPv4Protocol
                                                             | HostAddress::IPv6Protocol);
QSharedPointer<class SocketLike> createKcpServer(const HostAddress &host, quint16 port, int backlog = 50);

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_KCP_H
