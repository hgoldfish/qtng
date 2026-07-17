#ifndef QTNG_DATA_CHANNEL_H
#define QTNG_DATA_CHANNEL_H

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

#include "qtng/socket.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

enum DataChannelPole {
    PositivePole = 1,
    NegativePole = -1,
};

enum SystemChannelNubmer {
    CommandChannelNumber = 0,
    DataChannelNumber = 1,
};

class VirtualChannel;
class DataChannelPrivate;
class DataChannel
{
    NG_DISABLE_COPY(DataChannel)
public:
    enum ChannelError {
        RemotePeerClosedError = 1,
        KeepaliveTimeoutError = 2,
        ReceivingError = 3,
        SendingError = 4,
        InvalidCommand = 5,
        InvalidPacket = 6,
        UserShutdown = 7,
        PluggedChannelError = 8,
        PakcetTooLarge = 9,

        UnknownError = 100,
        ProgrammingError = 101,
        NoError = 0,
    };
public:
    DataChannel(DataChannelPrivate *d);
    virtual ~DataChannel();
public:
    ChannelError error() const;
    std::string errorString() const;
    std::string toString() const;
    std::uint32_t maxPacketSize() const;  // packet with size > maxPacketSize is an error.
    std::uint32_t maxPayloadSize() const;  // maxPacketSize - headerSize(4 or 8)
    std::uint32_t payloadSizeHint() const;  // should be <= maxPayloadSize
    void setCapacity(std::uint32_t packets);  // channel blocked if there are n packets not read.
    std::uint32_t
    capacity() const;  // so, a data channel may consume `maxPacketSize * capacity` bytes of receiving buffer memory.
    std::uint32_t receivingQueueSize() const;
    DataChannelPole pole() const;
    void setName(const std::string &name);
    std::string name() const;

    bool isBroken() const;
    bool sendPacket(const std::string &packet, bool waitSent = true);
    bool sendPacketAsync(const std::string &packet);
    std::string recvPacket();
    void abort();
    std::shared_ptr<VirtualChannel> makeChannel();
    std::shared_ptr<VirtualChannel> takeChannel();
    std::shared_ptr<VirtualChannel> takeChannel(std::uint32_t channelNumber);
protected:
    DataChannelPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(DataChannel)
};

class SocketChannelPrivate;
class SocketChannel : public DataChannel
{
    NG_DISABLE_COPY(SocketChannel)
public:
    SocketChannel(std::shared_ptr<Socket> socket, DataChannelPole pole);
#ifndef QTNG_NO_CRYPTO
    SocketChannel(std::shared_ptr<class SslSocket> socket, DataChannelPole pole);
#endif
    SocketChannel(std::shared_ptr<KcpSocket> socket, DataChannelPole pole);
    SocketChannel(std::shared_ptr<SocketLike> socket, DataChannelPole pole);
public:
    void setMaxPacketSize(std::uint32_t size);  // set to 0 for the default 64k
    void setPayloadSizeHint(std::uint32_t payloadSizeHint);  // usually set to tcp/udp mtu, set to 0 for the default 1400
    void setKeepaliveTimeout(float timeout);
    float keepaliveTimeout() const;
    void setKeepaliveInterval(float keepaliveInterval);
    float keepaliveInterval() const;
    std::uint32_t sendingQueueSize() const;
    std::shared_ptr<SocketLike> connection() const;
private:
    NG_DECLARE_PRIVATE(SocketChannel)
};

class VirtualChannelPrivate;
class VirtualChannel : public DataChannel
{
    NG_DISABLE_COPY(VirtualChannel)
public:
    std::uint32_t channelNumber() const;
protected:
    VirtualChannel(DataChannel *parentChannel, DataChannelPole pole, std::uint32_t channelNumber);
private:
    NG_DECLARE_PRIVATE(VirtualChannel)
    friend class DataChannelPrivate;
    friend class SocketChannelPrivate;
};

void exchange(std::shared_ptr<DataChannel> incoming, std::shared_ptr<DataChannel> outgoing);

std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<DataChannel> channel);

inline std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<VirtualChannel> channel)
{
    return asSocketLike(std::dynamic_pointer_cast<DataChannel>(channel));
}

inline std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<SocketChannel> channel)
{
    return asSocketLike(std::dynamic_pointer_cast<DataChannel>(channel));
}

}  // namespace qtng

#endif  // QTNG_DATA_CHANNEL_H
