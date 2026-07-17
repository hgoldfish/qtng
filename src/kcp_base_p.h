#ifndef QTNG_KCP_BASE_P_H
#define QTNG_KCP_BASE_P_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtng/kcp_base.h"
#include "qtng/coroutine_utils.h"
#include "qtng/random.h"
#include "./kcp/ikcp.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/string_utils.h"
#include <cstring>
#include "qtng/utils/logging.h"

NG_LOGGER("qtng.kcp_base");

namespace qtng {

const char PACKET_TYPE_UNCOMPRESSED_DATA = 0x01;
const char PACKET_TYPE_CREATE_MULTIPATH = 0x02;
const char PACKET_TYPE_CLOSE = 0x03;
const char PACKET_TYPE_KEEPALIVE = 0x04;

// #define DEBUG_PROTOCOL 1

template<typename Link>
class KcpBase
{
public:
    typedef typename Link::PathID LinkPathID;
    explicit KcpBase(KcpMode mode = KcpMode::Internet);
    virtual ~KcpBase();
public:
    void setMode(KcpMode mode);
    void setDebugLevel(int level);
    void setSendQueueSize(uint32_t sendQueueSize);
    uint32_t sendQueueSize() const;
    void setUdpPacketSize(uint32_t udpPacketSize);
    uint32_t udpPacketSize() const;
    uint32_t payloadSizeHint() const;
    void setTearDownTime(float secs);
    float tearDownTime() const;
    void setState(Socket::SocketState state);
    LinkPathID peerId() const;
public:
    virtual bool isValid() const = 0;
    virtual std::shared_ptr<KcpBase<Link>> accept() = 0;
    virtual std::shared_ptr<KcpBase<Link>> accept(const LinkPathID &remote) = 0;
    virtual bool canBind() = 0;
    virtual bool canConnect() = 0;

    void close();
    void abort();

    int32_t peek(char *data, int32_t size);
    virtual int32_t peekRaw(char *data, int32_t size) = 0;
    int32_t recv(char *data, int32_t size);
    int32_t recvall(char *data, int32_t size);
    int32_t send(const char *data, int32_t size);
    int32_t sendall(const char *data, int32_t size);
    std::string recv(int32_t size);
    std::string recvall(int32_t size);
    int32_t send(const std::string &data);
    int32_t sendall(const std::string &data);

    int32_t udpSend(const std::string &packet, const LinkPathID &remote)
    {
        return udpSend(packet.data(), packet.size(), remote);
    }

    static int kcp_callback(const char *buf, int len, ikcpcb *, void *user);
    static std::string makeDataPacket(uint32_t connectionId, const char *data, int32_t size);
    static std::string makeShutdownPacket(uint32_t connectionId);
    static std::string makeKeepalivePacket(uint32_t connectionId);
    static std::string makeMultiPathPacket(uint32_t connectionId);

    virtual int32_t sendRaw(const char *data, int32_t size) = 0;
    virtual int32_t udpSend(const char *data, int32_t size, const LinkPathID &remote) = 0;
    virtual bool listen(int backlog) = 0;
protected:
    int32_t send(const char *data, int32_t size, bool all);
    int32_t recv(char *data, int32_t size, bool all);
    virtual bool close(bool force) = 0;
protected:
    bool handleDatagram(const char *buf, int32_t len, const LinkPathID &remote);  // len bigger than 5
    void updateKcp();
    void updateStatus();
    virtual void doUpdate();
public:
    CoroutineGroup *operations;
    std::string errorString;
    Socket::SocketState state;
    Socket::SocketError error;

    Event sendingQueueNotFull;
    Event sendingQueueEmpty;
    Event receivingQueueNotEmpty;
    RLock kcpLock;
    Gate forceToUpdate;

    char waitToReadBuffer[65536];
    int waitToReadOffset;
    int waitToReadSize;

    const uint64_t zeroTimestamp;
    uint64_t lastActiveTimestamp;
    uint64_t lastKeepaliveTimestamp;
    uint64_t m_tearDownTime;
    ikcpcb *kcp;
    uint32_t waterLine;
    uint32_t connectionId;
    LinkPathID remoteId;
    KcpMode mode;
};

template<typename Link>
class SlaveKcpBase;

template<typename Link>
class MasterKcpBase : public KcpBase<Link>
{
public:
    typedef typename Link::PathID LinkPathID;
    explicit MasterKcpBase(std::shared_ptr<Link> link);
    virtual ~MasterKcpBase();
public:
    virtual bool isValid() const override;
    virtual bool canBind() override;
    virtual bool canConnect() override;
    virtual std::shared_ptr<KcpBase<Link>> accept() override;
    virtual std::shared_ptr<KcpBase<Link>> accept(const LinkPathID &remote) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t sendRaw(const char *data, int32_t size) override;
    virtual int32_t udpSend(const char *data, int32_t size, const LinkPathID &remote) override;
protected:
    uint32_t nextConnectionId();
    void doReceive();
    void doAccept();
    std::weak_ptr<class SlaveKcpBase<Link>> doAccept(uint32_t connectionId, const LinkPathID &remote, bool &add);
    bool startReceivingCoroutine();
public:
    friend class SlaveKcpBase<Link>;
    std::shared_ptr<Link> link;
    std::map<LinkPathID, std::weak_ptr<class SlaveKcpBase<Link>>> receiversByLinkPathID;
    std::map<uint32_t, std::weak_ptr<class SlaveKcpBase<Link>>> receiversByConnectionId;
    Queue<std::shared_ptr<KcpBase<Link>>> pendingSlaves;
};

template<typename Link>
class SlaveKcpBase : public KcpBase<Link>
{
public:
    typedef typename Link::PathID LinkPathID;
    SlaveKcpBase(MasterKcpBase<Link> *parent, const LinkPathID &remote, KcpMode mode);
    virtual ~SlaveKcpBase();
public:
    virtual bool isValid() const override;
    virtual bool canBind() override;
    virtual bool canConnect() override;
    virtual std::shared_ptr<KcpBase<Link>> accept() override;
    virtual std::shared_ptr<KcpBase<Link>> accept(const LinkPathID &remote) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t sendRaw(const char *data, int32_t size) override;
    virtual int32_t udpSend(const char *data, int32_t size, const LinkPathID &remote) override;
    virtual void doUpdate() override;
public:
    friend class MasterKcpBase<Link>;
    LinkPathID originalPathID;
    MasterKcpBase<Link> *parent;
};

template<typename Link>
KcpBase<Link>::KcpBase(KcpMode mode /* = KcpMode::Internet*/)
    : operations(new CoroutineGroup())
    , state(Socket::UnconnectedState)
    , error(Socket::NoError)
    , waitToReadOffset(0)
    , waitToReadSize(0)
    , zeroTimestamp(static_cast<uint64_t>(qtng::utils::DateTime::currentMSecsSinceEpoch()))
    , lastActiveTimestamp(zeroTimestamp)
    , lastKeepaliveTimestamp(zeroTimestamp)
    , m_tearDownTime(1000 * 30)
    , waterLine(1024)
    , connectionId(0)
    , mode(mode)
{
    kcp = ikcp_create(0, this);
    ikcp_setoutput(kcp, kcp_callback);

    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
    receivingQueueNotEmpty.clear();
    setMode(mode);
}

template<typename Link>
KcpBase<Link>::~KcpBase()
{
    delete operations;
    ikcp_release(kcp);
}

template<typename Link>
void KcpBase<Link>::setMode(KcpMode mode)
{
    this->mode = mode;
    switch (mode) {
    case KcpMode::LargeDelayInternet:
        waterLine = 512;
        ikcp_nodelay(kcp, 0, 20, 1, 1);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 1024, 1024);
        break;
    case KcpMode::Internet:
        waterLine = 256;
        ikcp_nodelay(kcp, 1, 10, 1, 1);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 1024, 1024);
        kcp->rx_minrto = 30;
        // kcp->interval = 5;
        break;
    case KcpMode::AsymmetricInternet:
        waterLine = 256;
        ikcp_nodelay(kcp, 1, 10, 1, 0);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 1024, 1024);
        kcp->rx_minrto = 30;
        // kcp->interval = 5;
        break;
    case KcpMode::FastInternet:
        waterLine = 192;
        ikcp_nodelay(kcp, 1, 10, 1, 1);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 512, 512);
        kcp->rx_minrto = 20;
        // kcp->interval = 2;
        break;
    case KcpMode::Ethernet:
        waterLine = 64;
        ikcp_nodelay(kcp, 1, 10, 1, 1);
        ikcp_setmtu(kcp, 1024 * 32);
        ikcp_wndsize(kcp, 128, 128);
        kcp->rx_minrto = 10;
        // kcp->interval = 1;
        break;
    case KcpMode::Loopback:
        waterLine = 64;
        ikcp_nodelay(kcp, 1, 10, 1, 1);
        ikcp_setmtu(kcp, 1024 * 64 - 256);
        ikcp_wndsize(kcp, 128, 128);
        kcp->rx_minrto = 5;
        // kcp->interval = 1;
        break;
    }
}

template<typename Link>
void KcpBase<Link>::setDebugLevel(int level)
{
    if (level > 0) {
        kcp->writelog = [](const char *log, struct IKCPCB *kcp, void *user) {ngDebug() << log;};
        kcp->logmask |= IKCP_LOG_IN_ACK | IKCP_LOG_OUTPUT | IKCP_LOG_IN_DATA | IKCP_LOG_IN_PROBE | IKCP_LOG_IN_WINS;
    }
}

template<typename Link>
void KcpBase<Link>::setSendQueueSize(uint32_t sendQueueSize)
{
    waterLine = sendQueueSize;
}

template<typename Link>
uint32_t KcpBase<Link>::sendQueueSize() const
{
    return waterLine;
}

template<typename Link>
void KcpBase<Link>::setUdpPacketSize(uint32_t udpPacketSize)
{
    if (udpPacketSize < 65535) {
        ikcp_setmtu(kcp, static_cast<int>(udpPacketSize));
    }
}

template<typename Link>
uint32_t KcpBase<Link>::udpPacketSize() const
{
    return kcp->mtu;
}

template<typename Link>
uint32_t KcpBase<Link>::payloadSizeHint() const
{
    return kcp->mss;
}

template<typename Link>
void KcpBase<Link>::setTearDownTime(float secs)
{
    if (secs > 0) {
        m_tearDownTime = static_cast<uint64_t>(secs * 1000);
        if (m_tearDownTime < 1000) {
            m_tearDownTime = 1000;
        }
    }
}

template<typename Link>
float KcpBase<Link>::tearDownTime() const
{
    return m_tearDownTime / 1000.0f;
}

template<typename Link>
void KcpBase<Link>::setState(Socket::SocketState state)
{
    this->state = state;
}

template<typename Link>
typename Link::PathID KcpBase<Link>::peerId() const
{
    return remoteId;
}

template<typename Link>
void KcpBase<Link>::close()
{
    close(false);
}

template<typename Link>
void KcpBase<Link>::abort()
{
    close(true);
}

template<typename Link>
int32_t KcpBase<Link>::peek(char *data, int32_t size)
{
    if (state != Socket::ConnectedState) {
        return -1;
    }
    if (waitToReadSize - waitToReadOffset > 0) {
        int32_t result = std::min(size, waitToReadSize - waitToReadOffset);
        memcpy(data, waitToReadBuffer + waitToReadOffset, result);
        return result;
    }
    
    ScopedLock<RLock> l(kcpLock);
    int peeksize = ikcp_peeksize(kcp);
    if (peeksize > 0) {
        peeksize = std::min(static_cast<int>(sizeof(waitToReadBuffer)), peeksize);
        int readBytes = ikcp_recv(kcp, waitToReadBuffer, peeksize);
        assert(readBytes == peeksize);
        waitToReadOffset = 0;
        waitToReadSize = readBytes;

        int32_t result = std::min(size, waitToReadSize);
        memcpy(data, waitToReadBuffer, result);
        return result;
    }
    return 0;
}

template<typename Link>
int32_t KcpBase<Link>::recv(char *data, int32_t size)
{
    return recv(data, size, false);
}

template<typename Link>
int32_t KcpBase<Link>::recvall(char *data, int32_t size)
{
    return recv(data, size, true);
}

template<typename Link>
int32_t KcpBase<Link>::send(const char *data, int32_t size)
{
    int32_t bytesSent = send(data, size, false);
    if (bytesSent == 0 && !isValid()) {
        return -1;
    }
    return bytesSent;
}

template<typename Link>
int32_t KcpBase<Link>::sendall(const char *data, int32_t size)
{
    return send(data, size, true);
}

template<typename Link>
std::string KcpBase<Link>::recv(int32_t size)
{
    std::string bs(size, '\0');
    int32_t bytes = recv(&bs[0], bs.size(), false);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return std::string();
}

template<typename Link>
int32_t KcpBase<Link>::recv(char *data, int32_t size, bool all)
{
    if (size <= 0) {
        return -1;
    }
    int32_t left = size;
    int32_t total = 0;
    while (true) {
        if (waitToReadSize - waitToReadOffset > 0) {
            int32_t len = std::min(left, waitToReadSize - waitToReadOffset);
            memcpy(data + total, waitToReadBuffer + waitToReadOffset, static_cast<size_t>(len));
            total += len;
            waitToReadOffset += len;
            if (!all || total >= size) {
                return total;
            }
            left -= len;
        }
        while (true) {
            {
                ScopedLock<RLock> l(kcpLock);
                int peeksize = ikcp_peeksize(kcp);
                if (peeksize > 0) {
                    peeksize = std::min(static_cast<int>(sizeof(waitToReadBuffer)), peeksize);
                    int readBytes = ikcp_recv(kcp, waitToReadBuffer, peeksize);
                    assert(readBytes == peeksize);
                    waitToReadOffset = 0;
                    waitToReadSize = readBytes;
                    break;
                }
            }
            if (state != Socket::ConnectedState) {
                error = Socket::SocketAccessError;
                errorString = "KcpBase is not connected.";
                return -1;
            }
            receivingQueueNotEmpty.clear();
            if (!receivingQueueNotEmpty.tryWait()) {
                return total > 0 ? total : -1;
            }
        }
    }
}

template<typename Link>
std::string KcpBase<Link>::recvall(int32_t size)
{
    std::string bs(size, '\0');
    int32_t bytes = recv(&bs[0], bs.size(), true);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return std::string();
}

template<typename Link>
int32_t KcpBase<Link>::send(const std::string &data)
{
    int32_t bytesSent = send(data.data(), data.size(), false);
    if (bytesSent == 0 && !isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}

template<typename Link>
int32_t KcpBase<Link>::send(const char *data, int32_t size, bool all)
{
    if (size <= 0) {
        return -1;
    }

    sendingQueueEmpty.clear();

    int32_t total = 0;
    do {
        if (state != Socket::ConnectedState) {
            error = Socket::SocketAccessError;
            errorString = "KcpBase is not connected.";
            return -1;
        }
        if (!sendingQueueNotFull.tryWait()) {
            return total > 0 ? total : -1;
        }
        int32_t nextBlockSize = std::min<int32_t>(static_cast<int32_t>(kcp->mss), size - total);
        int result;
        {
            ScopedLock<RLock> l(kcpLock);
            result = ikcp_send(kcp, data + total, nextBlockSize);
        }
        if (result < 0) {
            ngWarning() << "why ikcp_send error happened? result:" << result << "connectionId:" << connectionId;
            return total > 0 ? total : -1;
        }
        assert(result == nextBlockSize);
        updateStatus();
        total += result;
        if (!all) {
            break;
        }
    } while (total < size);

    updateKcp();
    return total;
}

template<typename Link>
int32_t KcpBase<Link>::sendall(const std::string &data)
{
    return send(data.data(), data.size(), true);
}

template<typename Link>
int KcpBase<Link>::kcp_callback(const char *buf, int len, ikcpcb *, void *user)
{
    KcpBase<Link> *p = static_cast<KcpBase<Link> *>(user);
    if (!p || !buf || len > 65535) {
        ngWarning() << "kcp_callback got invalid data.";
        return -1;
    }
    const std::string &packet = KcpBase<Link>::makeDataPacket(p->connectionId, buf, len);
    int32_t sentBytes = p->sendRaw(packet.data(), packet.size());
    if (sentBytes != packet.size()) {  // but why this happens?
        if (p->error == Socket::NoError) {
            p->error = Socket::SocketAccessError;
            p->errorString = "can not send udp packet";
        }
#ifdef DEBUG_PROTOCOL
        ngWarning() << "can not send packet to connection:" << p->connectionId;
#endif
        p->close(true);
        return -1;
    }
    return sentBytes;
}

template<typename Link>
std::string KcpBase<Link>::makeDataPacket(uint32_t connectionId, const char *data, int32_t size)
{
    std::string packet(size + 1, '\0');
    packet[0] = PACKET_TYPE_UNCOMPRESSED_DATA;
    memcpy(&packet[1], data, static_cast<size_t>(size));
    ngToBigEndian<uint32_t>(connectionId, &packet[1]);
    return packet;
}

template<typename Link>
std::string KcpBase<Link>::makeShutdownPacket(uint32_t connectionId)
{
    std::string packet(6, '\0');
    packet[0] = PACKET_TYPE_CLOSE;
    ngToBigEndian<uint32_t>(connectionId, &packet[1]);
    return packet;
}

template<typename Link>
std::string KcpBase<Link>::makeKeepalivePacket(uint32_t connectionId)
{
    std::string packet(6, '\0');
    packet[0] = PACKET_TYPE_KEEPALIVE;
    ngToBigEndian<uint32_t>(connectionId, &packet[1]);
    return packet;
}

template<typename Link>
std::string KcpBase<Link>::makeMultiPathPacket(uint32_t connectionId)
{
    std::string packet(6, '\0');
    packet[0] = PACKET_TYPE_CREATE_MULTIPATH;
    ngToBigEndian<uint32_t>(connectionId, &packet[1]);
    return packet;
}

template<typename Link>
bool KcpBase<Link>::handleDatagram(const char *buf, int32_t len, const LinkPathID &remote)
{
    switch (buf[0]) {
    case PACKET_TYPE_UNCOMPRESSED_DATA: {
        int result;
        {
            ScopedLock<RLock> l(kcpLock);
            result = ikcp_input(kcp, buf + 1, len - 1);
        }
        if (result < 0) {
            // invalid datagram
#ifdef DEBUG_PROTOCOL
            ngDebug() << "invalid datagram. kcp returns" << result;
#endif
            return false;
        }
        lastActiveTimestamp = static_cast<uint64_t>(qtng::utils::DateTime::currentMSecsSinceEpoch());
        updateKcp(); // send ack before info user layer that has receive data can let kcp faster
        receivingQueueNotEmpty.set();
        remoteId = remote;
        return true;
    }
    case PACKET_TYPE_CREATE_MULTIPATH:
        remoteId = remote;
        return true;
    case PACKET_TYPE_CLOSE:
        if (remote == remoteId) {
            close(true);
            // error for return
            return false;
        }
        // ignore if remote is not recored one
        return true;
    case PACKET_TYPE_KEEPALIVE:
        lastActiveTimestamp = static_cast<uint64_t>(qtng::utils::DateTime::currentMSecsSinceEpoch());
#ifdef DEBUG_PROTOCOL
        ngDebug() << "recv keep alive from" << connectionId << remoteId;
#endif
        remoteId = remote;
        return true;
    default:
        break;
    }
    // ignore if remote is not recored one
    return !(remote == remoteId);
}

template<typename Link>
void KcpBase<Link>::doUpdate()
{
    // in close(), state is set to Socket::UnconnectedState but error = NoError.
    while (state == Socket::ConnectedState || (state == Socket::UnconnectedState && error == Socket::NoError)) {
        uint64_t now = static_cast<uint64_t>(qtng::utils::DateTime::currentMSecsSinceEpoch());
        // now and lastActiveTimestamp both are unsigned int, we should check which is larger before apply minus
        // operator to them.
        if (now > lastActiveTimestamp && (now - lastActiveTimestamp > m_tearDownTime)
            && state == Socket::ConnectedState) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "kcp socket tear down!" << remoteId << "connectionId:" << connectionId;
#endif
            error = Socket::SocketTimeoutError;
            errorString = "kcp is timeout.";
            close(true);
            return;
        }
        uint32_t current = static_cast<uint32_t>(now - zeroTimestamp);  // impossible to overflow.
        {
            ScopedLock<RLock> l(kcpLock);
            ikcp_update(kcp,
                        current);  // ikcp_update() call ikcp_flush() and then kcp_callback(), and maybe close(true)
        }
        if (!(state == Socket::ConnectedState || (state == Socket::UnconnectedState && error == Socket::NoError))) {
            return;
        }

        // now and lastKeepaliveTimestamp both are unsigned int, we should check which is larger before apply minus
        // operator to them.
        if (now > lastKeepaliveTimestamp && (now - lastKeepaliveTimestamp > 1000 * 5)
            && state == Socket::ConnectedState) {
            const std::string &packet = KcpBase<Link>::makeKeepalivePacket(connectionId);
            if (sendRaw(packet.data(), packet.size()) != packet.size()) {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "can not send keep alive packet.";
#endif
                close(true);
                return;
            }
#ifdef DEBUG_PROTOCOL
            ngDebug() << "keep alive packet sent to" << remoteId << "connectionId:" << connectionId;
#endif
        }

        updateStatus();

        uint32_t ts = ikcp_check(kcp, current);
        uint32_t interval = ts - current;
        if (interval > 0) {
            forceToUpdate.close();
            forceToUpdate.tryWait(interval);  // timeout continue
        }
    }
}

template<typename Link>
void KcpBase<Link>::updateKcp()
{
    std::shared_ptr<Coroutine> t = operations->spawnWithName(
            "update_kcp", [this] { doUpdate(); }, false);
    kcp->updated = 0;
    forceToUpdate.open();
}

template<typename Link>
void KcpBase<Link>::updateStatus()
{
    int sendingQueueSize = ikcp_waitsnd(kcp);
    if (sendingQueueSize <= 0) {
        sendingQueueNotFull.set();
        sendingQueueEmpty.set();
    } else {
        sendingQueueEmpty.clear();
        if (static_cast<uint32_t>(sendingQueueSize) > (kcp->snd_wnd << 1)) {
            sendingQueueNotFull.clear();
        } else if (static_cast<uint32_t>(sendingQueueSize) <= waterLine) {
            sendingQueueNotFull.set();
        }
    }
}

template<typename Link>
MasterKcpBase<Link>::MasterKcpBase(std::shared_ptr<Link> link)
    : KcpBase<Link>()
    , link(link)
{
}

template<typename Link>
MasterKcpBase<Link>::~MasterKcpBase()
{
    MasterKcpBase<Link>::close(true);
}

template<typename Link>
bool MasterKcpBase<Link>::isValid() const
{
    return this->state == Socket::ConnectedState || this->state == Socket::BoundState
            || this->state == Socket::ListeningState;
}

template<typename Link>
bool MasterKcpBase<Link>::canBind()
{
    return this->state == Socket::UnconnectedState;
}

template<typename Link>
bool MasterKcpBase<Link>::canConnect()
{
    return this->state == Socket::UnconnectedState || this->state == Socket::BoundState;
}

template<typename Link>
std::shared_ptr<KcpBase<Link>> MasterKcpBase<Link>::accept()
{
    if (this->state != Socket::ListeningState) {
        return nullptr;
    }
    startReceivingCoroutine();
    return pendingSlaves.get();
}

template<typename Link>
std::shared_ptr<KcpBase<Link>> MasterKcpBase<Link>::accept(const LinkPathID &remote)
{
    if (this->state != Socket::ListeningState || remote.isNull()) {
        return nullptr;
    }
    startReceivingCoroutine();
    std::weak_ptr<SlaveKcpBase<Link>> receiverPtr = receiversByLinkPathID.at(remote);
    if (auto receiver = receiverPtr.lock()) {
        if (receiver->isValid()) {
            return receiver;
        }
    }

    std::shared_ptr<SlaveKcpBase<Link>> slave(new SlaveKcpBase<Link>(this, remote, this->mode));
    slave->updateKcp();
    receiversByLinkPathID[remote] = slave;
    // the connectionId is generated in server side. accept() is acually a connect().
    // receiversByConnectionId[slave->connectionId] = slave;
    return slave;
}

template<typename Link>
bool MasterKcpBase<Link>::close(bool force)
{
    // if `force` is true, must not block. see doUpdate()
    if (this->state == Socket::UnconnectedState) {
        return true;
    } else if (this->state == Socket::ConnectedState) {
        this->state = Socket::UnconnectedState;
        if (!force && this->error == Socket::NoError) {
            if (!this->sendingQueueEmpty.isSet()) {
                this->updateKcp();
                if (!this->sendingQueueEmpty.tryWait()) {
                    return false;
                }
            }
            const std::string &packet = KcpBase<Link>::makeShutdownPacket(this->connectionId);
            sendRaw(packet.data(), packet.size());
        }
    } else if (this->state == Socket::ListeningState) {
        this->state = Socket::UnconnectedState;
        std::map<LinkPathID, std::weak_ptr<class SlaveKcpBase<Link>>> receiversCopy(this->receiversByLinkPathID);
        this->receiversByLinkPathID.clear();
        for (const auto &item : receiversCopy) {
            if (auto receiver = item.second.lock()) {
                receiver->close(force);
            }
        }
        receiversByConnectionId.clear();
    } else {  // BoundState
        this->state = Socket::UnconnectedState;
        this->link->abort();
        return true;
    }

    while (!pendingSlaves.isEmpty()) {
        pendingSlaves.get();
    }
    pendingSlaves.put(nullptr);

    // connected and listen state would do more cleaning work.
    this->operations->killall();
    // always kill operations before release resources.

    if (force) {
        this->link->abort();
    } else {
        this->link->close();
    }
    // awake all pending recv()/send()
    this->receivingQueueNotEmpty.set();
    this->sendingQueueEmpty.set();
    this->sendingQueueNotFull.set();
#ifdef DEBUG_PROTOCOL
    ngDebug() << "MasterKcpBasePrivate::close() done. connectionId:" << this->connectionId;
#endif
    return true;
}

template<typename Link>
bool MasterKcpBase<Link>::listen(int backlog)
{
    if (this->state != Socket::BoundState || backlog <= 0) {
        return false;
    }
    this->state = Socket::ListeningState;
    pendingSlaves.setCapacity(static_cast<uint32_t>(backlog));
    return true;
}

template<typename Link>
int32_t MasterKcpBase<Link>::peekRaw(char *data, int32_t size)
{
    if (size <= 0) {
        return -1;
    }
    return KcpBase<Link>::peek(data, size);
}

template<typename Link>
int32_t MasterKcpBase<Link>::sendRaw(const char *data, int32_t size)
{
    this->lastKeepaliveTimestamp = static_cast<uint64_t>(qtng::utils::DateTime::currentMSecsSinceEpoch());
    startReceivingCoroutine();
    return this->link->sendto(data, size, this->remoteId);
}

template<typename Link>
int32_t MasterKcpBase<Link>::udpSend(const char *data, int32_t size, const LinkPathID &remote)
{
    return this->link->sendto(data, size, remote);
}

template<typename Link>
uint32_t MasterKcpBase<Link>::nextConnectionId()
{
    uint32_t id;
    do {
        const std::string bytes = randomBytes(4);
        memcpy(&id, bytes.data(), sizeof(id));
    } while (receiversByConnectionId.find(id) != receiversByConnectionId.end());
    return id;
}

template<typename Link>
void MasterKcpBase<Link>::doReceive()
{
    LinkPathID remote;
    std::string buf(1024 * 64, '\0');
    char *data = &buf[0];
    while (true) {
        int32_t len = this->link->recvfrom(data, buf.size(), remote);
        if ((len < 0)) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "kcp can not receive udp packet when do receive. len:" << len << remote;
#endif
            MasterKcpBase<Link>::close(true);
            return;
        }
        if (this->link->filter(data, &len, &remote)) {
            continue;
        }
        if (len < 5) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "got invalid kcp packet smaller than 5 bytes." << std::string(data, len);
#endif
            continue;
        }

        uint32_t connectionId = ngFromBigEndian<uint32_t>(data + 1);
        ngToBigEndian<uint32_t>(0, reinterpret_cast<uint8_t *>(data + 1));

        if (connectionId == 0) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "the kcp server side returns an invalid packet with zero connection id.";
#endif
            continue;
        }

        if (this->connectionId == 0) {
            this->connectionId = connectionId;
        } else if (connectionId != this->connectionId) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "the kcp server side returns an invalid packet with mismatched connection id.";
#endif
            continue;
        }

        ngToBigEndian<uint32_t>(0, reinterpret_cast<uint8_t *>(data + 1));
        if (!this->handleDatagram(data, static_cast<uint32_t>(len), remote)) {
            return;
        }
    }
}

template<typename Link>
void MasterKcpBase<Link>::doAccept()
{
    LinkPathID remote;
    std::string buf(1024 * 64, '\0');
    char *data = &buf[0];
    while (true) {
        int32_t len = this->link->recvfrom(data, buf.size(), remote);
        if ((len < 0)) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "kcp can not receive udp packet when do accept.";
#endif
            MasterKcpBase<Link>::close(true);
            return;
        }
        if (remote.isNull()) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "remote is not valid";
#endif
            continue;
        }
        if (this->link->filter(data, &len, &remote)) {
            continue;
        }
        if (len < 5) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "got invalid kcp packet smaller than 5 bytes.";
#endif
            continue;
        }
        uint32_t connectionId = ngFromBigEndian<uint32_t>(data + 1);
        ngToBigEndian<uint32_t>(0, reinterpret_cast<uint8_t *>(data + 1));
        bool add = false;
        std::weak_ptr<SlaveKcpBase<Link>> receiverPtr = doAccept(connectionId, remote, add);
        if (receiverPtr.expired()) {
            if (!add) {
                continue;
            }
            std::shared_ptr<SlaveKcpBase<Link>> slave(
                    new SlaveKcpBase<Link>(this, remote, this->mode));
            if (this->kcp->logmask > 0) {
                slave->setDebugLevel(1);
            }
            slave->connectionId = nextConnectionId();
            if (!slave->handleDatagram(data, static_cast<uint32_t>(len), remote)) {
                continue;
            }
#ifdef DEBUG_PROTOCOL
            ngDebug() << "new connection coming. connectionId:" << slave->connectionId << remote;
#endif
            if (!link->addSlave(remote, slave->connectionId)) {
                continue;
            }

            receiversByLinkPathID[remote] = slave;
            receiversByConnectionId[slave->connectionId] = slave;
            pendingSlaves.put(slave);
            continue;
        }
        std::shared_ptr<SlaveKcpBase<Link>> receiver = receiverPtr.lock();
        if (!receiver->handleDatagram(data, len, remote)) {
            receiver->abort();
            continue;
        }
    }
}

template<typename Link>
std::weak_ptr<SlaveKcpBase<Link>> MasterKcpBase<Link>::doAccept(uint32_t connectionId,
                                                                                   const LinkPathID &remote, bool &add)
{
    std::weak_ptr<SlaveKcpBase<Link>> receiverPtr;
    if (connectionId != 0) {  // a multipath packet.
        receiverPtr = receiversByConnectionId.at(connectionId);
        if (!!receiverPtr.lock()) {
            std::shared_ptr<SlaveKcpBase<Link>> receiver = receiverPtr.lock();
            if (connectionId != receiver->connectionId) {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "kcp client:" << remote << "sent a invalid connection id ";
#endif
                return std::weak_ptr<SlaveKcpBase<Link>>();
            }
            return receiver;
        }
        receiverPtr = receiversByLinkPathID.at(remote);
        if (!!receiverPtr.lock()) {
            std::shared_ptr<SlaveKcpBase<Link>> receiver = receiverPtr.lock();
            if (receiver->connectionId == 0) {
                // only if the slave was created by accept(host, port), we had zero id.
                // if this connectionId is unique in client. we add it to the receiversByConnectionId map.
                // if it is not, say sorry, and disable the multipath feature.
                // only happened in the newly accept(host, port) connections.
                // or remote create new conn with the same port as old, and the old packet is received.
                receiver->connectionId = connectionId;
                receiversByConnectionId[connectionId] = receiver;
                return receiver;
            }
        }

        // it must be bad packet.
        const std::string &closePacket = KcpBase<Link>::makeShutdownPacket(connectionId);
        if (this->link->sendto(closePacket.data(), closePacket.size(), remote) != closePacket.size()) {
            if (this->error == Socket::NoError) {
                this->error = Socket::SocketResourceError;
                this->errorString = "kcp can not send udp packet.";
            }
#ifdef DEBUG_PROTOCOL
            ngDebug() << this->errorString;
#endif
            MasterKcpBase<Link>::close(true);
        }
#ifdef DEBUG_PROTOCOL
        ngDebug() << "bad packet" << remote << "connectionId:" << connectionId;
#endif
        return std::weak_ptr<SlaveKcpBase<Link>>();
    }
    // at beginning, all connectionId is zero
    receiverPtr = receiversByLinkPathID.at(remote);
    if (!!receiverPtr.lock()) {
        return receiverPtr;
    }
    if (pendingSlaves.size() >= pendingSlaves.capacity()) {
        return std::weak_ptr<SlaveKcpBase<Link>>();
    }
    // not full. process new connection.
    add = true;
    return std::weak_ptr<SlaveKcpBase<Link>>();
}

template<typename Link>
bool MasterKcpBase<Link>::startReceivingCoroutine()
{
    if (this->operations->get("receiving")) {
        return true;
    }
    switch (this->state) {
    case Socket::UnconnectedState:
    case Socket::BoundState:
    case Socket::ConnectingState:
    case Socket::HostLookupState:
    case Socket::ClosingState:
        return false;
    case Socket::ConnectedState:
        this->operations->spawnWithName("receiving", [this] { doReceive(); });
        break;
    case Socket::ListeningState:
        this->operations->spawnWithName("receiving", [this] { doAccept(); });
        break;
    }
    return true;
}

template<typename Link>
SlaveKcpBase<Link>::SlaveKcpBase(MasterKcpBase<Link> *parent, const LinkPathID &remote,
                                             KcpMode mode)
    : KcpBase<Link>(mode)
    , originalPathID(remote)
    , parent(parent)
{
    this->remoteId = remote;
    this->state = Socket::ConnectedState;
}

template<typename Link>
SlaveKcpBase<Link>::~SlaveKcpBase()
{
    SlaveKcpBase<Link>::close(true);
}

template<typename Link>
bool SlaveKcpBase<Link>::isValid() const
{
    return this->state == Socket::ConnectedState && parent;
}

template<typename Link>
bool SlaveKcpBase<Link>::close(bool force)
{
    // if `force` is true, must not block. it is called by doUpdate()
    if (this->state == Socket::UnconnectedState) {
        return true;
    }
    if (this->state == Socket::ConnectedState) {
        this->state = Socket::UnconnectedState;
        if (!force && this->error == Socket::NoError) {
            if (!this->sendingQueueEmpty.isSet()) {
                this->updateKcp();
                if (!this->sendingQueueEmpty.tryWait(3000)) {
                    return false;
                }
            }
            const std::string &packet = KcpBase<Link>::makeShutdownPacket(this->connectionId);
            sendRaw(packet.data(), packet.size());
        }
    } else {  // there can be no other states.
        this->state = Socket::UnconnectedState;
    }
    this->operations->killall();
    if (parent) {
        parent->receiversByLinkPathID.erase(originalPathID);
        parent->receiversByConnectionId.erase(this->connectionId);
        if (force) {
            parent->link->abortSlave(originalPathID);
        } else {
            parent->link->closeSlave(originalPathID);
        }
        parent = nullptr;
    }

    // await all pending recv()/send()
    this->receivingQueueNotEmpty.set();
    this->sendingQueueEmpty.set();
    this->sendingQueueNotFull.set();
#ifdef DEBUG_PROTOCOL
    ngDebug() << "slave kcp closed. connetionId:" << this->connectionId;
#endif
    return true;
}

template<typename Link>
bool SlaveKcpBase<Link>::listen(int)
{
    return false;
}

template<typename Link>
std::shared_ptr<KcpBase<Link>> SlaveKcpBase<Link>::accept()
{
    return nullptr;
}

template<typename Link>
std::shared_ptr<KcpBase<Link>> SlaveKcpBase<Link>::accept(const LinkPathID &)
{
    return nullptr;
}

template<typename Link>
int32_t SlaveKcpBase<Link>::peekRaw(char *data, int32_t size)
{
    if (!parent) {
        return -1;
    }
    return this->peek(data, size);
}

template<typename Link>
int32_t SlaveKcpBase<Link>::sendRaw(const char *data, int32_t size)
{
    if (!parent) {
        return -1;
    }
    this->lastKeepaliveTimestamp = static_cast<uint64_t>(qtng::utils::DateTime::currentMSecsSinceEpoch());
    return parent->link->sendto(data, size, this->remoteId);
}

template<typename Link>
int32_t SlaveKcpBase<Link>::udpSend(const char *data, int32_t size, const LinkPathID &remote)
{
    if (!parent) {
        return -1;
    }
    return parent->link->sendto(data, size, remote);
}

template<typename Link>
void SlaveKcpBase<Link>::doUpdate()
{
    if (!parent) {
        return;
    }
    // sent first packet to let peer known its connection id
    const std::string &multiPathPacket = KcpBase<Link>::makeMultiPathPacket(this->connectionId);
    if (parent->link->sendto(multiPathPacket.data(), multiPathPacket.size(), this->remoteId) != multiPathPacket.size()) {
        if (this->error == Socket::NoError) {
            this->error = Socket::SocketResourceError;
            this->errorString = "kcp can not send udp packet.";
        }
#ifdef DEBUG_PROTOCOL
        ngDebug() << this->errorString;
#endif
        SlaveKcpBase<Link>::close(true);
        return;
    }
    KcpBase<Link>::doUpdate();
}

template<typename Link>
bool SlaveKcpBase<Link>::canBind()
{
    return false;
}

template<typename Link>
bool SlaveKcpBase<Link>::canConnect()
{
    return false;
}

template<typename Link>
class KcpBaseSocketLike : public SocketLike
{
protected:
    explicit KcpBaseSocketLike(std::shared_ptr<KcpBase<Link>> kcpBase);
public:
    ~KcpBaseSocketLike();
public:
    virtual HostAddress localAddress() const override { return HostAddress(); }
    virtual uint16_t localPort() const override { return 0; }
    virtual HostAddress peerAddress() const override { return HostAddress(); }
    virtual std::string peerName() const override { return std::string(); }
    virtual uint16_t peerPort() const override { return 0; }
    virtual intptr_t fileno() const override { return -1; }
    virtual Socket::SocketType type() const override { return Socket::KcpSocket; }

    virtual HostAddress::NetworkLayerProtocol protocol() const override
    {
        return HostAddress::UnknownNetworkLayerProtocol;
    }
    virtual std::string localAddressURI() const override { return std::string(); }
    virtual std::string peerAddressURI() const override { return std::string(); }

    virtual std::shared_ptr<SocketLike> accept() override { return std::shared_ptr<SocketLike>(); }
    virtual Socket *acceptRaw() override { return nullptr; }
    virtual bool bind(const HostAddress &address, uint16_t port = 0,
                      Socket::BindMode mode = Socket::DefaultForPlatform) override
    {
        return false;
    }
    virtual bool bind(uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform) override { return false; }
    virtual bool connect(const HostAddress &addr, uint16_t port) override { return false; }
    virtual bool connect(const std::string &hostName, uint16_t port,
                         std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>()) override
    {
        return false;
    }
    
    virtual bool setOption(Socket::SocketOption option, int value) override { return false; }
    virtual int option(Socket::SocketOption option) const override { return -1; }

    virtual Socket::SocketError error() const override;
    virtual std::string errorString() const override;
    virtual bool isValid() const override;
    virtual void abort() override;
    virtual void close() override;
    virtual Socket::SocketState state() const override;
    virtual bool listen(int backlog) override;

    virtual int32_t peek(char *data, int32_t size) override;
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t recv(char *data, int32_t size) override;
    virtual int32_t recvall(char *data, int32_t size) override;
    virtual int32_t send(const char *data, int32_t size) override;
    virtual int32_t sendall(const char *data, int32_t size) override;
    virtual std::string recv(int32_t size) override;
    virtual std::string recvall(int32_t size) override;
    virtual int32_t send(const std::string &data) override;
    virtual int32_t sendall(const std::string &data) override;
public:
    std::shared_ptr<KcpBase<Link>> kcpBase;
};

template<typename Link>
KcpBaseSocketLike<Link>::KcpBaseSocketLike(std::shared_ptr<KcpBase<Link>> kcpBase)
    : kcpBase(kcpBase)
{
}

template<typename Link>
KcpBaseSocketLike<Link>::~KcpBaseSocketLike()
{
}

template<typename Link>
Socket::SocketError KcpBaseSocketLike<Link>::error() const
{
    return kcpBase->error;
}

template<typename Link>
std::string KcpBaseSocketLike<Link>::errorString() const
{
    return kcpBase->errorString;
}

template<typename Link>
bool KcpBaseSocketLike<Link>::isValid() const
{
    return kcpBase->isValid();
}

template<typename Link>
void KcpBaseSocketLike<Link>::abort()
{
    kcpBase->abort();
}

template<typename Link>
void KcpBaseSocketLike<Link>::close()
{
    kcpBase->close();
}

template<typename Link>
Socket::SocketState KcpBaseSocketLike<Link>::state() const
{
    return kcpBase->state;
}

template<typename Link>
inline bool KcpBaseSocketLike<Link>::listen(int backlog)
{
    return kcpBase->listen(backlog);
}

template<typename Link>
int32_t KcpBaseSocketLike<Link>::peek(char *data, int32_t size)
{
    return kcpBase->peek(data, size);
}

template<typename Link>
int32_t KcpBaseSocketLike<Link>::peekRaw(char *data, int32_t size)
{
    return kcpBase->peekRaw(data, size);
}

template<typename Link>
int32_t KcpBaseSocketLike<Link>::recv(char *data, int32_t size)
{
    return kcpBase->recv(data, size);
}

template<typename Link>
int32_t KcpBaseSocketLike<Link>::recvall(char *data, int32_t size)
{
    return kcpBase->recvall(data, size);
}

template<typename Link>
int32_t KcpBaseSocketLike<Link>::send(const char *data, int32_t size)
{
    return kcpBase->send(data, size);
}

template<typename Link>
int32_t KcpBaseSocketLike<Link>::sendall(const char *data, int32_t size)
{
    return kcpBase->sendall(data, size);
}

template<typename Link>
std::string KcpBaseSocketLike<Link>::recv(int32_t size)
{
    return kcpBase->recv(size);
}

template<typename Link>
std::string KcpBaseSocketLike<Link>::recvall(int32_t size)
{
    return kcpBase->recvall(size);
}

template<typename Link>
int32_t KcpBaseSocketLike<Link>::send(const std::string &data)
{
    return kcpBase->send(data);
}

template<typename Link>
int32_t KcpBaseSocketLike<Link>::sendall(const std::string &data)
{
    return kcpBase->sendall(data);
}

}  // namespace qtng

#endif  // QTNG_KCP_BASE_P_H
