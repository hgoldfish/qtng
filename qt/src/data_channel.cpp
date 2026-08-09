#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "bridge/socket_access.h"
#include "data_channel.h"

using std::shared_ptr;
using std::static_pointer_cast;

namespace qtng_bridge {

class DataChannelPrivateBridge
{
public:
    static QTNETWORKNG_NAMESPACE::DataChannelPrivate *priv(QTNETWORKNG_NAMESPACE::DataChannel *channel)
    {
        return channel->d_ptr;
    }
};

}  // namespace qtng_bridge

namespace QTNETWORKNG_NAMESPACE {

class DataChannelPrivate
{
public:
    shared_ptr<qtng_core::DataChannel> core;
};

static shared_ptr<qtng_core::DataChannel> &coreOf(DataChannel *channel)
{
    return ::qtng_bridge::DataChannelPrivateBridge::priv(channel)->core;
}

}  // namespace QTNETWORKNG_NAMESPACE

namespace QTNETWORKNG_NAMESPACE {

DataChannel::DataChannel(DataChannelPrivate *d)
    : d_ptr(d)
{
}

DataChannel::~DataChannel()
{
    delete d_ptr;
}

DataChannel::ChannelError DataChannel::error() const
{
    return static_cast<ChannelError>(coreOf(const_cast<DataChannel *>(this))->error());
}

QString DataChannel::errorString() const
{
    return ::qtng_bridge::toQString(coreOf(const_cast<DataChannel *>(this))->errorString());
}

QString DataChannel::toString() const
{
    return ::qtng_bridge::toQString(coreOf(const_cast<DataChannel *>(this))->toString());
}

quint32 DataChannel::maxPacketSize() const
{
    return coreOf(const_cast<DataChannel *>(this))->maxPacketSize();
}

quint32 DataChannel::maxPayloadSize() const
{
    return coreOf(const_cast<DataChannel *>(this))->maxPayloadSize();
}

quint32 DataChannel::payloadSizeHint() const
{
    return coreOf(const_cast<DataChannel *>(this))->payloadSizeHint();
}

void DataChannel::setCapacity(quint32 packets)
{
    coreOf(this)->setCapacity(packets);
}

quint32 DataChannel::capacity() const
{
    return coreOf(const_cast<DataChannel *>(this))->capacity();
}

quint32 DataChannel::receivingQueueSize() const
{
    return coreOf(const_cast<DataChannel *>(this))->receivingQueueSize();
}

void DataChannel::setMaxPendingChannels(quint32 count)
{
    coreOf(this)->setMaxPendingChannels(count);
}

quint32 DataChannel::maxPendingChannels() const
{
    return coreOf(const_cast<DataChannel *>(this))->maxPendingChannels();
}

void DataChannel::setSendingTimeout(float timeout)
{
    coreOf(this)->setSendingTimeout(timeout);
}

float DataChannel::sendingTimeout() const
{
    return coreOf(const_cast<DataChannel *>(this))->sendingTimeout();
}

DataChannelPole DataChannel::pole() const
{
    return static_cast<DataChannelPole>(coreOf(const_cast<DataChannel *>(this))->pole());
}

void DataChannel::setName(const QString &name)
{
    coreOf(this)->setName(::qtng_bridge::toStdString(name));
}

QString DataChannel::name() const
{
    return ::qtng_bridge::toQString(coreOf(const_cast<DataChannel *>(this))->name());
}

bool DataChannel::isBroken() const
{
    return coreOf(const_cast<DataChannel *>(this))->isBroken();
}

bool DataChannel::sendPacket(const QByteArray &packet, bool waitSent)
{
    return coreOf(this)->sendPacket(::qtng_bridge::toStdString(packet), waitSent);
}

bool DataChannel::sendPacketAsync(const QByteArray &packet)
{
    return coreOf(this)->sendPacketAsync(::qtng_bridge::toStdString(packet));
}

QByteArray DataChannel::recvPacket()
{
    return ::qtng_bridge::toQByteArray(coreOf(this)->recvPacket());
}

void DataChannel::abort()
{
    coreOf(this)->abort();
}

QSharedPointer<VirtualChannel> DataChannel::makeChannel()
{
    return QSharedPointer<VirtualChannel>();
}

QSharedPointer<VirtualChannel> DataChannel::takeChannel()
{
    return QSharedPointer<VirtualChannel>();
}

QSharedPointer<VirtualChannel> DataChannel::takeChannel(quint32 channelNumber)
{
    Q_UNUSED(channelNumber);
    return QSharedPointer<VirtualChannel>();
}

class SocketChannelPrivate : public DataChannelPrivate
{
};

SocketChannel::SocketChannel(QSharedPointer<Socket> socket, DataChannelPole pole)
    : DataChannel(new SocketChannelPrivate)
{
    coreOf(this) = std::make_shared<qtng_core::SocketChannel>(::qtng_bridge::socketCoreOf(socket.data()),
                                                              static_cast<qtng_core::DataChannelPole>(pole));
}

SocketChannel::SocketChannel(QSharedPointer<SocketLike> socket, DataChannelPole pole)
    : DataChannel(new SocketChannelPrivate)
{
    coreOf(this) = std::make_shared<qtng_core::SocketChannel>(::qtng_bridge::toCoreSocketLike(socket),
                                                              static_cast<qtng_core::DataChannelPole>(pole));
}

void SocketChannel::setMaxPacketSize(quint32 size)
{
    static_pointer_cast<qtng_core::SocketChannel>(coreOf(this))->setMaxPacketSize(size);
}

void SocketChannel::setPayloadSizeHint(quint32 payloadSizeHint)
{
    static_pointer_cast<qtng_core::SocketChannel>(coreOf(this))->setPayloadSizeHint(payloadSizeHint);
}

void SocketChannel::setKeepaliveTimeout(float timeout)
{
    static_pointer_cast<qtng_core::SocketChannel>(coreOf(this))->setKeepaliveTimeout(timeout);
}

float SocketChannel::keepaliveTimeout() const
{
    return static_pointer_cast<qtng_core::SocketChannel>(coreOf(const_cast<SocketChannel *>(this)))->keepaliveTimeout();
}

void SocketChannel::setKeepaliveInterval(float keepaliveInterval)
{
    static_pointer_cast<qtng_core::SocketChannel>(coreOf(this))->setKeepaliveInterval(keepaliveInterval);
}

float SocketChannel::keepaliveInterval() const
{
    return static_pointer_cast<qtng_core::SocketChannel>(coreOf(const_cast<SocketChannel *>(this)))->keepaliveInterval();
}

quint32 SocketChannel::sendingQueueSize() const
{
    return static_pointer_cast<qtng_core::SocketChannel>(coreOf(const_cast<SocketChannel *>(this)))->sendingQueueSize();
}

QSharedPointer<SocketLike> SocketChannel::connection() const
{
    return ::qtng_bridge::toQtSocketLike(
            static_pointer_cast<qtng_core::SocketChannel>(coreOf(const_cast<SocketChannel *>(this)))->connection());
}

VirtualChannel::VirtualChannel(DataChannel *parentChannel, DataChannelPole pole, quint32 channelNumber)
    : DataChannel(new DataChannelPrivate)
{
    Q_UNUSED(parentChannel);
    Q_UNUSED(pole);
    Q_UNUSED(channelNumber);
}

quint32 VirtualChannel::channelNumber() const
{
    return static_pointer_cast<qtng_core::VirtualChannel>(coreOf(const_cast<VirtualChannel *>(this)))->channelNumber();
}

void exchange(QSharedPointer<DataChannel> incoming, QSharedPointer<DataChannel> outgoing)
{
    qtng_core::exchange(coreOf(incoming.data()), coreOf(outgoing.data()));
}

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<DataChannel> channel)
{
    return ::qtng_bridge::toQtSocketLike(qtng_core::asSocketLike(coreOf(channel.data())));
}

}  // namespace QTNETWORKNG_NAMESPACE
