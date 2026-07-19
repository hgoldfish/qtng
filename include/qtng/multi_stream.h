#ifndef QTNG_MULTI_STREAM_H
#define QTNG_MULTI_STREAM_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/socket.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

enum MultiStreamPole {
    MultiStreamPositivePole = 1,
    MultiStreamNegativePole = -1,
};

// Wire RESET reason (also exposed after a peer reset for retry policy).
enum MultiStreamResetCode : std::uint32_t {
    MultiStreamResetNormalClose = 0,  // graceful close()
    MultiStreamResetAbort = 1,        // application abort()
    MultiStreamResetProtocolError = 2,
    MultiStreamResetRefused = 3,  // unknown / mismatched stream
};

class MultiStreamSlave;
class MultiStreamMasterPrivate;
class MultiStreamSlavePrivate;

class MultiStreamMaster
{
    NG_DISABLE_COPY(MultiStreamMaster)
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
    MultiStreamMaster(std::shared_ptr<Socket> socket, MultiStreamPole pole);
#ifndef QTNG_NO_CRYPTO
    MultiStreamMaster(std::shared_ptr<class SslSocket> socket, MultiStreamPole pole);
#endif
    MultiStreamMaster(std::shared_ptr<class KcpSocket> socket, MultiStreamPole pole);
    MultiStreamMaster(std::shared_ptr<SocketLike> socket, MultiStreamPole pole);
    ~MultiStreamMaster();
public:
    StreamError error() const;
    std::string errorString() const;
    std::string toString() const;
    MultiStreamPole pole() const;
    void setName(const std::string &name);
    std::string name() const;

    bool isBroken() const;
    void abort();

    std::shared_ptr<MultiStreamSlave> makeSlave();
    std::shared_ptr<MultiStreamSlave> takeSlave();
    std::shared_ptr<MultiStreamSlave> takeSlave(std::uint32_t streamNumber);

    void setMaxPacketSize(std::uint32_t size);
    std::uint32_t maxPacketSize() const;
    std::uint32_t maxPayloadSize() const;
    void setPayloadSizeHint(std::uint32_t payloadSizeHint);
    std::uint32_t payloadSizeHint() const;

    void setSlaveReceivingCapacity(std::uint32_t bytes);
    std::uint32_t slaveReceivingCapacity() const;
    void setSlaveSendingCapacity(std::uint32_t bytes);
    std::uint32_t slaveSendingCapacity() const;

    void setKeepaliveTimeout(float timeout);
    float keepaliveTimeout() const;
    void setKeepaliveInterval(float keepaliveInterval);
    float keepaliveInterval() const;

    std::uint32_t sendingQueueSize() const;
    std::shared_ptr<SocketLike> connection() const;
private:
    MultiStreamMasterPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(MultiStreamMaster)
    friend class MultiStreamSlavePrivate;
};

class MultiStreamSlave
{
    NG_DISABLE_COPY(MultiStreamSlave)
public:
    std::uint32_t streamNumber() const;
    MultiStreamMaster::StreamError error() const;
    std::string errorString() const;
    std::string toString() const;
    MultiStreamPole pole() const;
    void setName(const std::string &name);
    std::string name() const;

    bool isBroken() const;
    bool isClosing() const;
    // Graceful close: flush queued sends, discard received packets, then send RESET(NormalClose).
    void close();
    // Hard teardown: drop send/receive queues and send RESET(Abort) immediately.
    void abort();

    // Peer RESET reason when error() == RemotePeerClosedError; otherwise undefined.
    MultiStreamResetCode resetCode() const;

    bool sendPacket(const std::string &packet, bool waitSent = true);
    bool sendPacket(std::string &&packet, bool waitSent = true);
    bool sendPacketAsync(const std::string &packet);
    bool sendPacketAsync(std::string &&packet);
    std::string recvPacket();

    std::uint32_t maxPacketSize() const;
    std::uint32_t maxPayloadSize() const;
    std::uint32_t payloadSizeHint() const;
    void setReceivingCapacity(std::uint32_t bytes);
    std::uint32_t receivingCapacity() const;
    std::uint32_t receivingQueueSize() const;

    // Larger value = higher scheduling priority. Default 0. Weight used by WRR is priority + 1.
    void setPriority(int priority);
    int priority() const;

    ~MultiStreamSlave();
protected:
    MultiStreamSlave(MultiStreamMaster *master, MultiStreamPole pole, std::uint32_t streamNumber);
private:
    MultiStreamSlavePrivate * const d_ptr;
    NG_DECLARE_PRIVATE(MultiStreamSlave)
    friend class MultiStreamMasterPrivate;
    friend class MultiStreamSlavePrivate;
};

std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<MultiStreamSlave> slave);

}  // namespace qtng

#endif  // QTNG_MULTI_STREAM_H
