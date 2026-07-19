#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>


#ifndef QTNG_NO_CRYPTO
#  include "qtng/ssl.h"
#endif

#include "qtng/coroutine_utils.h"
#include "qtng/data_channel.h"
#include "qtng/utils/string_utils.h"
#include "qtng/udp.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.data_channel");

//#define DEBUG_PROTOCOL

namespace qtng {

const uint8_t MAKE_CHANNEL_REQUEST = 1;
const uint8_t CHANNEL_MADE_REQUEST = 2;
const uint8_t DESTROY_CHANNEL_REQUEST = 3;
const uint8_t SLOW_DOWN_REQUEST = 4;
const uint8_t GO_THROUGH_REQUEST = 5;
const uint8_t KEEPALIVE_REQUEST = 6;
const uint32_t DefaultPacketSize = 1024 * 64;
const uint32_t DefaultPayloadSize = 1400;

static string packMakeChannelRequest(uint32_t channelNumber)
{
    uint8_t buf[sizeof(uint8_t) + sizeof(uint32_t)];
    ngToBigEndian(MAKE_CHANNEL_REQUEST, buf);
    ngToBigEndian(channelNumber, buf + sizeof(uint8_t));
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

static string packChannelMadeRequest(uint32_t channelNumber)
{
    uint8_t buf[sizeof(uint8_t) + sizeof(uint32_t)];
    ngToBigEndian(CHANNEL_MADE_REQUEST, buf);
    ngToBigEndian(channelNumber, buf + sizeof(uint8_t));
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

static string packDestoryChannelRequest(uint32_t channelNumber)
{
    uint8_t buf[sizeof(uint8_t) + sizeof(uint32_t)];
    ngToBigEndian(DESTROY_CHANNEL_REQUEST, buf);
    ngToBigEndian(channelNumber, buf + sizeof(uint8_t));
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

static string packSlowDownRequest()
{
    uint8_t buf[sizeof(uint8_t)];
    ngToBigEndian(SLOW_DOWN_REQUEST, buf);
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

static string packGoThroughRequest()
{
    uint8_t buf[sizeof(uint8_t)];
    ngToBigEndian(GO_THROUGH_REQUEST, buf);
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

static string packKeepaliveRequest()
{
    uint8_t buf[sizeof(uint8_t)];
    ngToBigEndian(KEEPALIVE_REQUEST, buf);
    return string(reinterpret_cast<char *>(buf), sizeof(buf));
}

static bool unpackCommand(string data, uint8_t *command, uint32_t *channelNumber)
{
    if (data.size() == (sizeof(uint8_t) + sizeof(uint32_t))) {
        *command = ngFromBigEndian<uint8_t>(data.data());
        if (*command != MAKE_CHANNEL_REQUEST && *command != CHANNEL_MADE_REQUEST
            && *command != DESTROY_CHANNEL_REQUEST) {
            return false;
        }
        *channelNumber = ngFromBigEndian<uint32_t>(data.data() + sizeof(uint8_t));
        return true;
    } else if (data.size() == sizeof(uint8_t)) {
        *command = ngFromBigEndian<uint8_t>(data.data());
        if (*command != GO_THROUGH_REQUEST && *command != SLOW_DOWN_REQUEST && *command != KEEPALIVE_REQUEST) {
            return false;
        }
        return true;
    } else {
        return false;
    }
}

enum class BlockFlag
{
    NonBlock,
    Block_And_Not_Wait_Sent,
    Block_Until_Sent,
};

class DataChannelPrivate
{
public:
    DataChannelPrivate(DataChannelPole pole, DataChannel *parent);
    virtual ~DataChannelPrivate();

    // called by the public class DataChannel
    shared_ptr<VirtualChannel> makeChannelInternal(DataChannelPole pole, uint32_t channelNumber);
    shared_ptr<VirtualChannel> makeChannel();
    shared_ptr<VirtualChannel> takeChannel();
    shared_ptr<VirtualChannel> takeChannel(uint32_t channelNumber);
    shared_ptr<VirtualChannel> peekChannel(uint32_t channelNumber);
    string recvPacket();
    bool sendPacket(const string &packet, bool waitSent);
    bool sendPacket(string &&packet, bool waitSent);
    bool sendPacketAsync(const string &packet);
    bool sendPacketAsync(string &&packet);
    string toString() const;

    // must be implemented by subclasses
    virtual void abort(DataChannel::ChannelError reason);
    virtual bool isBroken() const = 0;
    virtual bool sendPacketRaw(uint32_t channelNumber, string payload, BlockFlag blocking) = 0;
    virtual void cleanChannel(uint32_t channelNumber, bool sendDestroyPacket) = 0;
    virtual void cleanSendingPacket(uint32_t subChannelNumber,
                                    function<bool(const string &)> subCheckPacket) = 0;
    virtual uint32_t maxPayloadSize() const = 0;
    virtual uint32_t payloadSizeHint() const = 0;
    virtual uint32_t headerSize() const = 0;
    virtual shared_ptr<SocketLike> getBackend() const = 0;

    // called by the subclasses.
    bool handleCommand(const string &packet);
    void notifyChannelClose(uint32_t channelNumber);
    DataChannel::ChannelError handleIncomingPacket(uint32_t channelNumber, string payload);

    string name;
    DataChannelPole pole;
    uint32_t nextChannelNumber;
    map<uint32_t, weak_ptr<VirtualChannel>> subChannels;
    deque<shared_ptr<VirtualChannel>> pendingChannels;
    Condition pendingChannelsNotEmpty;
    SizedQueue<string> receivingQueue;
    bool slowDownRequested = false;
    Gate goThrough;
    DataChannel::ChannelError error;

    shared_ptr<DataChannel> pluggedChannel;

    NG_DECLARE_PUBLIC(DataChannel)
    DataChannel * const q_ptr;

    inline static DataChannelPrivate *getPrivateHelper(weak_ptr<DataChannel> channel)
    {
        shared_ptr<DataChannel> strong = channel.lock();
        return strong ? strong->d_func() : nullptr;
    }
    inline static DataChannelPrivate *getPrivateHelper(shared_ptr<DataChannel> channel)
    {
        return channel ? channel->d_func() : nullptr;
    }
    inline static DataChannelPrivate *getPrivateHelper(DataChannel *channel)
    {
        return channel ? channel->d_func() : nullptr;
    }
};

class WritingPacket
{
public:
    WritingPacket()
        : channelNumber(0)
    {
    }
    WritingPacket(uint32_t channelNumber, string packet, shared_ptr<ValueEvent<bool>> done)
        : packet(std::move(packet))
        , done(done)
        , channelNumber(channelNumber)
    {
    }

    string packet;
    shared_ptr<ValueEvent<bool>> done;
    uint32_t channelNumber;
    uint32_t size() const { return static_cast<uint32_t>(packet.size()); }
    bool isValid() { return !(channelNumber == 0 && packet.empty() && !done); }
};

class SocketChannelPrivate : public DataChannelPrivate
{
public:
    SocketChannelPrivate(shared_ptr<SocketLike> connection, DataChannelPole pole, SocketChannel *parent);
    virtual ~SocketChannelPrivate() override;
    virtual bool isBroken() const override;
    virtual void abort(DataChannel::ChannelError reason) override;
    virtual bool sendPacketRaw(uint32_t channelNumber, string packet, BlockFlag blocking) override;
    virtual void cleanChannel(uint32_t channelNumber, bool sendDestroyPacket) override;
    virtual void cleanSendingPacket(uint32_t subChannelNumber,
                                    function<bool(const string &)> subCheckPacket) override;
    virtual uint32_t maxPayloadSize() const override;
    virtual uint32_t payloadSizeHint() const override;
    virtual uint32_t headerSize() const override;
    virtual shared_ptr<SocketLike> getBackend() const override;
    void doSend();
    void doReceive();
    void doKeepalive();

    const shared_ptr<SocketLike> connection;
    SizedQueue<WritingPacket> sendingQueue;
    CoroutineGroup *operations;
    uint32_t _maxPayloadSize;
    uint32_t _payloadSizeHint;
    int64_t lastActiveTimestamp;
    int64_t lastKeepaliveTimestamp;
    int64_t keepaliveTimeout;
    int64_t keepaliveInterval;

    NG_DECLARE_PUBLIC(SocketChannel)
};

class VirtualChannelPrivate : public DataChannelPrivate
{
public:
    VirtualChannelPrivate(DataChannel *parentChannel, DataChannelPole pole, uint32_t channelNumber,
                          VirtualChannel *parent);
    virtual ~VirtualChannelPrivate() override;
    virtual bool isBroken() const override;
    virtual void abort(DataChannel::ChannelError reason) override;
    virtual bool sendPacketRaw(uint32_t channelNumber, string packet, BlockFlag blocking) override;
    virtual void cleanChannel(uint32_t channelNumber, bool sendDestroyPacket) override;
    virtual void cleanSendingPacket(uint32_t subChannelNumber,
                                    function<bool(const string &)> subCheckPacket) override;
    virtual uint32_t maxPayloadSize() const override;
    virtual uint32_t payloadSizeHint() const override;
    virtual uint32_t headerSize() const override;
    virtual shared_ptr<SocketLike> getBackend() const override;

    DataChannel *parentChannel;
    uint32_t channelNumber;

    NG_DECLARE_PUBLIC(VirtualChannel)
};

DataChannelPrivate::DataChannelPrivate(DataChannelPole pole, DataChannel *parent)
    : pole(pole)
    , receivingQueue(128 * 1024)
    , error(DataChannel::NoError)
    , q_ptr(parent)
{
    if (pole == DataChannelPole::NegativePole) {
        nextChannelNumber = 0xffffffff;
    } else {
        nextChannelNumber = 2;
    }
}

DataChannelPrivate::~DataChannelPrivate()
{
    // do not uncomment these lines of code
    // these codes lead to bug.
    //    for (int i = 0; i < receivingQueue.getting(); ++i) {
    //        receivingQueue.put(string());
    //    }
}

string DataChannelPrivate::toString() const
{
    string clazz;
    if (dynamic_cast<const VirtualChannelPrivate *>(this)) {
        clazz = "VirtualChannel";
    } else {
        clazz = "SocketChannel";
    }
    return utils::formatMessage("<%1 (name = %2, error = %3, capacity = %4, queue_size = %5)>",
            {clazz, name.empty() ? "unamed" : name, q_ptr->errorString(),
             utils::number(static_cast<long long>(receivingQueue.capacity())),
             utils::number(static_cast<int>(receivingQueue.size()))});
}

void DataChannelPrivate::abort(DataChannel::ChannelError reason)
{
    assert(error != DataChannel::NoError);  // must be called by subclasses's close method.
    if (pluggedChannel) {
        getPrivateHelper(pluggedChannel)->abort(reason);
        pluggedChannel.reset();
    }

    for (uint32_t i = 0; i < receivingQueue.getting(); ++i) {
        receivingQueue.put(string());
    }
    for (uint32_t i = 0; i < pendingChannelsNotEmpty.getting(); ++i) {
        pendingChannels.push_back(shared_ptr<VirtualChannel>());
    }
    pendingChannelsNotEmpty.notifyAll();

    goThrough.open();
    for (const auto &item : subChannels) {
        shared_ptr<VirtualChannel> strong = item.second.lock();
        if (strong) {
            strong->d_func()->parentChannel = nullptr;
            strong->d_func()->abort(this->error);
        }
    }
    subChannels.clear();
}

DataChannel::ChannelError DataChannelPrivate::handleIncomingPacket(uint32_t channelNumber, string payload)
{
    if (pluggedChannel) {
        if (!getPrivateHelper(pluggedChannel)
                     ->sendPacketRaw(channelNumber, std::move(payload), BlockFlag::Block_Until_Sent)) {
            return DataChannel::PluggedChannelError;
        } else {
            return DataChannel::NoError;
        }
    }

    if (channelNumber == DataChannelNumber) {
        if (!slowDownRequested && receivingQueue.size() >= (receivingQueue.capacity() * 3 / 4)) {
            slowDownRequested = true;
            sendPacketRaw(CommandChannelNumber, packSlowDownRequest(), BlockFlag::Block_And_Not_Wait_Sent);
        }
        receivingQueue.put(std::move(payload));
    } else if (channelNumber == CommandChannelNumber) {
        if (!handleCommand(payload)) {
            return DataChannel::InvalidCommand;
        } else {
            return DataChannel::NoError;
        }
    } else if (subChannels.find(channelNumber) != subChannels.end()) {
        shared_ptr<VirtualChannel> channel = subChannels.at(channelNumber).lock();
        if (!channel) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "channel is destroyed and data is abandoned: " << channelNumber;
#endif
            subChannels.erase(channelNumber);
        } else {
            const int headerSize = sizeof(uint32_t);
            if (payload.size() < headerSize) {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "the sub channel got an too small packet: " << channelNumber << payload.size()
                           << headerSize;
#endif
                return DataChannel::InvalidPacket;
            }
            uint32_t nestedChannelNumber = ngFromBigEndian<uint32_t>(payload.data());
            string packet = payload.substr(headerSize);
            DataChannel::ChannelError handlePacketResult =
                    channel->d_func()->handleIncomingPacket(nestedChannelNumber, std::move(packet));
            if (handlePacketResult != DataChannel::NoError) {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "the sub channel got an too small packet: " << channelNumber << payload.size()
                           << headerSize;
#endif
                channel->d_func()->abort(handlePacketResult);
            }
        }
    } else {
#ifdef DEBUG_PROTOCOL
        ngDebug() << "channel is destroyed and data is abandoned: " << channelNumber;
#endif
    }
    return DataChannel::NoError;
}

shared_ptr<VirtualChannel> DataChannelPrivate::makeChannelInternal(DataChannelPole pole, uint32_t channelNumber)
{
    shared_ptr<VirtualChannel> channel(new VirtualChannel(q_ptr, pole, channelNumber));
    channel->d_func()->receivingQueue.setCapacity(receivingQueue.capacity());
    subChannels[channelNumber] = channel;
    return channel;
}

shared_ptr<VirtualChannel> DataChannelPrivate::makeChannel()
{
    if (isBroken()) {
#ifdef DEBUG_PROTOCOL
        ngDebug() << "the data channel is broken, can not make channel.";
#endif
        return shared_ptr<VirtualChannel>();
    }
    uint32_t channelNumber = nextChannelNumber;
    nextChannelNumber += this->pole;
    shared_ptr<VirtualChannel> channel = makeChannelInternal(DataChannelPole::PositivePole, channelNumber);
    sendPacketRaw(CommandChannelNumber, packMakeChannelRequest(channelNumber), BlockFlag::NonBlock);
    return channel;
}

shared_ptr<VirtualChannel> DataChannelPrivate::takeChannel()
{
    if (isBroken()) {
        return shared_ptr<VirtualChannel>();
    }
    while (true) {
        if (!pendingChannels.empty()) {
            shared_ptr<VirtualChannel> channel = pendingChannels.front();
            pendingChannels.pop_front();
            return channel;
        }
        if (!pendingChannelsNotEmpty.wait()) {
            return shared_ptr<VirtualChannel>();
        }
    }
}

shared_ptr<VirtualChannel> DataChannelPrivate::takeChannel(uint32_t channelNumber)
{
    if (isBroken()) {
        return shared_ptr<VirtualChannel>();
    }
    for (int i = 0; i < pendingChannels.size(); i++) {
        shared_ptr<VirtualChannel> channel = pendingChannels[i];
        if (channel && channel->channelNumber() == channelNumber) {
            pendingChannels.erase(pendingChannels.begin() + i);
            return channel;
        }
    }
    return shared_ptr<VirtualChannel>();
}

shared_ptr<VirtualChannel> DataChannelPrivate::peekChannel(uint32_t channelNumber)
{
    if (isBroken()) {
        return shared_ptr<VirtualChannel>();
    }
    for (int i = 0; i < pendingChannels.size(); i++) {
        shared_ptr<VirtualChannel> channel = pendingChannels[i];
        if (channel && channel->channelNumber() == channelNumber) {
            return channel;
        }
    }
    return shared_ptr<VirtualChannel>();
}

string DataChannelPrivate::recvPacket()
{
    if (receivingQueue.isEmpty() && error != DataChannel::NoError) {
        return string();
    }
    string packet = receivingQueue.get();
    if (packet.empty()) {
        return string();
    }
    if (slowDownRequested && receivingQueue.size() <= (receivingQueue.capacity() / 2)) {
        slowDownRequested = false;
        sendPacketRaw(CommandChannelNumber, packGoThroughRequest(), BlockFlag::NonBlock);
    }
    return packet;
}

bool DataChannelPrivate::sendPacket(const string &packet, bool waitSent)
{
    if (!goThrough.tryWait()) {
        return false;
    }
    return sendPacketRaw(DataChannelNumber, packet, waitSent ? BlockFlag::Block_Until_Sent : BlockFlag::Block_And_Not_Wait_Sent);
}

bool DataChannelPrivate::sendPacket(string &&packet, bool waitSent)
{
    if (!goThrough.tryWait()) {
        return false;
    }
    return sendPacketRaw(DataChannelNumber, std::move(packet),
                         waitSent ? BlockFlag::Block_Until_Sent : BlockFlag::Block_And_Not_Wait_Sent);
}

bool DataChannelPrivate::sendPacketAsync(const string &packet)
{
    return sendPacketRaw(DataChannelNumber, packet, BlockFlag::NonBlock);
}

bool DataChannelPrivate::sendPacketAsync(string &&packet)
{
    return sendPacketRaw(DataChannelNumber, std::move(packet), BlockFlag::NonBlock);
}

bool DataChannelPrivate::handleCommand(const string &packet)
{
    uint8_t command;
    uint32_t channelNumber;
    bool isCommand = unpackCommand(packet, &command, &channelNumber);
    if (!isCommand) {
        ngWarning() << "invalid command.";
        return false;
    }
    if (command == MAKE_CHANNEL_REQUEST) {
#ifdef DEBUG_PROTOCOL
        ngDebug() << "make channel request:" << channelNumber;
#endif
        if (subChannels.find(channelNumber) != subChannels.end()) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "the peer is making an exists channel channel:" << channelNumber;
#endif
            return false;
        }
        shared_ptr<VirtualChannel> channel = makeChannelInternal(DataChannelPole::NegativePole, channelNumber);
        sendPacketRaw(CommandChannelNumber, packChannelMadeRequest(channelNumber), BlockFlag::NonBlock);
        pendingChannels.push_back(channel);
        pendingChannelsNotEmpty.notify();
        return true;
    } else if (command == CHANNEL_MADE_REQUEST) {
#ifdef DEBUG_PROTOCOL
        ngDebug() << "channel made request:" << channelNumber;
#endif
        if (subChannels.find(channelNumber) != subChannels.end()) {
            weak_ptr<VirtualChannel> channel = subChannels.at(channelNumber);
            if (channel.expired()) {
                subChannels.erase(channelNumber);
            } else {
                return true;
            }
        }
#ifdef DEBUG_PROTOCOL
        ngDebug() << "channel is gone." << channelNumber;
#endif
        // the channel is open by me and then closed quickly...
        sendPacketRaw(CommandChannelNumber, packDestoryChannelRequest(channelNumber), BlockFlag::NonBlock);
        return true;
    } else if (command == DESTROY_CHANNEL_REQUEST) {
#ifdef DEBUG_PROTOCOL
        ngDebug() << "destroy channel request:" << channelNumber;
#endif
        shared_ptr<VirtualChannel> strong = peekChannel(channelNumber); // not remove channel from pending channels.
        if (!strong && subChannels.find(channelNumber) != subChannels.end()) {
            weak_ptr<VirtualChannel> channel = subChannels.at(channelNumber);
            cleanChannel(channelNumber, false);
            if (!channel.expired()) {
                strong = channel.lock();
            }
        }
        if (strong) {
            strong->d_func()->parentChannel = nullptr;
            // the receiving queue is still ok after aborted.
            strong->d_func()->abort(DataChannel::RemotePeerClosedError);
        }
        return true;
    } else if (command == SLOW_DOWN_REQUEST) {
        goThrough.close();
        return true;
    } else if (command == GO_THROUGH_REQUEST) {
        goThrough.open();
        return true;
    } else if (command == KEEPALIVE_REQUEST) {
        return true;
    } else if (command < 32) {
        // if command < 32, this command must be processed.
        ngWarning() << "unknown command.";
        return false;
    } else {
        ngInfo() << "unknown optional command, you might upgrade your qtng.";
        return true;
    }
}

void DataChannelPrivate::notifyChannelClose(uint32_t channelNumber)
{
    if (error != DataChannel::NoError) {
        return;
    }
    sendPacketRaw(CommandChannelNumber, packDestoryChannelRequest(channelNumber), BlockFlag::NonBlock);
}

SocketChannelPrivate::SocketChannelPrivate(shared_ptr<SocketLike> connection, DataChannelPole pole,
                                           SocketChannel *parent)
    : DataChannelPrivate(pole, parent)
    , connection(connection)
    , sendingQueue(64 * 1024)
    , operations(new CoroutineGroup())
    , _maxPayloadSize(DefaultPacketSize - sizeof(uint32_t) * 2)
    , _payloadSizeHint(DefaultPayloadSize)  // tcp fragment size.
    , lastActiveTimestamp(utils::DateTime::currentMSecsSinceEpoch())
    , lastKeepaliveTimestamp(lastActiveTimestamp)
    , keepaliveTimeout(-1)
    , keepaliveInterval(1000 * 2)
{
    // connection->setOption(Socket::LowDelayOption, true);
    // connection->setOption(Socket::KeepAliveOption, false);  // we do it!
    operations->spawnWithName("receiving", [this] { this->doReceive(); });
    operations->spawnWithName("sending", [this] { this->doSend(); });
    operations->spawnWithName("keepalive", [this] { this->doKeepalive(); });
}

SocketChannelPrivate::~SocketChannelPrivate()
{
    SocketChannelPrivate::abort(DataChannel::UserShutdown);
    delete operations;
}

bool SocketChannelPrivate::sendPacketRaw(uint32_t channelNumber, string packet, BlockFlag blocking)
{
    if (error != DataChannel::NoError || packet.empty()) {
        return false;
    }
    if (static_cast<uint32_t>(packet.size()) > _maxPayloadSize) {
#ifdef DEBUG_PROTOCOL
        ngDebug() << "the packet size is too large." << packet.size() << _maxPayloadSize;
#endif
        return false;
    }
    switch (blocking) {
    case BlockFlag::NonBlock:
        sendingQueue.putForcedly(
                WritingPacket(channelNumber, std::move(packet), shared_ptr<ValueEvent<bool>>()));
        return true;
    case BlockFlag::Block_And_Not_Wait_Sent:
        sendingQueue.put(WritingPacket(channelNumber, std::move(packet), shared_ptr<ValueEvent<bool>>()));
        return true;
    case BlockFlag::Block_Until_Sent: {
        shared_ptr<ValueEvent<bool>> done(new ValueEvent<bool>());
        sendingQueue.put(WritingPacket(channelNumber, std::move(packet), done));
        bool success = done->tryWait();
        return success;
    }
    default:
        NG_UNREACHABLE();
        break;
    }
}

void SocketChannelPrivate::doSend()
{
    const int maxSendSize = 64 * 1024;
    int count = 0;
    string buf(maxSendSize, '\0');
    while (true) {
        vector<WritingPacket> writingPackets;
        bool sendSucceeded = false;
        auto clean = shared_ptr<void>(nullptr, [&writingPackets, &sendSucceeded](void *) {
            if (!sendSucceeded) {
                for (WritingPacket &writingPacket : writingPackets) {
                    if (writingPacket.done) {
                        writingPacket.done->send(false);
                    }
                }
            }
        });
        try {
            WritingPacket writingPacket = sendingQueue.get();
            if (!writingPacket.isValid()) {
                assert(error != DataChannel::NoError);
                return;
            }
            writingPackets.push_back(std::move(writingPacket));
#define CHANNEL_HEAD_SIZE (sizeof(uint32_t) + sizeof(uint32_t))
            count = CHANNEL_HEAD_SIZE + writingPackets.back().packet.size();
            while (count + CHANNEL_HEAD_SIZE < maxSendSize && !sendingQueue.isEmpty()) {
                WritingPacket writingPacket = sendingQueue.get();
                if (!writingPacket.isValid()) {
                    break;
                }
                if (count + CHANNEL_HEAD_SIZE + writingPacket.packet.size() > maxSendSize) {
                    sendingQueue.returnsForcely(std::move(writingPacket));
                    break;
                }
                count += CHANNEL_HEAD_SIZE + writingPacket.packet.size();
                writingPackets.push_back(std::move(writingPacket));
            }
        } catch (CoroutineExitException) {
            assert(error != DataChannel::NoError);
            return;
        } catch (...) {
            return abort(DataChannel::UnknownError);
        }
        if (error != DataChannel::NoError) {
            return;
        }
        buf.reserve(count);
        char *p = &buf[0];
        for (WritingPacket &writingPacket : writingPackets) {
            ngToBigEndian<uint32_t>(static_cast<uint32_t>(writingPacket.packet.size()), p);
            p += sizeof(uint32_t);
            ngToBigEndian<uint32_t>(writingPacket.channelNumber, p);
            p += sizeof(uint32_t);
            memcpy(p, writingPacket.packet.data(), writingPacket.packet.size());
            p += writingPacket.packet.size();
        }

        int sentBytes;
        try {
            sentBytes = connection->sendall(buf.data(), count);
        } catch (CoroutineExitException) {
            assert(error != DataChannel::NoError);
            return;
        } catch (...) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "unhandled exception while sending packet.";
#endif
            return abort(DataChannel::UnknownError);
        }

        if (sentBytes == count) {
            sendSucceeded = true;
            for (WritingPacket &writingPacket : writingPackets) {
                if (writingPacket.done) {
                    writingPacket.done->send(true);
                }
            }
            lastKeepaliveTimestamp = utils::DateTime::currentMSecsSinceEpoch();
        } else {
            return abort(DataChannel::SendingError);
        }
    }
}

void SocketChannelPrivate::doReceive()
{
    const size_t headerSize = sizeof(uint32_t) + sizeof(uint32_t);
    uint32_t payloadSize;
    uint32_t channelNumber;
    string payload;
    while (true) {
        try {
            const string &header = connection->recvall(headerSize);
            if (header.size() != headerSize) {
                return abort(DataChannel::ReceivingError);
            }
            payloadSize = ngFromBigEndian<uint32_t>(header.data());
            channelNumber = ngFromBigEndian<uint32_t>(header.data() + sizeof(uint32_t));
            if (payloadSize > _maxPayloadSize) {
#ifdef DEBUG_PROTOCOL
                ngDebug()
                        << utils::formatMessage("packetSize %1 is larger than %2",
                                {utils::number(payloadSize), utils::number(_maxPayloadSize)});
#endif
                return abort(DataChannel::PakcetTooLarge);
            }
            payload = connection->recvall(static_cast<int32_t>(payloadSize));
            if (payload.size() != static_cast<int>(payloadSize)) {
                ngDebug() << "invalid packet does not fit packet size:" << payloadSize << payload.size();
                return abort(DataChannel::InvalidPacket);
            }
        } catch (CoroutineExitException) {
            assert(error != DataChannel::NoError);
            return;
        } catch (...) {
            return abort(DataChannel::UnknownError);
        }
        DataChannel::ChannelError handlePacketResult =
                handleIncomingPacket(channelNumber, std::move(payload));
        if (handlePacketResult != DataChannel::NoError) {
            return abort(handlePacketResult);
        }
        lastActiveTimestamp = utils::DateTime::currentMSecsSinceEpoch();
    }
}

void SocketChannelPrivate::doKeepalive()
{
    while (true) {
        Coroutine::sleep(0.5f);
        int64_t now = utils::DateTime::currentMSecsSinceEpoch();
        // now and lastActiveTimestamp both are unsigned int, we should check which is larger before apply minus
        // operator to them.
        if (keepaliveTimeout > 0 && now > lastActiveTimestamp && (now - lastActiveTimestamp > keepaliveTimeout)) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "channel is timeout." << connection->peerAddressURI() << "receivingQueue size:" << receivingQueue.size();
            for (shared_ptr<VirtualChannel> channel : subChannels) {
                ngDebug() << "sub channel:" << channel->channelNumber() << "receivingQueue size:" << channel->d_func()->receivingQueue.size();
            }
#endif
            return abort(DataChannel::KeepaliveTimeoutError);
        }
        // now and lastKeepaliveTimestamp both are unsigned int, we should check which is larger before apply minus
        // operator to them.
        if (now > lastKeepaliveTimestamp && (now - lastKeepaliveTimestamp > keepaliveInterval)
            && sendingQueue.isEmpty()) {
            lastKeepaliveTimestamp = now;
            shared_ptr<ValueEvent<bool>> done;
#ifdef DEBUG_PROTOCOL
            ngDebug() << "sending keepalive packet." << connection->peerAddressURI();
#endif
            sendingQueue.putForcedly(WritingPacket(CommandChannelNumber, packKeepaliveRequest(), done));
        }
    }
}

void SocketChannelPrivate::abort(DataChannel::ChannelError reason)
{
    if (error != DataChannel::NoError) {
        return;
    }
    error = reason;
#ifdef DEBUG_PROTOCOL
    ngDebug() << "socket data channel abort:" << error;
#endif
    Coroutine *current = Coroutine::current();
    connection->abort();

    while (!sendingQueue.isEmpty()) {
        WritingPacket writingPacket = sendingQueue.get();
        if (writingPacket.done) {
            writingPacket.done->send(false);
        }
    }
    if (operations->get("receiving").get() != current) {
        operations->kill("receiving");
    }
    if (operations->get("sending").get() != current) {
        operations->kill("sending");
    }
    if (operations->get("keepalive").get() != current) {
        operations->kill("keepalive");
    }
    DataChannelPrivate::abort(reason);
}

bool SocketChannelPrivate::isBroken() const
{
    return error != DataChannel::NoError || !connection->isValid();
}

static inline bool alwayTrue(const string &)
{
    return true;
}

void SocketChannelPrivate::cleanChannel(uint32_t channelNumber, bool sendDestroyPacket)
{
    int found = subChannels.erase(channelNumber);
    if (found <= 0) {
        return;
    }
    if (sendDestroyPacket) {
        notifyChannelClose(channelNumber);
    }
    cleanSendingPacket(channelNumber, alwayTrue);
}

void SocketChannelPrivate::cleanSendingPacket(uint32_t subChannelNumber,
                                              function<bool(const string &)> subCheckPacket)
{
    vector<WritingPacket> reserved;
    while (!sendingQueue.isEmpty()) {
        WritingPacket writingPacket = sendingQueue.get();
        if (writingPacket.channelNumber == subChannelNumber && subCheckPacket(writingPacket.packet)) {
            if (writingPacket.done) {
                writingPacket.done.get()->send(false);
            }
        } else {
            reserved.push_back(std::move(writingPacket));
        }
    }
    for (WritingPacket &writingPacket : reserved) {
        sendingQueue.putForcedly(std::move(writingPacket));
    }
}

uint32_t SocketChannelPrivate::maxPayloadSize() const
{
    return _maxPayloadSize;
}

uint32_t SocketChannelPrivate::payloadSizeHint() const
{
    return _payloadSizeHint;
}

uint32_t SocketChannelPrivate::headerSize() const
{
    return static_cast<int>(sizeof(uint32_t) + sizeof(uint32_t));
}

shared_ptr<SocketLike> SocketChannelPrivate::getBackend() const
{
    return connection;
}

VirtualChannelPrivate::VirtualChannelPrivate(DataChannel *parentChannel, DataChannelPole pole, uint32_t channelNumber,
                                             VirtualChannel *parent)
    : DataChannelPrivate(pole, parent)
    , parentChannel(parentChannel)
    , channelNumber(channelNumber)
{
}

VirtualChannelPrivate::~VirtualChannelPrivate()
{
    VirtualChannelPrivate::abort(DataChannel::UserShutdown);
}

bool VirtualChannelPrivate::sendPacketRaw(uint32_t channelNumber, string packet, BlockFlag blocking)
{
    if (error != DataChannel::NoError || !parentChannel || packet.empty()) {
#ifdef DEBUG_PROTOCOL
        ngDebug() << "the packet is empty?" << (error != DataChannel::NoError) << (parentChannel == nullptr)
                   << packet.empty();
#endif
        return false;
    }
    uint8_t header[sizeof(uint32_t)];
    ngToBigEndian(channelNumber, header);
    string data;
    data.reserve(packet.size() + static_cast<int>(sizeof(uint32_t)));
    data.append(reinterpret_cast<char *>(header), sizeof(uint32_t));
    data.append(packet);
    return getPrivateHelper(parentChannel)->sendPacketRaw(this->channelNumber, std::move(data), blocking);
}

void VirtualChannelPrivate::abort(DataChannel::ChannelError reason)
{
    if (error != DataChannel::NoError) {
        return;
    }
    error = reason;
    if (parentChannel) {
        getPrivateHelper(parentChannel)->cleanChannel(channelNumber, true);
    }
    DataChannelPrivate::abort(reason);
}

void VirtualChannelPrivate::cleanChannel(uint32_t channelNumber, bool sendDestroyPacket)
{
    int found = subChannels.erase(channelNumber);
    if (error != DataChannel::NoError || !parentChannel || found <= 0) {
        return;
    }
    if (sendDestroyPacket) {
        notifyChannelClose(channelNumber);
    }
    getPrivateHelper(parentChannel)
            ->cleanSendingPacket(this->channelNumber, [channelNumber](const string &packet) -> bool {
                const int headerSize = sizeof(uint32_t);
                if (packet.size() < headerSize) {
                    return false;
                }
                uint32_t channelNumberInPacket = ngFromBigEndian<uint32_t>(packet.data());
                return channelNumberInPacket == channelNumber;
            });
}

void VirtualChannelPrivate::cleanSendingPacket(uint32_t subChannelNumber,
                                               function<bool(const string &packet)> subCheckPacket)
{
    if (error != DataChannel::NoError || !parentChannel) {
        return;
    }
    getPrivateHelper(parentChannel)
            ->cleanSendingPacket(this->channelNumber, [subChannelNumber, subCheckPacket](const string &packet) {
                const int headerSize = sizeof(uint32_t);
                if (packet.size() < headerSize) {
                    return false;
                }
                uint32_t channelNumberInPacket = ngFromBigEndian<uint32_t>(packet.data());
                if (channelNumberInPacket != subChannelNumber) {
                    return false;
                }
                return subCheckPacket(packet.substr(headerSize));
            });
}

bool VirtualChannelPrivate::isBroken() const
{
    return error != DataChannel::NoError || !parentChannel || parentChannel->isBroken();
}

uint32_t VirtualChannelPrivate::maxPayloadSize() const
{
    if (isBroken()) {
        return 1400;
    } else {
        return parentChannel->maxPayloadSize() - sizeof(uint32_t);
    }
}

uint32_t VirtualChannelPrivate::payloadSizeHint() const
{
    if (isBroken()) {
        return 1400;
    } else {
        return parentChannel->payloadSizeHint() - sizeof(uint32_t);
    }
}

uint32_t VirtualChannelPrivate::headerSize() const
{
    return sizeof(uint32_t);
}

shared_ptr<SocketLike> VirtualChannelPrivate::getBackend() const
{
    if (error != DataChannel::NoError || !parentChannel) {
        return shared_ptr<SocketLike>();
    }
    return getPrivateHelper(parentChannel)->getBackend();
}

SocketChannel::SocketChannel(shared_ptr<Socket> connection, DataChannelPole pole)
    : DataChannel(new SocketChannelPrivate(asSocketLike(connection), pole, this))
{
}

#ifndef QTNG_NO_CRYPTO
SocketChannel::SocketChannel(shared_ptr<SslSocket> connection, DataChannelPole pole)
    : DataChannel(new SocketChannelPrivate(asSocketLike(connection), pole, this))
{
}
#endif

SocketChannel::SocketChannel(shared_ptr<KcpSocket> connection, DataChannelPole pole)
    : DataChannel(new SocketChannelPrivate(asSocketLike(connection), pole, this))
{
}

SocketChannel::SocketChannel(shared_ptr<SocketLike> connection, DataChannelPole pole)
    : DataChannel(new SocketChannelPrivate(connection, pole, this))
{
}

void SocketChannel::setMaxPacketSize(uint32_t size)
{
    NG_D(SocketChannel);
    if (size == 0) {
        size = DefaultPacketSize;
    } else if (size < 64) {
        ngWarning() << "the max packet size of DataChannel should not lesser than 64.";
        return;
    }
    d->_maxPayloadSize = size - sizeof(uint32_t) - sizeof(uint32_t);
    d->_payloadSizeHint = min(d->_payloadSizeHint, d->_maxPayloadSize);
}

void SocketChannel::setPayloadSizeHint(uint32_t payloadSizeHint)
{
    NG_D(SocketChannel);
    if (payloadSizeHint == 0) {
        payloadSizeHint = DefaultPayloadSize;
    } else if (payloadSizeHint < 64) {
        ngWarning() << "the payload size hint of DataChannel should not lesser than 64.";
        return;
    }
    d->_payloadSizeHint = min(payloadSizeHint, d->_maxPayloadSize);
}

void SocketChannel::setKeepaliveTimeout(float timeout)
{
    NG_D(SocketChannel);
    if (timeout > 0) {
        d->keepaliveTimeout = static_cast<int64_t>(timeout * 1000);
        if (d->keepaliveTimeout < 1000) {
            d->keepaliveTimeout = 1000;
        }
    } else {
        d->keepaliveTimeout = -1;
    }
}

float SocketChannel::keepaliveTimeout() const
{
    NG_D(const SocketChannel);
    return static_cast<float>(d->keepaliveTimeout) / 1000;
}

void SocketChannel::setKeepaliveInterval(float keepaliveInterval)
{
    NG_D(SocketChannel);
    d->keepaliveInterval = static_cast<int64_t>(keepaliveInterval * 1000);
    if (d->keepaliveInterval < 200) {
        d->keepaliveInterval = 200;
    }
}

float SocketChannel::keepaliveInterval() const
{
    NG_D(const SocketChannel);
    return static_cast<float>(d->keepaliveInterval) / 1000;
}

uint32_t SocketChannel::sendingQueueSize() const
{
    NG_D(const SocketChannel);
    return d->sendingQueue.size();
}

shared_ptr<SocketLike> SocketChannel::connection() const
{
    NG_D(const SocketChannel);
    return d->connection;
}

VirtualChannel::VirtualChannel(DataChannel *parentChannel, DataChannelPole pole, uint32_t channelNumber)
    : DataChannel(new VirtualChannelPrivate(parentChannel, pole, channelNumber, this))
{
}

uint32_t VirtualChannel::channelNumber() const
{
    NG_D(const VirtualChannel);
    return d->channelNumber;
}

DataChannel::DataChannel(DataChannelPrivate *d)
    : d_ptr(d)
{
}

DataChannel::~DataChannel()
{
    delete d_ptr;
}

bool DataChannel::isBroken() const
{
    NG_D(const DataChannel);
    return d->isBroken();
}

bool DataChannel::sendPacket(const string &packet, bool waitSent/* = true*/)
{
    NG_D(DataChannel);
    return d->sendPacket(packet, waitSent);
}

bool DataChannel::sendPacket(string &&packet, bool waitSent/* = true*/)
{
    NG_D(DataChannel);
    return d->sendPacket(std::move(packet), waitSent);
}

bool DataChannel::sendPacketAsync(const string &packet)
{
    NG_D(DataChannel);
    return d->sendPacketAsync(packet);
}

bool DataChannel::sendPacketAsync(string &&packet)
{
    NG_D(DataChannel);
    return d->sendPacketAsync(std::move(packet));
}

string DataChannel::recvPacket()
{
    NG_D(DataChannel);
    return d->recvPacket();
}

void DataChannel::abort()
{
    NG_D(DataChannel);
    d->abort(DataChannel::UserShutdown);
}

shared_ptr<VirtualChannel> DataChannel::makeChannel()
{
    NG_D(DataChannel);
    return d->makeChannel();
}

shared_ptr<VirtualChannel> DataChannel::takeChannel()
{
    NG_D(DataChannel);
    return d->takeChannel();
}

shared_ptr<VirtualChannel> DataChannel::takeChannel(uint32_t channelNumber)
{
    NG_D(DataChannel);
    return d->takeChannel(channelNumber);
}

DataChannel::ChannelError DataChannel::error() const
{
    NG_D(const DataChannel);
    return d->error;
}

string DataChannel::errorString() const
{
    NG_D(const DataChannel);
    switch (d->error) {
    case RemotePeerClosedError:
        return "The remote peer closed the connection";
    case KeepaliveTimeoutError:
        return "The remote peer didn't send keepalive packet for a long time.";
    case ReceivingError:
        return "Can not receive packet from remote peer";
    case SendingError:
        return "Can not send packet to remote peer.";
    case InvalidPacket:
        return "Can not parse packet header.";
    case InvalidCommand:
        return "Can not parse command or unknown command.";
    case UserShutdown:
        return "Programmer shutdown channel manually.";
    case PluggedChannelError:
        return "The plugged channel has error.";
    case PakcetTooLarge:
        return "The packet is too large.";
    case UnknownError:
        return "Caught unknown error.";
    case ProgrammingError:
        return "The QtNetwork programmer do a stupid thing.";
    case NoError:
        return string();
    default:
        NG_UNREACHABLE();
    }
}

string DataChannel::toString() const
{
    NG_D(const DataChannel);
    return d->toString();
}

uint32_t DataChannel::maxPacketSize() const
{
    NG_D(const DataChannel);
    return d->maxPayloadSize() + d->headerSize();
}

uint32_t DataChannel::maxPayloadSize() const
{
    NG_D(const DataChannel);
    return d->maxPayloadSize();
}

uint32_t DataChannel::payloadSizeHint() const
{
    NG_D(const DataChannel);
    return d->payloadSizeHint();
}

void DataChannel::setCapacity(uint32_t capacity)
{
    NG_D(DataChannel);
    d->receivingQueue.setCapacity(capacity);
    //    SocketChannelPrivate *scp = dynamic_cast<SocketChannelPrivate*>(d);
    //    if (scp) {
    //        scp->sendingQueue.setCapacity(capacity);
    //    }
}

uint32_t DataChannel::capacity() const
{
    NG_D(const DataChannel);
    return d->receivingQueue.capacity();
}

uint32_t DataChannel::receivingQueueSize() const
{
    NG_D(const DataChannel);
    return d->receivingQueue.size();
}

DataChannelPole DataChannel::pole() const
{
    NG_D(const DataChannel);
    return d->pole;
}

void DataChannel::setName(const string &name)
{
    NG_D(DataChannel);
    d->name = name;
}

string DataChannel::name() const
{
    NG_D(const DataChannel);
    return d->name;
}

namespace {

class DataChannelSocketLikeImpl : public SocketLike
{
public:
    DataChannelSocketLikeImpl(shared_ptr<DataChannel> channel);
public:
    virtual Socket::SocketError error() const override;
    virtual string errorString() const override;
    virtual bool isValid() const override;
    virtual HostAddress localAddress() const override;
    virtual uint16_t localPort() const override;
    virtual HostAddress peerAddress() const override;
    virtual string peerName() const override;
    virtual uint16_t peerPort() const override;
    virtual intptr_t fileno() const override;
    virtual Socket::SocketType type() const override;
    virtual Socket::SocketState state() const override;
    virtual HostAddress::NetworkLayerProtocol protocol() const override;
    virtual string localAddressURI() const override;
    virtual string peerAddressURI() const override;

    virtual Socket *acceptRaw() override;
    virtual shared_ptr<SocketLike> accept() override;
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override;
    virtual bool bind(uint16_t port, Socket::BindMode mode) override;
    virtual bool connect(const HostAddress &addr, uint16_t port) override;
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;
    virtual void abort() override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, int value) override;
    virtual int option(Socket::SocketOption option) const override;
public:
    virtual int32_t peek(char *data, int32_t size) override;
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t recv(char *data, int32_t size) override;
    virtual int32_t recvall(char *data, int32_t size) override;
    virtual int32_t send(const char *data, int32_t size) override;
    virtual int32_t sendall(const char *data, int32_t size) override;
    virtual string recv(int32_t size) override;
    virtual string recvall(int32_t size) override;
    virtual int32_t send(const string &data) override;
    virtual int32_t sendall(const string &data) override;
    virtual void close() override;
public:
    shared_ptr<SocketLike> getBackend() const;
public:
    string buf;
    shared_ptr<DataChannel> channel;
};

DataChannelSocketLikeImpl::DataChannelSocketLikeImpl(shared_ptr<DataChannel> channel)
    : channel(channel)
{
}

shared_ptr<SocketLike> DataChannelSocketLikeImpl::getBackend() const
{
    return DataChannelPrivate::getPrivateHelper(channel)->getBackend();
}

Socket::SocketError DataChannelSocketLikeImpl::error() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return Socket::UnknownSocketError;
    } else {
        return backend->error();
    }
}

string DataChannelSocketLikeImpl::errorString() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return string();
    } else {
        return backend->errorString();
    }
}

bool DataChannelSocketLikeImpl::isValid() const
{
    return !channel->isBroken();
}

HostAddress DataChannelSocketLikeImpl::localAddress() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return HostAddress();
    } else {
        return backend->localAddress();
    }
}

uint16_t DataChannelSocketLikeImpl::localPort() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return 0;
    } else {
        return backend->localPort();
    }
}

HostAddress DataChannelSocketLikeImpl::peerAddress() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return HostAddress();
    } else {
        return backend->peerAddress();
    }
}

string DataChannelSocketLikeImpl::peerName() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return string();
    } else {
        return backend->peerName();
    }
}

uint16_t DataChannelSocketLikeImpl::peerPort() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return 0;
    } else {
        return backend->peerPort();
    }
}

intptr_t DataChannelSocketLikeImpl::fileno() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return 0;
    } else {
        return backend->fileno();
    }
}

Socket::SocketType DataChannelSocketLikeImpl::type() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return Socket::UnknownSocketType;
    } else {
        return backend->type();
    }
}

Socket::SocketState DataChannelSocketLikeImpl::state() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return Socket::UnconnectedState;
    } else {
        return backend->state();
    }
}

HostAddress::NetworkLayerProtocol DataChannelSocketLikeImpl::protocol() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return HostAddress::UnknownNetworkLayerProtocol;
    } else {
        return backend->protocol();
    }
}

string DataChannelSocketLikeImpl::localAddressURI() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return string();
    } else {
        return "datachannel+" + backend->localAddressURI();
    }
}

string DataChannelSocketLikeImpl::peerAddressURI() const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return string();
    } else {
        return "datachannel+" + backend->peerAddressURI();
    }
}

Socket *DataChannelSocketLikeImpl::acceptRaw()
{
    return nullptr;
}

shared_ptr<SocketLike> DataChannelSocketLikeImpl::accept()
{
    return shared_ptr<SocketLike>();
}

bool DataChannelSocketLikeImpl::bind(const HostAddress &, uint16_t, Socket::BindMode)
{
    return false;
}

bool DataChannelSocketLikeImpl::bind(uint16_t, Socket::BindMode)
{
    return false;
}

bool DataChannelSocketLikeImpl::connect(const HostAddress &, uint16_t)
{
    return false;
}

bool DataChannelSocketLikeImpl::connect(const string &, uint16_t, shared_ptr<SocketDnsCache>)
{
    return false;
}

void DataChannelSocketLikeImpl::abort()
{
    channel->abort();
}

bool DataChannelSocketLikeImpl::listen(int)
{
    return false;
}

bool DataChannelSocketLikeImpl::setOption(Socket::SocketOption option, int value)
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return false;
    } else {
        return backend->setOption(option, value);
    }
}

int DataChannelSocketLikeImpl::option(Socket::SocketOption option) const
{
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return -1;
    } else {
        return backend->option(option);
    }
}

int32_t DataChannelSocketLikeImpl::peek(char *data, int32_t size) 
{
    if (size <= 0) {
        return -1;
    }
    int32_t len = min(size, static_cast<int32_t>(buf.size()));
    memcpy(data, buf.data(), static_cast<size_t>(len));
    return len;
}

int32_t DataChannelSocketLikeImpl::peekRaw(char *data, int32_t size)
{
    if (size <= 0) {
        return -1;
    }
    shared_ptr<SocketLike> backend = getBackend();
    if (!backend) {
        return -1;
    }
    return backend->peekRaw(data, size);
}

int32_t DataChannelSocketLikeImpl::recv(char *data, int32_t size)
{
    if (size <= 0) {
        return -1;
    }
    if (buf.empty()) {
        buf = channel->recvPacket();
        if (buf.empty()) {
            return 0;
        }
    }
    int32_t len = min(size, static_cast<int32_t>(buf.size()));
    memcpy(data, buf.data(), static_cast<size_t>(len));
    buf.erase(0, static_cast<size_t>(len));
    return len;
}

int32_t DataChannelSocketLikeImpl::recvall(char *data, int32_t size)
{
    if (size <= 0) {
        return -1;
    }
    while (buf.size() < size) {
        const string &packet = channel->recvPacket();
        if (packet.empty()) {
            break;
        }
        buf.append(packet);
    }
    int32_t len = min(size, static_cast<int32_t>(buf.size()));
    if (len > 0) {
        memcpy(data, buf.data(), static_cast<size_t>(len));
        buf.erase(0, static_cast<size_t>(len));
    }
    return len;
}

int32_t DataChannelSocketLikeImpl::send(const char *data, int32_t size)
{
    int32_t len = min<int32_t>(size, static_cast<int32_t>(channel->maxPayloadSize()));
    bool ok = channel->sendPacket(string(data, len));
    return ok ? len : -1;
}

int32_t DataChannelSocketLikeImpl::sendall(const char *data, int32_t size)
{
    int32_t count = 0;
    int32_t maxPayloadSize = static_cast<int32_t>(channel->maxPayloadSize());
    while (count < size) {
        int32_t len = min(size - count, maxPayloadSize);
        bool ok = channel->sendPacket(string(data + count, len));
        if (!ok) {
            break;
        }
        count += len;
    }
    return count;
}

string DataChannelSocketLikeImpl::recv(int32_t size)
{
    string t(size, '\0');
    int32_t len = recv(&t[0], size);
    if (len <= 0) {
        return string();
    } else {
        t.resize(len);
        return t;
    }
}

string DataChannelSocketLikeImpl::recvall(int32_t size)
{
    string t(size, '\0');
    int32_t len = recvall(&t[0], size);
    if (len <= 0) {
        return string();
    } else {
        t.resize(len);
        return t;
    }
}

int32_t DataChannelSocketLikeImpl::send(const string &data)
{
    return send(data.data(), data.size());
}

int32_t DataChannelSocketLikeImpl::sendall(const string &data)
{
    return sendall(data.data(), data.size());
}

void DataChannelSocketLikeImpl::close()
{
    channel->abort();
}

}  // namespace

void exchange(shared_ptr<DataChannel> incoming, shared_ptr<DataChannel> outgoing)
{
    DataChannelPrivate *incomingPrivate = DataChannelPrivate::getPrivateHelper(incoming);
    DataChannelPrivate *outgoingPrivate = DataChannelPrivate::getPrivateHelper(outgoing);

    while (!incomingPrivate->receivingQueue.isEmpty()) {
        string packet = incomingPrivate->receivingQueue.get();
        outgoingPrivate->sendPacketRaw(DataChannelNumber, std::move(packet), BlockFlag::NonBlock);
    }
    while (!outgoingPrivate->receivingQueue.isEmpty()) {
        string packet = outgoingPrivate->receivingQueue.get();
        incomingPrivate->sendPacketRaw(DataChannelNumber, std::move(packet), BlockFlag::NonBlock);
    }

    incomingPrivate->pluggedChannel = outgoing;
    outgoingPrivate->pluggedChannel = incoming;
    try {
        // the receiving queue of incoming and outgoing is always empty while exchanging.
        // if not, may be one of those peers is aborted. then we quit.
        while (!incoming->recvPacket().empty()) { }
        while (!outgoing->recvPacket().empty()) { }
    } catch (...) {
        incoming->abort();
        outgoing->abort();
        throw;
    }
}

shared_ptr<SocketLike> asSocketLike(shared_ptr<DataChannel> channel)
{
    return make_shared<DataChannelSocketLikeImpl>(channel);
}

}  // namespace qtng
