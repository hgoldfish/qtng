#ifndef QTNG_MULTI_STREAM_H
#define QTNG_MULTI_STREAM_H

#include <QtCore/qsharedpointer.h>
#include <QtCore/qstring.h>
#include "socket.h"
#include "socket_utils.h"
#include "config.h"

QTNETWORKNG_NAMESPACE_BEGIN

enum MultiStreamPole {
    MultiStreamPositivePole = 1,
    MultiStreamNegativePole = -1,
};

enum MultiStreamResetCode : quint32 {
    MultiStreamResetNormalClose = 0,
    MultiStreamResetAbort = 1,
    MultiStreamResetProtocolError = 2,
    MultiStreamResetRefused = 3,
};

class MultiStreamSlave;
class MultiStreamMasterPrivate;
class MultiStreamSlavePrivate;

class MultiStreamMaster
{
    Q_DISABLE_COPY(MultiStreamMaster)
public:
    enum StreamError {
        RemotePeerClosedError = 1,
        KeepaliveTimeoutError = 2,
        ReceivingError = 3,
        SendingError = 4,
        InvalidCommand = 5,
        InvalidPacket = 6,
        UserShutdown = 7,
        PacketTooLarge = 8,

        UnknownError = 100,
        ProgrammingError = 101,
        NoError = 0,
    };
public:
    MultiStreamMaster(QSharedPointer<Socket> socket, MultiStreamPole pole);
#ifndef QTNG_NO_CRYPTO
    MultiStreamMaster(QSharedPointer<class SslSocket> socket, MultiStreamPole pole);
#endif
    MultiStreamMaster(QSharedPointer<class KcpSocket> socket, MultiStreamPole pole);
    MultiStreamMaster(QSharedPointer<class UtpSocket> socket, MultiStreamPole pole);
    MultiStreamMaster(QSharedPointer<SocketLike> socket, MultiStreamPole pole);
    ~MultiStreamMaster();
public:
    StreamError error() const;
    QString errorString() const;
    QString toString() const;
    MultiStreamPole pole() const;
    void setName(const QString &name);
    QString name() const;

    bool isBroken() const;
    void abort();

    QSharedPointer<MultiStreamSlave> makeSlave();
    QSharedPointer<MultiStreamSlave> takeSlave();
    QSharedPointer<MultiStreamSlave> takeSlave(quint32 streamNumber);

    void setMaxPacketSize(quint32 size);
    quint32 maxPacketSize() const;
    quint32 maxPayloadSize() const;
    void setPayloadSizeHint(quint32 payloadSizeHint);
    quint32 payloadSizeHint() const;

    void setSlaveReceivingCapacity(quint32 bytes);
    quint32 slaveReceivingCapacity() const;
    void setSlaveSendingCapacity(quint32 bytes);
    quint32 slaveSendingCapacity() const;

    void setKeepaliveTimeout(float timeout);
    float keepaliveTimeout() const;
    void setKeepaliveInterval(float keepaliveInterval);
    float keepaliveInterval() const;

    quint32 sendingQueueSize() const;
    QSharedPointer<SocketLike> connection() const;
private:
    MultiStreamMasterPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(MultiStreamMaster)
    friend class MultiStreamSlavePrivate;
};

class MultiStreamSlave
{
    Q_DISABLE_COPY(MultiStreamSlave)
public:
    quint32 streamNumber() const;
    MultiStreamMaster::StreamError error() const;
    QString errorString() const;
    QString toString() const;
    MultiStreamPole pole() const;
    void setName(const QString &name);
    QString name() const;

    bool isBroken() const;
    bool isClosing() const;
    void close();
    void abort();

    MultiStreamResetCode resetCode() const;

    bool sendPacket(const QByteArray &packet, bool waitSent = true);
    bool sendPacketAsync(const QByteArray &packet);
    QByteArray recvPacket();

    quint32 maxPacketSize() const;
    quint32 maxPayloadSize() const;
    quint32 payloadSizeHint() const;
    void setReceivingCapacity(quint32 bytes);
    quint32 receivingCapacity() const;
    quint32 receivingQueueSize() const;

    void setPriority(int priority);
    int priority() const;

    ~MultiStreamSlave();
private:
    MultiStreamSlave(MultiStreamMaster *master, MultiStreamPole pole, quint32 streamNumber,
                     MultiStreamSlavePrivate *priv);
    friend class MultiStreamMaster;
private:
    MultiStreamSlavePrivate * const d_ptr;
    Q_DECLARE_PRIVATE(MultiStreamSlave)
    friend class MultiStreamMasterPrivate;
    friend class MultiStreamSlavePrivate;
    friend QSharedPointer<SocketLike> asSocketLike(QSharedPointer<MultiStreamSlave> slave);
};

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<MultiStreamSlave> slave);

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_MULTI_STREAM_H
