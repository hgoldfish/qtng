#include "bridge/core_access.h"
#include "bridge/socket_access.h"
#include "bridge/ssl_access.h"
#include "bridge/qt_socket_bridge.h"
#include "multi_stream.h"
#include "udp.h"

#ifndef QTNG_NO_CRYPTO
#  include "ssl.h"
#endif

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

namespace {

shared_ptr<qtng_core::MultiStreamMaster> makeCoreMaster(QSharedPointer<Socket> socket, MultiStreamPole pole)
{
    return make_shared<qtng_core::MultiStreamMaster>(socketCoreOf(socket.data()),
                                                     static_cast<qtng_core::MultiStreamPole>(pole));
}

#ifndef QTNG_NO_CRYPTO
shared_ptr<qtng_core::MultiStreamMaster> makeCoreMaster(QSharedPointer<SslSocket> socket, MultiStreamPole pole)
{
    return make_shared<qtng_core::MultiStreamMaster>(::qtng_bridge::sslSocketCoreOf(socket.data()),
                                                     static_cast<qtng_core::MultiStreamPole>(pole));
}
#endif

shared_ptr<qtng_core::MultiStreamMaster> makeCoreMaster(QSharedPointer<KcpSocket> socket, MultiStreamPole pole)
{
    return make_shared<qtng_core::MultiStreamMaster>(toCoreSocketLike(asSocketLike(socket)),
                                                     static_cast<qtng_core::MultiStreamPole>(pole));
}

shared_ptr<qtng_core::MultiStreamMaster> makeCoreMaster(QSharedPointer<UtpSocket> socket, MultiStreamPole pole)
{
    return make_shared<qtng_core::MultiStreamMaster>(toCoreSocketLike(asSocketLike(socket)),
                                                     static_cast<qtng_core::MultiStreamPole>(pole));
}

shared_ptr<qtng_core::MultiStreamMaster> makeCoreMaster(QSharedPointer<SocketLike> socket, MultiStreamPole pole)
{
    return make_shared<qtng_core::MultiStreamMaster>(toCoreSocketLike(socket), static_cast<qtng_core::MultiStreamPole>(pole));
}

}  // namespace

class MultiStreamMasterPrivate
{
public:
    explicit MultiStreamMasterPrivate(shared_ptr<qtng_core::MultiStreamMaster> master)
        : core(std::move(master))
    {
    }

    shared_ptr<qtng_core::MultiStreamMaster> core;
};

class MultiStreamSlavePrivate
{
public:
    MultiStreamSlavePrivate(MultiStreamMaster *master, MultiStreamPole pole, quint32 streamNumber,
                            shared_ptr<qtng_core::MultiStreamSlave> slave)
        : master(master)
        , pole(pole)
        , streamNumber(streamNumber)
        , core(std::move(slave))
    {
    }

    static QSharedPointer<MultiStreamSlave> wrap(MultiStreamMaster *master,
                                                 const shared_ptr<qtng_core::MultiStreamSlave> &coreSlave)
    {
        if (!coreSlave) {
            return QSharedPointer<MultiStreamSlave>();
        }
        MultiStreamSlavePrivate *priv =
                new MultiStreamSlavePrivate(master, static_cast<MultiStreamPole>(coreSlave->pole()),
                                            coreSlave->streamNumber(), coreSlave);
        return QSharedPointer<MultiStreamSlave>(
                new MultiStreamSlave(master, static_cast<MultiStreamPole>(coreSlave->pole()),
                                     coreSlave->streamNumber(), priv));
    }

    MultiStreamMaster *master;
    MultiStreamPole pole;
    quint32 streamNumber;
    shared_ptr<qtng_core::MultiStreamSlave> core;
};

MultiStreamMaster::MultiStreamMaster(QSharedPointer<Socket> socket, MultiStreamPole pole)
    : d_ptr(new MultiStreamMasterPrivate(makeCoreMaster(socket, pole)))
{
}

#ifndef QTNG_NO_CRYPTO
MultiStreamMaster::MultiStreamMaster(QSharedPointer<SslSocket> socket, MultiStreamPole pole)
    : d_ptr(new MultiStreamMasterPrivate(makeCoreMaster(socket, pole)))
{
}
#endif

MultiStreamMaster::MultiStreamMaster(QSharedPointer<KcpSocket> socket, MultiStreamPole pole)
    : d_ptr(new MultiStreamMasterPrivate(makeCoreMaster(socket, pole)))
{
}

MultiStreamMaster::MultiStreamMaster(QSharedPointer<UtpSocket> socket, MultiStreamPole pole)
    : d_ptr(new MultiStreamMasterPrivate(makeCoreMaster(socket, pole)))
{
}

MultiStreamMaster::MultiStreamMaster(QSharedPointer<SocketLike> socket, MultiStreamPole pole)
    : d_ptr(new MultiStreamMasterPrivate(makeCoreMaster(socket, pole)))
{
}

MultiStreamMaster::~MultiStreamMaster()
{
    delete d_ptr;
}

MultiStreamMaster::StreamError MultiStreamMaster::error() const
{
    return static_cast<StreamError>(d_ptr->core->error());
}

QString MultiStreamMaster::errorString() const
{
    return toQString(d_ptr->core->errorString());
}

QString MultiStreamMaster::toString() const
{
    return toQString(d_ptr->core->toString());
}

MultiStreamPole MultiStreamMaster::pole() const
{
    return static_cast<MultiStreamPole>(d_ptr->core->pole());
}

void MultiStreamMaster::setName(const QString &name)
{
    d_ptr->core->setName(toStdString(name));
}

QString MultiStreamMaster::name() const
{
    return toQString(d_ptr->core->name());
}

bool MultiStreamMaster::isBroken() const
{
    return d_ptr->core->isBroken();
}

void MultiStreamMaster::abort()
{
    d_ptr->core->abort();
}

QSharedPointer<MultiStreamSlave> MultiStreamMaster::makeSlave()
{
    return MultiStreamSlavePrivate::wrap(this, d_ptr->core->makeSlave());
}

QSharedPointer<MultiStreamSlave> MultiStreamMaster::takeSlave()
{
    return MultiStreamSlavePrivate::wrap(this, d_ptr->core->takeSlave());
}

QSharedPointer<MultiStreamSlave> MultiStreamMaster::takeSlave(quint32 streamNumber)
{
    return MultiStreamSlavePrivate::wrap(this, d_ptr->core->takeSlave(streamNumber));
}

void MultiStreamMaster::setMaxPacketSize(quint32 size)
{
    d_ptr->core->setMaxPacketSize(size);
}

quint32 MultiStreamMaster::maxPacketSize() const
{
    return d_ptr->core->maxPacketSize();
}

quint32 MultiStreamMaster::maxPayloadSize() const
{
    return d_ptr->core->maxPayloadSize();
}

void MultiStreamMaster::setPayloadSizeHint(quint32 payloadSizeHint)
{
    d_ptr->core->setPayloadSizeHint(payloadSizeHint);
}

quint32 MultiStreamMaster::payloadSizeHint() const
{
    return d_ptr->core->payloadSizeHint();
}

void MultiStreamMaster::setSlaveReceivingCapacity(quint32 bytes)
{
    d_ptr->core->setSlaveReceivingCapacity(bytes);
}

quint32 MultiStreamMaster::slaveReceivingCapacity() const
{
    return d_ptr->core->slaveReceivingCapacity();
}

void MultiStreamMaster::setSlaveSendingCapacity(quint32 bytes)
{
    d_ptr->core->setSlaveSendingCapacity(bytes);
}

quint32 MultiStreamMaster::slaveSendingCapacity() const
{
    return d_ptr->core->slaveSendingCapacity();
}

void MultiStreamMaster::setKeepaliveTimeout(float timeout)
{
    d_ptr->core->setKeepaliveTimeout(timeout);
}

float MultiStreamMaster::keepaliveTimeout() const
{
    return d_ptr->core->keepaliveTimeout();
}

void MultiStreamMaster::setKeepaliveInterval(float keepaliveInterval)
{
    d_ptr->core->setKeepaliveInterval(keepaliveInterval);
}

float MultiStreamMaster::keepaliveInterval() const
{
    return d_ptr->core->keepaliveInterval();
}

quint32 MultiStreamMaster::sendingQueueSize() const
{
    return d_ptr->core->sendingQueueSize();
}

QSharedPointer<SocketLike> MultiStreamMaster::connection() const
{
    return fromCoreSocketLike(d_ptr->core->connection());
}

MultiStreamSlave::MultiStreamSlave(MultiStreamMaster *master, MultiStreamPole pole, quint32 streamNumber,
                                   MultiStreamSlavePrivate *priv)
    : d_ptr(priv)
{
    Q_UNUSED(master);
    Q_UNUSED(pole);
    Q_UNUSED(streamNumber);
}

MultiStreamSlave::~MultiStreamSlave()
{
    delete d_ptr;
}

quint32 MultiStreamSlave::streamNumber() const
{
    return d_ptr->core ? d_ptr->core->streamNumber() : d_ptr->streamNumber;
}

MultiStreamMaster::StreamError MultiStreamSlave::error() const
{
    return d_ptr->core ? static_cast<MultiStreamMaster::StreamError>(d_ptr->core->error())
                       : MultiStreamMaster::NoError;
}

QString MultiStreamSlave::errorString() const
{
    return d_ptr->core ? toQString(d_ptr->core->errorString()) : QString();
}

QString MultiStreamSlave::toString() const
{
    return d_ptr->core ? toQString(d_ptr->core->toString()) : QString();
}

MultiStreamPole MultiStreamSlave::pole() const
{
    return d_ptr->core ? static_cast<MultiStreamPole>(d_ptr->core->pole()) : d_ptr->pole;
}

void MultiStreamSlave::setName(const QString &name)
{
    if (d_ptr->core) {
        d_ptr->core->setName(toStdString(name));
    }
}

QString MultiStreamSlave::name() const
{
    return d_ptr->core ? toQString(d_ptr->core->name()) : QString();
}

bool MultiStreamSlave::isBroken() const
{
    return d_ptr->core && d_ptr->core->isBroken();
}

bool MultiStreamSlave::isClosing() const
{
    return d_ptr->core && d_ptr->core->isClosing();
}

void MultiStreamSlave::close()
{
    if (d_ptr->core) {
        d_ptr->core->close();
    }
}

void MultiStreamSlave::abort()
{
    if (d_ptr->core) {
        d_ptr->core->abort();
    }
}

MultiStreamResetCode MultiStreamSlave::resetCode() const
{
    return d_ptr->core ? static_cast<MultiStreamResetCode>(d_ptr->core->resetCode()) : MultiStreamResetNormalClose;
}

bool MultiStreamSlave::sendPacket(const QByteArray &packet, bool waitSent)
{
    return d_ptr->core && d_ptr->core->sendPacket(toStdString(packet), waitSent);
}

bool MultiStreamSlave::sendPacketAsync(const QByteArray &packet)
{
    return d_ptr->core && d_ptr->core->sendPacketAsync(toStdString(packet));
}

QByteArray MultiStreamSlave::recvPacket()
{
    return d_ptr->core ? toQByteArray(d_ptr->core->recvPacket()) : QByteArray();
}

quint32 MultiStreamSlave::maxPacketSize() const
{
    return d_ptr->core ? d_ptr->core->maxPacketSize() : 0;
}

quint32 MultiStreamSlave::maxPayloadSize() const
{
    return d_ptr->core ? d_ptr->core->maxPayloadSize() : 0;
}

quint32 MultiStreamSlave::payloadSizeHint() const
{
    return d_ptr->core ? d_ptr->core->payloadSizeHint() : 0;
}

void MultiStreamSlave::setReceivingCapacity(quint32 bytes)
{
    if (d_ptr->core) {
        d_ptr->core->setReceivingCapacity(bytes);
    }
}

quint32 MultiStreamSlave::receivingCapacity() const
{
    return d_ptr->core ? d_ptr->core->receivingCapacity() : 0;
}

quint32 MultiStreamSlave::receivingQueueSize() const
{
    return d_ptr->core ? d_ptr->core->receivingQueueSize() : 0;
}

void MultiStreamSlave::setPriority(int priority)
{
    if (d_ptr->core) {
        d_ptr->core->setPriority(priority);
    }
}

int MultiStreamSlave::priority() const
{
    return d_ptr->core ? d_ptr->core->priority() : 0;
}

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<MultiStreamSlave> slave)
{
    if (!slave || !slave->d_ptr->core) {
        return QSharedPointer<SocketLike>();
    }
    return fromCoreSocketLike(qtng_core::asSocketLike(slave->d_ptr->core));
}

}  // namespace QTNETWORKNG_NAMESPACE
