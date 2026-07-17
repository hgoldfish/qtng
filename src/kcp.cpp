#include "qtng/kcp.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtng/socket_utils.h"
#include "qtng/coroutine_utils.h"
#include "qtng/random.h"
#include "qtng/private/socket_p.h"
#include "./kcp/ikcp.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/string_utils.h"
#include <cstring>
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.kcp");

namespace qtng {

const char PACKET_TYPE_UNCOMPRESSED_DATA = 0x01;
const char PACKET_TYPE_CREATE_MULTIPATH = 0x02;
const char PACKET_TYPE_CLOSE = 0X03;
const char PACKET_TYPE_KEEPALIVE = 0x04;

//#define DEBUG_PROTOCOL 1

class SlaveKcpSocketPrivate;
class KcpSocketPrivate
{
public:
    KcpSocketPrivate(KcpSocket *q);
    virtual ~KcpSocketPrivate();
public:
    virtual Socket::SocketError getError() const = 0;
    virtual string getErrorString() const = 0;
    virtual bool isValid() const = 0;
    virtual HostAddress localAddress() const = 0;
    virtual uint16_t localPort() const = 0;
    HostAddress peerAddress() const;
    string peerName() const;
    uint16_t peerPort() const;
    Socket::SocketType type() const;
    virtual HostAddress::NetworkLayerProtocol protocol() const = 0;
public:
    virtual KcpSocket *accept() = 0;
    virtual KcpSocket *accept(const HostAddress &addr, uint16_t port) = 0;
    virtual KcpSocket *accept(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) = 0;
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) = 0;
    virtual bool bind(uint16_t port, Socket::BindMode mode) = 0;
    virtual bool connect(const HostAddress &addr, uint16_t port) = 0;
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) = 0;
    virtual bool close(bool force) = 0;
    virtual bool listen(int backlog) = 0;
    virtual bool setOption(Socket::SocketOption option, int value) = 0;
    virtual int option(Socket::SocketOption option) const = 0;
    virtual bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface) = 0;
    virtual bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface) = 0;
    virtual NetworkInterface multicastInterface() const = 0;
    virtual bool setMulticastInterface(const NetworkInterface &iface) = 0;
public:
    void setMode(KcpSocket::Mode mode);
    int32_t send(const char *data, int32_t size, bool all);
    int32_t recv(char *data, int32_t size, bool all);
    int32_t peek(char *data, int32_t size);
    virtual int32_t peekRaw(char *data, int32_t size) = 0;
    bool handleDatagram(const char *buf, uint32_t len);
    void updateKcp();
    void updateStatus();
    void doUpdate();
    virtual int32_t rawSend(const char *data, int32_t size) = 0;
    virtual int32_t udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port) = 0;

    string makeDataPacket(const char *data, int32_t size);
    string makeShutdownPacket(uint32_t connectionId);
    string makeKeepalivePacket();
    string makeMultiPathPacket(uint32_t connectionId);
public:
    KcpSocket * const q_ptr;
    NG_DECLARE_PUBLIC(KcpSocket)
public:
    CoroutineGroup *operations;
    string errorString;
    Socket::SocketState state;
    Socket::SocketError error;

    Event sendingQueueNotFull;
    Event sendingQueueEmpty;
    Event receivingQueueNotEmpty;
    RLock kcpLock;
    Gate forceToUpdate;
    string receivingBuffer;

    const uint64_t zeroTimestamp;
    uint64_t lastActiveTimestamp;
    uint64_t lastKeepaliveTimestamp;
    uint64_t tearDownTime;
    ikcpcb *kcp;
    uint32_t waterLine;
    uint32_t connectionId;

    HostAddress remoteAddress;
    uint16_t remotePort;

    KcpSocket::Mode mode;
};

static inline string concat(const HostAddress &addr, uint16_t port)
{
    return addr.toString() + ":" + utils::number(port);
}

class MasterKcpSocketPrivate : public KcpSocketPrivate
{
public:
    MasterKcpSocketPrivate(HostAddress::NetworkLayerProtocol protocol, KcpSocket *q);
    MasterKcpSocketPrivate(intptr_t socketDescriptor, KcpSocket *q);
    MasterKcpSocketPrivate(shared_ptr<Socket> rawSocket, KcpSocket *q);
    virtual ~MasterKcpSocketPrivate() override;
public:
    virtual Socket::SocketError getError() const override;
    virtual string getErrorString() const override;
    virtual bool isValid() const override;
    virtual HostAddress localAddress() const override;
    virtual uint16_t localPort() const override;
    virtual HostAddress::NetworkLayerProtocol protocol() const override;
public:
    virtual KcpSocket *accept() override;
    virtual KcpSocket *accept(const HostAddress &addr, uint16_t port) override;
    virtual KcpSocket *accept(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override;
    virtual bool bind(uint16_t port, Socket::BindMode mode) override;
    virtual bool connect(const HostAddress &addr, uint16_t port) override;
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, int value) override;
    virtual int option(Socket::SocketOption option) const override;
    virtual bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface) override;
    virtual bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface) override;
    virtual NetworkInterface multicastInterface() const override;
    virtual bool setMulticastInterface(const NetworkInterface &iface) override;
public:
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t rawSend(const char *data, int32_t size) override;
    virtual int32_t udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port) override;
public:
    void removeSlave(const string &originalHostAndPort) { receiversByHostAndPort.erase(originalHostAndPort); }
    void removeSlave(uint32_t connectionId) { receiversByConnectionId.erase(connectionId); }
    uint32_t nextConnectionId();
    void doReceive();
    void doAccept();
    bool startReceivingCoroutine();
    HostAddress resolve(const string &hostName, shared_ptr<SocketDnsCache> dnsCache);
public:
    map<string, SlaveKcpSocketPrivate *> receiversByHostAndPort;
    map<uint32_t, SlaveKcpSocketPrivate *> receiversByConnectionId;
    shared_ptr<Socket> rawSocket;
    Queue<KcpSocket *> pendingSlaves;
    int nextPathSocket;  // 0 for rawSocket
};

class SlaveKcpSocketPrivate : public KcpSocketPrivate
{
public:
    SlaveKcpSocketPrivate(MasterKcpSocketPrivate *parent, const HostAddress &addr, uint16_t port, KcpSocket *q);
    virtual ~SlaveKcpSocketPrivate() override;
public:
    static KcpSocket *create(KcpSocketPrivate *d, const HostAddress &addr, uint16_t port, KcpSocket::Mode mode);
    static SlaveKcpSocketPrivate *getPrivateHelper(KcpSocket *s);
public:
    virtual Socket::SocketError getError() const override;
    virtual string getErrorString() const override;
    virtual bool isValid() const override;
    virtual HostAddress localAddress() const override;
    virtual uint16_t localPort() const override;
    virtual HostAddress::NetworkLayerProtocol protocol() const override;
public:
    virtual KcpSocket *accept() override;
    virtual KcpSocket *accept(const HostAddress &addr, uint16_t port) override;
    virtual KcpSocket *accept(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override;
    virtual bool bind(uint16_t port, Socket::BindMode mode) override;
    virtual bool connect(const HostAddress &addr, uint16_t port) override;
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, int value) override;
    virtual int option(Socket::SocketOption option) const override;
    virtual bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface) override;
    virtual bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface) override;
    virtual NetworkInterface multicastInterface() const override;
    virtual bool setMulticastInterface(const NetworkInterface &iface) override;
public:
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t rawSend(const char *data, int32_t size) override;
    virtual int32_t udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port) override;
public:
    string originalHostAndPort;
    MasterKcpSocketPrivate *parent;
};

KcpSocket *SlaveKcpSocketPrivate::create(KcpSocketPrivate *d, const HostAddress &addr, uint16_t port,
                                         KcpSocket::Mode mode)
{
    return new KcpSocket(d, addr, port, mode);
}

SlaveKcpSocketPrivate *SlaveKcpSocketPrivate::getPrivateHelper(KcpSocket *s)
{
    return static_cast<SlaveKcpSocketPrivate *>(s->d_ptr);
}

int kcp_callback(const char *buf, int len, ikcpcb *, void *user)
{
    KcpSocketPrivate *p = static_cast<KcpSocketPrivate *>(user);
    if (!p || !buf || len > 65535) {
        ngWarning() << "kcp_callback got invalid data.";
        return -1;
    }
    const string &packet = p->makeDataPacket(buf, len);
    int32_t sentBytes = p->rawSend(packet.data(), packet.size());
    if (sentBytes != packet.size()) {  // but why this happens?
        if (p->error == Socket::NoError) {
            p->error = Socket::SocketAccessError;
            p->errorString = "can not send udp packet";
        }
#ifdef DEBUG_PROTOCOL
        ngWarning() << "can not send packet.";
#endif
        p->close(true);
        return -1;
    }
    return sentBytes;
}

KcpSocketPrivate::KcpSocketPrivate(KcpSocket *q)
    : q_ptr(q)
    , operations(new CoroutineGroup)
    , state(Socket::UnconnectedState)
    , error(Socket::NoError)
    , zeroTimestamp(static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch()))
    , lastActiveTimestamp(zeroTimestamp)
    , lastKeepaliveTimestamp(zeroTimestamp)
    , tearDownTime(1000 * 30)
    , waterLine(1024)
    , connectionId(0)
    , remotePort(0)
    , mode(KcpSocket::Internet)
{
    kcp = ikcp_create(0, this);
    ikcp_setoutput(kcp, kcp_callback);
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
    receivingQueueNotEmpty.clear();
    q_ptr->busy.clear();
    q_ptr->notBusy.set();
    setMode(mode);
}

KcpSocketPrivate::~KcpSocketPrivate()
{
    delete operations;
    ikcp_release(kcp);
}

HostAddress KcpSocketPrivate::peerAddress() const
{
    return remoteAddress;
}

string KcpSocketPrivate::peerName() const
{
    return remoteAddress.toString();
}

uint16_t KcpSocketPrivate::peerPort() const
{
    return remotePort;
}

Socket::SocketType KcpSocketPrivate::type() const
{
    return Socket::KcpSocket;
}

// bool KcpSocketPrivate::close()
//{
// }

void KcpSocketPrivate::setMode(KcpSocket::Mode mode)
{
    this->mode = mode;
    switch (mode) {
    case KcpSocket::LargeDelayInternet:
        waterLine = 512;
        ikcp_nodelay(kcp, 0, 20, 32, 1);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 1024, 1024);
        break;
    case KcpSocket::Internet:
        waterLine = 256;
        ikcp_nodelay(kcp, 1, 10, 16, 1);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 1024, 1024);
        kcp->rx_minrto = 30;
        // kcp->interval = 5;
        break;
    case KcpSocket::FastInternet:
        waterLine = 192;
        ikcp_nodelay(kcp, 1, 10, 8, 1);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 512, 512);
        kcp->rx_minrto = 20;
        // kcp->interval = 2;
        break;
    case KcpSocket::Ethernet:
        waterLine = 64;
        ikcp_nodelay(kcp, 1, 10, 4, 1);
        ikcp_setmtu(kcp, 1024 * 32);
        ikcp_wndsize(kcp, 128, 128);
        kcp->rx_minrto = 10;
        // kcp->interval = 1;
        break;
    case KcpSocket::Loopback:
        waterLine = 64;
        ikcp_nodelay(kcp, 1, 10, 1, 1);
        ikcp_setmtu(kcp, 1024 * 64 - 256);
        ikcp_wndsize(kcp, 128, 128);
        kcp->rx_minrto = 5;
        // kcp->interval = 1;
        break;
    }
}

int32_t KcpSocketPrivate::send(const char *data, int32_t size, bool all)
{
    if (size <= 0 || !isValid()) {
        return -1;
    }

    sendingQueueEmpty.clear();

    int count = 0;
    while (count < size) {
        if (state != Socket::ConnectedState) {
            error = Socket::SocketAccessError;
            errorString = "KcpSocket is not connected.";
            return -1;
        }
        bool ok = sendingQueueNotFull.tryWait();
        if (!ok) {
            return -1;
        }
        int32_t nextBlockSize = min<int32_t>(static_cast<int32_t>(kcp->mss), size - count);
        int result;
        {
            ScopedLock<RLock> l(kcpLock);
            result = ikcp_send(kcp, data + count, nextBlockSize);
        }
        updateStatus();
        if (result < 0) {
            ngWarning() << "why this happened?";
            if (count > 0) {
                updateKcp();
                return count;
            } else {
                return -1;
            }
        } else {  // result == 0
            count += nextBlockSize;
            if (!all) {
                updateKcp();
                return count;
            }
        }
    }
    assert(all);
    updateKcp();
    return isValid() ? count : -1;
}

int32_t KcpSocketPrivate::recv(char *data, int32_t size, bool all)
{
    while (true) {
        if (state != Socket::ConnectedState) {
            error = Socket::SocketAccessError;
            errorString = "KcpSocket is not connected.";
            return -1;
        }
        if (!receivingBuffer.empty()) {
            if (!all || receivingBuffer.size() >= size) {
                int32_t len = static_cast<int32_t>(min(static_cast<size_t>(size), receivingBuffer.size()));
                memcpy(data, receivingBuffer.data(), static_cast<size_t>(len));
                receivingBuffer.erase(0, static_cast<size_t>(len));
                return len;
            }
        }
        int peeksize = ikcp_peeksize(kcp);
        if (peeksize > 0) {
            string buf(peeksize, '\0');
            int readBytes;
            {
                ScopedLock<RLock> l(kcpLock);
                readBytes = ikcp_recv(kcp, &buf[0], buf.size());
            }
            assert(readBytes == peeksize);
            receivingBuffer.append(buf);
            continue;
        }
        receivingQueueNotEmpty.clear();
        bool ok = receivingQueueNotEmpty.tryWait();
        if (!ok) {
            ngDebug() << "not receivingQueueNotEmpty->tryWait()";
            return -1;
        }
    }
}

int32_t KcpSocketPrivate::peek(char *data, int32_t size)
{
    if (state != Socket::ConnectedState) {
        return -1;
    }
    if (!receivingBuffer.empty()) {
        int32_t len = static_cast<int32_t>(min(static_cast<size_t>(size), receivingBuffer.size()));
        memcpy(data, receivingBuffer.data(), static_cast<size_t>(len));
        return len;
    }
    return 0;
}

bool KcpSocketPrivate::handleDatagram(const char *buf, uint32_t len)
{
    if (len < 5) {
        return true;
    }
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
        } else {
            lastActiveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
            receivingQueueNotEmpty.set();
            updateKcp();
        }
        break;
    }
    case PACKET_TYPE_CREATE_MULTIPATH:
        break;
    case PACKET_TYPE_CLOSE:
        close(true);
        return false;
    case PACKET_TYPE_KEEPALIVE:
        lastActiveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
        break;
    default:
        break;
    }
    return true;
}

void KcpSocketPrivate::doUpdate()
{
    // in close(), state is set to Socket::UnconnectedState but error = NoError.
    while (state == Socket::ConnectedState || (state == Socket::UnconnectedState && error == Socket::NoError)) {
        uint64_t now = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
        // now and lastActiveTimestamp both are unsigned int, we should check which is larger before apply minus
        // operator to them.
        if (now > lastActiveTimestamp && (now - lastActiveTimestamp > tearDownTime)
            && state == Socket::ConnectedState) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "kcp socket tearDown!";
#endif
            error = Socket::SocketTimeoutError;
            errorString = "KcpSocket is timeout.";
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
            const string &packet = makeKeepalivePacket();
            if (rawSend(packet.data(), packet.size()) != packet.size()) {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "can not send keep alive packet.";
#endif
                close(true);
                return;
            } else {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "keep alive packet sent.";
#endif
            }
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

void KcpSocketPrivate::updateKcp()
{
    shared_ptr<Coroutine> t = operations->spawnWithName(
            "update_kcp", [this] { doUpdate(); }, false);
    kcp->updated = 0;
    forceToUpdate.open();
}

void KcpSocketPrivate::updateStatus()
{
        int sendingQueueSize = ikcp_waitsnd(kcp);
    if (sendingQueueSize <= 0) {
        sendingQueueNotFull.set();
        sendingQueueEmpty.set();
        q_ptr->busy.clear();
        q_ptr->notBusy.set();
    } else {
        sendingQueueEmpty.clear();
        if (static_cast<uint32_t>(sendingQueueSize) > (waterLine * 1.2)) {
            sendingQueueNotFull.clear();
            q_ptr->busy.set();
            q_ptr->notBusy.clear();
        } else if (static_cast<uint32_t>(sendingQueueSize) > waterLine) {
            q_ptr->busy.set();
            q_ptr->notBusy.clear();
        } else {
            sendingQueueNotFull.set();
            q_ptr->busy.clear();
            q_ptr->notBusy.set();
        }
    }
}

string KcpSocketPrivate::makeDataPacket(const char *data, int32_t size)
{
    string packet(size + 1, '\0');
    packet[0] = PACKET_TYPE_UNCOMPRESSED_DATA;
    memcpy(&packet[1], data, static_cast<size_t>(size));
    ngToBigEndian<uint32_t>(this->connectionId, &packet[1]);
    return packet;
}

string KcpSocketPrivate::makeShutdownPacket(uint32_t connectionId)
{
    string packet = randomBytes(5 + static_cast<int>(random() % (64 - 5)));
    packet[0] = PACKET_TYPE_CLOSE;
    ngToBigEndian<uint32_t>(connectionId, &packet[1]);
    return packet;
}

string KcpSocketPrivate::makeKeepalivePacket()
{
    string packet = randomBytes(5 + static_cast<int>(random() % (64 - 5)));
    packet[0] = PACKET_TYPE_KEEPALIVE;
    ngToBigEndian<uint32_t>(this->connectionId, &packet[1]);
    return packet;
}

string KcpSocketPrivate::makeMultiPathPacket(uint32_t connectionId)
{
    string packet = randomBytes(5 + static_cast<int>(random() % (64 - 5)));
    packet[0] = PACKET_TYPE_CREATE_MULTIPATH;
    ngToBigEndian<uint32_t>(connectionId, &packet[1]);
    return packet;
}

MasterKcpSocketPrivate::MasterKcpSocketPrivate(HostAddress::NetworkLayerProtocol protocol, KcpSocket *q)
    : KcpSocketPrivate(q)
    , rawSocket(new Socket(protocol, Socket::UdpSocket))
    , nextPathSocket(0)
{
}

MasterKcpSocketPrivate::MasterKcpSocketPrivate(intptr_t socketDescriptor, KcpSocket *q)
    : KcpSocketPrivate(q)
    , rawSocket(new Socket(socketDescriptor))
    , nextPathSocket(0)
{
}

MasterKcpSocketPrivate::MasterKcpSocketPrivate(shared_ptr<Socket> rawSocket, KcpSocket *q)
    : KcpSocketPrivate(q)
    , rawSocket(rawSocket)
    , nextPathSocket(0)
{
}

MasterKcpSocketPrivate::~MasterKcpSocketPrivate()
{
    MasterKcpSocketPrivate::close(true);
}

Socket::SocketError MasterKcpSocketPrivate::getError() const
{
    if (error != Socket::NoError) {
        return error;
    } else {
        return rawSocket->error();
    }
}

string MasterKcpSocketPrivate::getErrorString() const
{
    if (!errorString.empty()) {
        return errorString;
    } else {
        return rawSocket->errorString();
    }
}

bool MasterKcpSocketPrivate::isValid() const
{
    return state == Socket::ConnectedState || state == Socket::BoundState || state == Socket::ListeningState;
}

HostAddress MasterKcpSocketPrivate::localAddress() const
{
    return rawSocket->localAddress();
}

uint16_t MasterKcpSocketPrivate::localPort() const
{
    return rawSocket->localPort();
}

HostAddress::NetworkLayerProtocol MasterKcpSocketPrivate::protocol() const
{
    return rawSocket->protocol();
}

bool MasterKcpSocketPrivate::close(bool force)
{
    // if `force` is true, must not block. see doUpdate()
    if (state == Socket::UnconnectedState) {
        return true;
    } else if (state == Socket::ConnectedState) {
        state = Socket::UnconnectedState;
        if (!force && error == Socket::NoError) {
            if (!sendingQueueEmpty.isSet()) {
                updateKcp();
                if (!sendingQueueEmpty.tryWait()) {
                    return false;
                }
            }
            const string &packet = makeShutdownPacket(this->connectionId);
            rawSend(packet.data(), packet.size());
        }
    } else if (state == Socket::ListeningState) {
        state = Socket::UnconnectedState;
        map<string, SlaveKcpSocketPrivate *> receiversByHostAndPort(this->receiversByHostAndPort);
        this->receiversByHostAndPort.clear();
        for (const auto &item : receiversByHostAndPort) {
            SlaveKcpSocketPrivate *receiver = item.second;
            if (receiver) {
                receiver->close(force);
            }
        }
        receiversByConnectionId.clear();
    } else {  // BoundState
        state = Socket::UnconnectedState;
        rawSocket->abort();
        return true;
    }

    while (!pendingSlaves.isEmpty()) {
        delete pendingSlaves.get();
    }
    pendingSlaves.put(nullptr);

    // connected and listen state would do more cleaning work.
    operations->killall();
    // always kill operations before release resources.
    rawSocket->abort();
    //    if (force) {
    //        rawSocket->abort();
    //    } else {
    //        rawSocket->close();
    //    }
    // awake all pending recv()/send()
    receivingQueueNotEmpty.set();
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
#ifdef DEBUG_PROTOCOL
    ngDebug() << "MasterKcpSocketPrivate::close() done";
#endif
    return true;
}

bool MasterKcpSocketPrivate::listen(int backlog)
{
    if (state != Socket::BoundState || backlog <= 0) {
        return false;
    }
    state = Socket::ListeningState;
    pendingSlaves.setCapacity(static_cast<uint32_t>(backlog));
    return true;
}

uint32_t MasterKcpSocketPrivate::nextConnectionId()
{
    uint32_t id;
    do {
        const string bytes = randomBytes(4);
        memcpy(&id, bytes.data(), sizeof(id));
    } while (receiversByConnectionId.find(id) != receiversByConnectionId.end());
    return id;
}

void MasterKcpSocketPrivate::doReceive()
{
        HostAddress addr;
    uint16_t port;
    string buf(1024 * 64, '\0');
    while (true) {
        int32_t len = rawSocket->recvfrom(&buf[0], buf.size(), &addr, &port);
        if ((len < 0 || addr.isNull() || port == 0)) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "KcpSocket can not receive udp packet." << rawSocket->errorString();
#endif
            MasterKcpSocketPrivate::close(true);
            return;
        }
        if (q_ptr->filter(&buf[0], &len, &addr, &port)) {
            continue;
        }
        if (len < 5) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "got invalid kcp packet smaller than 5 bytes." << string(buf.data(), len);
#endif
            continue;
        }

        const uint32_t packetConnectionId = ngFromBigEndian<uint32_t>(buf.data() + 1);
        if (packetConnectionId == 0) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "the kcp server side returns an invalid packet with zero connection id.";
#endif
            continue;
        } else {
            if (this->connectionId == 0) {
                this->connectionId = packetConnectionId;
            } else {
                if (packetConnectionId != this->connectionId) {
#ifdef DEBUG_PROTOCOL
                    ngDebug() << "the kcp server side returns an invalid packet with mismatched connection id.";
#endif
                    continue;
                } else {
                    // do nothing.
                }
            }
        }
        ngToBigEndian<uint32_t>(0, reinterpret_cast<uint8_t *>(&buf[1]));
        if (!handleDatagram(buf.data(), static_cast<uint32_t>(len))) {
            return;
        }
    }
}

void MasterKcpSocketPrivate::doAccept()
{
        HostAddress addr;
    uint16_t port;
    string buf(1024 * 64, '\0');
    while (true) {
        int32_t len = rawSocket->recvfrom(&buf[0], buf.size(), &addr, &port);
        if ((len < 0 || addr.isNull() || port == 0)) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "KcpSocket can not receive udp packet." << rawSocket->errorString();
#endif
            MasterKcpSocketPrivate::close(true);
            return;
        }
        if (q_ptr->filter(&buf[0], &len, &addr, &port)) {
            continue;
        }
        if (len < 5) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "got invalid kcp packet smaller than 5 bytes.";
#endif
            continue;
        }

        uint32_t connectionId = ngFromBigEndian<uint32_t>(buf.data() + 1);
        ngToBigEndian<uint32_t>(0, reinterpret_cast<uint8_t *>(&buf[1]));
        const string &key = concat(addr, port);
        SlaveKcpSocketPrivate *receiver = nullptr;
        auto hostIt = receiversByHostAndPort.find(key);
        if (hostIt != receiversByHostAndPort.end()) {
            receiver = hostIt->second;
        }
        if (receiver) {
            receiver->remoteAddress = addr;
            receiver->remotePort = port;
            if (connectionId != 0) {
                if (receiver->connectionId == 0) {
                    // only if the slave was created by accept(host, port), we had zero id.
                    // if this connectionId is unique in client. we add it to the receiversByConnectionId map.
                    // if it is not, say sorry, and disable the multipath feature.
                    if (receiversByConnectionId.find(connectionId) == receiversByConnectionId.end()) {
                        // only happened in the newly accept(host, port) connections.
                        // or remote create new conn with the same port as old, and the old packet is received.
                        receiver->connectionId = connectionId;
                        receiversByConnectionId[connectionId] = receiver;
                    }
                } else if (connectionId != receiver->connectionId) {
#ifdef DEBUG_PROTOCOL
                    ngDebug() << "the client sent a invalid connection id";
#endif
                    continue;
                }
            }
            if (!receiver->handleDatagram(buf.data(), static_cast<uint32_t>(len))) {
                receiversByHostAndPort.erase(receiver->originalHostAndPort);
                receiversByConnectionId.erase(receiver->connectionId);
            }
        } else {
            if (connectionId != 0) {  // a multipath packet.
                const auto it = receiversByConnectionId.find(connectionId);
                receiver = (it != receiversByConnectionId.end()) ? it->second : nullptr;
                if (!receiver) {
                    // it must be bad packet.
                    const string &closePacket = makeShutdownPacket(connectionId);
                    if (rawSocket->sendto(closePacket, addr, port) != closePacket.size()) {
                        if (error == Socket::NoError) {
                            error = Socket::SocketResourceError;
                            errorString = "KcpSocket can not send udp packet.";
                        }
#ifdef DEBUG_PROTOCOL
                        ngDebug() << errorString;
#endif
                        MasterKcpSocketPrivate::close(true);
                    }
                } else {
                    assert(connectionId == receiver->connectionId);
                    receiver->remoteAddress = addr;
                    receiver->remotePort = port;
                    if (!receiver->handleDatagram(buf.data(), static_cast<uint32_t>(len))) {
#ifdef DEBUG_PROTOCOL
                        ngDebug() << "can not handle multipath packet.";
#endif
                        receiversByHostAndPort.erase(receiver->originalHostAndPort);
                        receiversByConnectionId.erase(receiver->connectionId);
                    }
                }
            } else if (pendingSlaves.size() < pendingSlaves.capacity()) {  // not full. process new connection.
                unique_ptr<KcpSocket> slave(SlaveKcpSocketPrivate::create(this, addr, port, this->mode));
                SlaveKcpSocketPrivate *d = SlaveKcpSocketPrivate::getPrivateHelper(slave.get());
                d->originalHostAndPort = key;
                d->connectionId = nextConnectionId();
                if (d->handleDatagram(buf.data(), static_cast<uint32_t>(len))) {
                    receiversByHostAndPort[key] = d;
                    receiversByConnectionId[d->connectionId] = d;
                    pendingSlaves.put(slave.release());
                    const string &multiPathPacket = makeMultiPathPacket(d->connectionId);
                    if (rawSocket->sendto(multiPathPacket, addr, port) != multiPathPacket.size()) {
                        if (error == Socket::NoError) {
                            error = Socket::SocketResourceError;
                            errorString = "KcpSocket can not send udp packet.";
                        }
#ifdef DEBUG_PROTOCOL
                        ngDebug() << errorString;
#endif
                        MasterKcpSocketPrivate::close(true);
                    }
                }
            }
        }
    }
}

bool MasterKcpSocketPrivate::startReceivingCoroutine()
{
    if (operations->get("receiving")) {
        return true;
    }
    switch (state) {
    case Socket::UnconnectedState:
    case Socket::BoundState:
    case Socket::ConnectingState:
    case Socket::HostLookupState:
    case Socket::ClosingState:
        return false;
    case Socket::ConnectedState:
        operations->spawnWithName("receiving", [this] { doReceive(); });
        break;
    case Socket::ListeningState:
        operations->spawnWithName("receiving", [this] { doAccept(); });
        break;
    }
    return true;
}

KcpSocket *MasterKcpSocketPrivate::accept()
{
    if (state != Socket::ListeningState) {
        return nullptr;
    }
    startReceivingCoroutine();
    return pendingSlaves.get();
}

KcpSocket *MasterKcpSocketPrivate::accept(const HostAddress &addr, uint16_t port)
{
    if (state != Socket::ListeningState || addr.isNull() || port == 0) {
        return nullptr;
    }
    startReceivingCoroutine();
    const string &key = concat(addr, port);
    SlaveKcpSocketPrivate *receiver;
    receiver = receiversByHostAndPort.at(key);
    if (receiver && receiver->isValid()) {
        return nullptr;
    } else {
        unique_ptr<KcpSocket> slave(SlaveKcpSocketPrivate::create(this, addr, port, this->mode));
        SlaveKcpSocketPrivate *d = SlaveKcpSocketPrivate::getPrivateHelper(slave.get());
        d->originalHostAndPort = key;
        d->updateKcp();
        receiversByHostAndPort[key] = d;
        // the connectionId is generated in server side. accept() is acually a connect().
        // receiversByConnectionId[d->connectionId] = d;
        return slave.release();
    }
}

KcpSocket *MasterKcpSocketPrivate::accept(const string &hostName, uint16_t port,
                                          shared_ptr<SocketDnsCache> dnsCache)
{
    if (state != Socket::ListeningState || hostName.empty() || port == 0) {
        return nullptr;
    }
    const HostAddress &addr = resolve(hostName, dnsCache);
    if (addr.isNull()) {
        return nullptr;
    } else {
        return accept(addr, port);
    }
}

bool MasterKcpSocketPrivate::connect(const HostAddress &addr, uint16_t port)
{
    if ((state != Socket::UnconnectedState && state != Socket::BoundState) || addr.isNull()) {
        return false;
    }
    remoteAddress = addr;
    remotePort = port;
    state = Socket::ConnectedState;
    return true;
}

bool MasterKcpSocketPrivate::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    if (state != Socket::UnconnectedState && state != Socket::BoundState) {
        return false;
    }
    const HostAddress &addr = resolve(hostName, dnsCache);
    if (addr.isNull()) {
        return false;
    } else {
        return connect(addr, port);
    }
}

HostAddress MasterKcpSocketPrivate::resolve(const string &hostName, shared_ptr<SocketDnsCache> dnsCache)
{
    vector<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.push_back(t);
    } else {
        if (!dnsCache) {
            addresses = Socket::resolve(hostName);
        } else {
            addresses = dnsCache->resolve(hostName);
        }
    }
    for (int i = 0; i < addresses.size(); ++i) {
        const HostAddress &addr = addresses[i];
        if (rawSocket->protocol() == HostAddress::IPv4Protocol && addr.protocol() == HostAddress::IPv6Protocol) {
            continue;
        }
        if (rawSocket->protocol() == HostAddress::IPv6Protocol && addr.protocol() == HostAddress::IPv4Protocol) {
            continue;
        }
        return addr;
    }
    return HostAddress();
}

int32_t MasterKcpSocketPrivate::peekRaw(char *data, int32_t size)
{
    return rawSocket->peek(data, size);
}

int32_t MasterKcpSocketPrivate::rawSend(const char *data, int32_t size)
{
    lastKeepaliveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
    startReceivingCoroutine();
    return rawSocket->sendto(data, size, remoteAddress, remotePort);
}

int32_t MasterKcpSocketPrivate::udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port)
{
    return rawSocket->sendto(data, size, addr, port);
}

bool MasterKcpSocketPrivate::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    if (state != Socket::UnconnectedState) {
        return false;
    }
    if (mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    if (rawSocket->bind(address, port, mode)) {
        state = Socket::BoundState;
        return true;
    } else {
        return false;
    }
}

bool MasterKcpSocketPrivate::bind(uint16_t port, Socket::BindMode mode)
{
    if (state != Socket::UnconnectedState) {
        return false;
    }
    if (mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    if (rawSocket->bind(port, mode)) {
        state = Socket::BoundState;
        return true;
    } else {
        return false;
    }
}

bool MasterKcpSocketPrivate::setOption(Socket::SocketOption option, int value)
{
    return rawSocket->setOption(option, value);
}

int MasterKcpSocketPrivate::option(Socket::SocketOption option) const
{
    return rawSocket->option(option);
}

bool MasterKcpSocketPrivate::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return rawSocket->joinMulticastGroup(groupAddress, iface);
}

bool MasterKcpSocketPrivate::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return rawSocket->leaveMulticastGroup(groupAddress, iface);
}

NetworkInterface MasterKcpSocketPrivate::multicastInterface() const
{
    return rawSocket->multicastInterface();
}

bool MasterKcpSocketPrivate::setMulticastInterface(const NetworkInterface &iface)
{
    return rawSocket->setMulticastInterface(iface);
}

SlaveKcpSocketPrivate::SlaveKcpSocketPrivate(MasterKcpSocketPrivate *parent, const HostAddress &addr, uint16_t port,
                                             KcpSocket *q)
    : KcpSocketPrivate(q)
    , parent(parent)
{
    remoteAddress = addr;
    remotePort = port;
    state = Socket::ConnectedState;
}

SlaveKcpSocketPrivate::~SlaveKcpSocketPrivate()
{
    SlaveKcpSocketPrivate::close(true);
}

Socket::SocketError SlaveKcpSocketPrivate::getError() const
{
    if (error != Socket::NoError) {
        return error;
    } else {
        if (parent) {
            return parent->rawSocket->error();
        } else {
            return Socket::SocketAccessError;
        }
    }
}

string SlaveKcpSocketPrivate::getErrorString() const
{
    if (!errorString.empty()) {
        return errorString;
    } else {
        if (parent) {
            return parent->rawSocket->errorString();
        } else {
            return "Invalid socket descriptor";
        }
    }
}

bool SlaveKcpSocketPrivate::isValid() const
{
    return state == Socket::ConnectedState && parent;
}

HostAddress SlaveKcpSocketPrivate::localAddress() const
{
    if (!parent) {
        return HostAddress();
    }
    return parent->rawSocket->localAddress();
}

uint16_t SlaveKcpSocketPrivate::localPort() const
{
    if (!parent) {
        return 0;
    }
    return parent->rawSocket->localPort();
}

HostAddress::NetworkLayerProtocol SlaveKcpSocketPrivate::protocol() const
{
    if (!parent) {
        return HostAddress::UnknownNetworkLayerProtocol;
    }
    return parent->rawSocket->protocol();
}

bool SlaveKcpSocketPrivate::close(bool force)
{
        // if `force` is true, must not block. it is called by doUpdate()
    if (state == Socket::UnconnectedState) {
        return true;
    } else if (state == Socket::ConnectedState) {
        state = Socket::UnconnectedState;
        if (!force && error == Socket::NoError) {
            if (!sendingQueueEmpty.isSet()) {
                updateKcp();
                if (!sendingQueueEmpty.tryWait()) {
                    return false;
                }
            }
            const string &packet = makeShutdownPacket(this->connectionId);
            rawSend(packet.data(), packet.size());
        }
    } else {  // there can be no other states.
        state = Socket::UnconnectedState;
    }
    operations->killall();
    if (parent) {
        parent->removeSlave(originalHostAndPort);
        parent->removeSlave(connectionId);
        parent = nullptr;
    }
    // await all pending recv()/send()
    receivingQueueNotEmpty.set();
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
    q_ptr->notBusy.set();
    q_ptr->busy.set();
#ifdef DEBUG_PROTOCOL
    ngDebug() << "SlaveKcpSocketPrivate::close() done.";
#endif
    return true;
}

bool SlaveKcpSocketPrivate::listen(int)
{
    return false;
}

KcpSocket *SlaveKcpSocketPrivate::accept()
{
    return nullptr;
}

KcpSocket *SlaveKcpSocketPrivate::accept(const HostAddress &, uint16_t)
{
    return nullptr;
}

KcpSocket *SlaveKcpSocketPrivate::accept(const string &, uint16_t, shared_ptr<SocketDnsCache>)
{
    return nullptr;
}

bool SlaveKcpSocketPrivate::connect(const HostAddress &, uint16_t)
{
    return false;
}

bool SlaveKcpSocketPrivate::connect(const string &, uint16_t, shared_ptr<SocketDnsCache>)
{
    return false;
}

int32_t SlaveKcpSocketPrivate::peekRaw(char *data, int32_t size)
{
    if (!parent) {
        return -1;
    }
    return parent->rawSocket->peek(data, size);
}

int32_t SlaveKcpSocketPrivate::rawSend(const char *data, int32_t size)
{
    if (!parent) {
        return -1;
    } else {
        lastKeepaliveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
        return parent->rawSocket->sendto(data, size, remoteAddress, remotePort);
    }
}

int32_t SlaveKcpSocketPrivate::udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port)
{
    if (!parent) {
        return -1;
    } else {
        return parent->rawSocket->sendto(data, size, addr, port);
    }
}

bool SlaveKcpSocketPrivate::bind(const HostAddress &, uint16_t, Socket::BindMode)
{
    return false;
}

bool SlaveKcpSocketPrivate::bind(uint16_t, Socket::BindMode)
{
    return false;
}

bool SlaveKcpSocketPrivate::setOption(Socket::SocketOption, int)
{
    return false;
}

int SlaveKcpSocketPrivate::option(Socket::SocketOption option) const
{
    if (!parent) {
        return -1;
    } else {
        return parent->rawSocket->option(option);
    }
}

bool SlaveKcpSocketPrivate::joinMulticastGroup(const HostAddress &, const NetworkInterface &)
{
    return false;
}

bool SlaveKcpSocketPrivate::leaveMulticastGroup(const HostAddress &, const NetworkInterface &)
{
    return false;
}

NetworkInterface SlaveKcpSocketPrivate::multicastInterface() const
{
    return NetworkInterface();
}

bool SlaveKcpSocketPrivate::setMulticastInterface(const NetworkInterface &)
{
    return false;
}

KcpSocket::KcpSocket(HostAddress::NetworkLayerProtocol protocol)
    : d_ptr(new MasterKcpSocketPrivate(protocol, this))
{
}

KcpSocket::KcpSocket(intptr_t socketDescriptor)
    : d_ptr(new MasterKcpSocketPrivate(socketDescriptor, this))
{
}

KcpSocket::KcpSocket(shared_ptr<Socket> rawSocket)
    : d_ptr(new MasterKcpSocketPrivate(rawSocket, this))
{
}

KcpSocket::KcpSocket(KcpSocketPrivate *parent, const HostAddress &addr, const uint16_t port, KcpSocket::Mode mode)
    : d_ptr(new SlaveKcpSocketPrivate(static_cast<MasterKcpSocketPrivate *>(parent), addr, port, this))
{
    setMode(mode);
}

KcpSocket::~KcpSocket()
{
    delete d_ptr;
}

void KcpSocket::setMode(Mode mode)
{
    NG_D(KcpSocket);
    d->setMode(mode);
}

KcpSocket::Mode KcpSocket::mode() const
{
    NG_D(const KcpSocket);
    return d->mode;
}

void KcpSocket::setUdpPacketSize(uint32_t udpPacketSize)
{
    NG_D(const KcpSocket);
    if (udpPacketSize < 65535) {
        ikcp_setmtu(d->kcp, static_cast<int>(udpPacketSize));
    }
}

uint32_t KcpSocket::udpPacketSize() const
{
    NG_D(const KcpSocket);
    return d->kcp->mtu;
}

void KcpSocket::setSendQueueSize(uint32_t sendQueueSize)
{
    NG_D(KcpSocket);
    d->waterLine = sendQueueSize;
}

uint32_t KcpSocket::sendQueueSize() const
{
    NG_D(const KcpSocket);
    return d->waterLine;
}

uint32_t KcpSocket::payloadSizeHint() const
{
    NG_D(const KcpSocket);
    return d->kcp->mss;
}

void KcpSocket::setTearDownTime(float secs)
{
    NG_D(KcpSocket);
    if (secs > 0) {
        d->tearDownTime = static_cast<uint64_t>(secs * 1000);
        if (d->tearDownTime < 1000) {
            d->tearDownTime = 1000;
        }
    }
}

float KcpSocket::tearDownTime() const
{
    NG_D(const KcpSocket);
    return d->tearDownTime / 1000.0f;
}

Socket::SocketError KcpSocket::error() const
{
    NG_D(const KcpSocket);
    return d->getError();
}

string KcpSocket::errorString() const
{
    NG_D(const KcpSocket);
    return d->getErrorString();
}

bool KcpSocket::isValid() const
{
    NG_D(const KcpSocket);
    return d->isValid();
}

HostAddress KcpSocket::localAddress() const
{
    NG_D(const KcpSocket);
    return d->localAddress();
}

uint16_t KcpSocket::localPort() const
{
    NG_D(const KcpSocket);
    return d->localPort();
}

HostAddress KcpSocket::peerAddress() const
{
    NG_D(const KcpSocket);
    return d->peerAddress();
}

string KcpSocket::peerName() const
{
    NG_D(const KcpSocket);
    return d->peerName();
}

uint16_t KcpSocket::peerPort() const
{
    NG_D(const KcpSocket);
    return d->peerPort();
}

Socket::SocketType KcpSocket::type() const
{
    NG_D(const KcpSocket);
    return d->type();
}

Socket::SocketState KcpSocket::state() const
{
    NG_D(const KcpSocket);
    return d->state;
}

HostAddress::NetworkLayerProtocol KcpSocket::protocol() const
{
    NG_D(const KcpSocket);
    return d->protocol();
}

string KcpSocket::localAddressURI() const
{
    NG_D(const KcpSocket);
    string address = "kcp://%1:%2";
    const HostAddress &localAddress = d->localAddress();
    if (localAddress.protocol() == HostAddress::IPv6Protocol) {
        address = utils::formatMessage("[%1]", {localAddress.toString()});
    } else {
        address = localAddress.toString();
    }
    address = utils::formatMessage("%1:%2", {address, utils::number(d->localPort())});
    return address;
}

string KcpSocket::peerAddressURI() const
{
    NG_D(const KcpSocket);
    string address = "kcp://%1:%2";
    if (d->remoteAddress.protocol() == HostAddress::IPv6Protocol) {
        address = utils::formatMessage("[%1]", {d->remoteAddress.toString()});
    } else {
        address = d->remoteAddress.toString();
    }
    address = utils::formatMessage("%1:%2", {address, utils::number(d->remotePort)});
    return address;
}

KcpSocket *KcpSocket::accept()
{
    NG_D(KcpSocket);
    return d->accept();
}

KcpSocket *KcpSocket::accept(const HostAddress &addr, uint16_t port)
{
    NG_D(KcpSocket);
    return d->accept(addr, port);
}

KcpSocket *KcpSocket::accept(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    NG_D(KcpSocket);
    return d->accept(hostName, port, dnsCache);
}

bool KcpSocket::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    NG_D(KcpSocket);
    return d->bind(address, port, mode);
}

bool KcpSocket::bind(uint16_t port, Socket::BindMode mode)
{
    NG_D(KcpSocket);
    return d->bind(port, mode);
}

bool KcpSocket::connect(const HostAddress &addr, uint16_t port)
{
    NG_D(KcpSocket);
    return d->connect(addr, port);
}

bool KcpSocket::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    NG_D(KcpSocket);
    return d->connect(hostName, port, dnsCache);
}

void KcpSocket::close()
{
    NG_D(KcpSocket);
    d->close(false);
}

void KcpSocket::abort()
{
    NG_D(KcpSocket);
    d->close(true);
}

bool KcpSocket::listen(int backlog)
{
    NG_D(KcpSocket);
    return d->listen(backlog);
}

bool KcpSocket::setOption(Socket::SocketOption option, int value)
{
    NG_D(KcpSocket);
    return d->setOption(option, value);
}

int KcpSocket::option(Socket::SocketOption option) const
{
    NG_D(const KcpSocket);
    return d->option(option);
}

bool KcpSocket::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    NG_D(KcpSocket);
    return d->joinMulticastGroup(groupAddress, iface);
}

bool KcpSocket::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    NG_D(KcpSocket);
    return d->leaveMulticastGroup(groupAddress, iface);
}

NetworkInterface KcpSocket::multicastInterface() const
{
    NG_D(const KcpSocket);
    return d->multicastInterface();
}

bool KcpSocket::setMulticastInterface(const NetworkInterface &iface)
{
    NG_D(KcpSocket);
    return d->setMulticastInterface(iface);
}

int32_t KcpSocket::peek(char *data, int32_t size)
{
    NG_D(KcpSocket);
    return d->peek(data, size);
}

int32_t KcpSocket::peekRaw(char *data, int32_t size)
{
    NG_D(KcpSocket);
    return d->peekRaw(data, size);
}

int32_t KcpSocket::recv(char *data, int32_t size)
{
    NG_D(KcpSocket);
    return d->recv(data, size, false);
}

int32_t KcpSocket::recvall(char *data, int32_t size)
{
    NG_D(KcpSocket);
    return d->recv(data, size, true);
}

int32_t KcpSocket::send(const char *data, int32_t size)
{
    NG_D(KcpSocket);
    int32_t bytesSent = d->send(data, size, false);
    if (bytesSent == 0 && !d->isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}

int32_t KcpSocket::sendall(const char *data, int32_t size)
{
    NG_D(KcpSocket);
    return d->send(data, size, true);
}

string KcpSocket::recv(int32_t size)
{
    NG_D(KcpSocket);
    string bs(size, '\0');

    int32_t bytes = d->recv(&bs[0], bs.size(), false);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return string();
}

string KcpSocket::recvall(int32_t size)
{
    NG_D(KcpSocket);
    string bs(size, '\0');

    int32_t bytes = d->recv(&bs[0], bs.size(), true);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return string();
}

int32_t KcpSocket::send(const string &data)
{
    NG_D(KcpSocket);
    int32_t bytesSent = d->send(data.data(), data.size(), false);
    if (bytesSent == 0 && !d->isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}

int32_t KcpSocket::sendall(const string &data)
{
    NG_D(KcpSocket);
    return d->send(data.data(), data.size(), true);
}

bool KcpSocket::filter(char *data, int32_t *len, HostAddress *addr, uint16_t *port)
{
    (void)(data);
    (void)(len);
    (void)(addr);
    (void)(port);
    return false;
}

int32_t KcpSocket::udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port)
{
    NG_D(KcpSocket);
    return d->udpSend(data, size, addr, port);
}

KcpSocket *KcpSocket::createConnection(const HostAddress &host, uint16_t port, Socket::SocketError *error,
                                       int allowProtocol)
{
    return qtng::createConnection<KcpSocket>(host, port, error, allowProtocol,
                                                              MakeSocketType<KcpSocket>);
}

KcpSocket *KcpSocket::createConnection(const string &hostName, uint16_t port, Socket::SocketError *error,
                                       shared_ptr<SocketDnsCache> dnsCache, int allowProtocol)
{
    return qtng::createConnection<KcpSocket>(hostName, port, error, dnsCache, allowProtocol,
                                                              MakeSocketType<KcpSocket>);
}

KcpSocket *KcpSocket::createServer(const HostAddress &host, uint16_t port, int backlog)
{
    return qtng::createServer<KcpSocket>(host, port, backlog, MakeSocketType<KcpSocket>);
}

namespace {

class KcpSocketLikeImpl : public SocketLike
{
public:
    KcpSocketLikeImpl(shared_ptr<KcpSocket> s);
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
    virtual void close() override;
    virtual void abort() override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, int value) override;
    virtual int option(Socket::SocketOption option) const override;

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
public:
    shared_ptr<KcpSocket> s;
};

KcpSocketLikeImpl::KcpSocketLikeImpl(shared_ptr<KcpSocket> s)
    : s(s)
{
}

Socket::SocketError KcpSocketLikeImpl::error() const
{
    return s->error();
}

string KcpSocketLikeImpl::errorString() const
{
    return s->errorString();
}

bool KcpSocketLikeImpl::isValid() const
{
    return s->isValid();
}

HostAddress KcpSocketLikeImpl::localAddress() const
{
    return s->localAddress();
}

uint16_t KcpSocketLikeImpl::localPort() const
{
    return s->localPort();
}

HostAddress KcpSocketLikeImpl::peerAddress() const
{
    return s->peerAddress();
}

string KcpSocketLikeImpl::peerName() const
{
    return s->peerName();
}

uint16_t KcpSocketLikeImpl::peerPort() const
{
    return s->peerPort();
}

intptr_t KcpSocketLikeImpl::fileno() const
{
    return -1;
}

Socket::SocketType KcpSocketLikeImpl::type() const
{
    return s->type();
}

Socket::SocketState KcpSocketLikeImpl::state() const
{
    return s->state();
}

HostAddress::NetworkLayerProtocol KcpSocketLikeImpl::protocol() const
{
    return s->protocol();
}

string KcpSocketLikeImpl::localAddressURI() const
{
    return s->localAddressURI();
}

string KcpSocketLikeImpl::peerAddressURI() const
{
    return s->peerAddressURI();
}

Socket *KcpSocketLikeImpl::acceptRaw()
{
    return nullptr;
}

shared_ptr<SocketLike> KcpSocketLikeImpl::accept()
{
    return asSocketLike(s->accept());
}

bool KcpSocketLikeImpl::bind(const HostAddress &address, uint16_t port = 0,
                             Socket::BindMode mode = Socket::DefaultForPlatform)
{
    return s->bind(address, port, mode);
}

bool KcpSocketLikeImpl::bind(uint16_t port, Socket::BindMode mode)
{
    return s->bind(port, mode);
}

bool KcpSocketLikeImpl::connect(const HostAddress &addr, uint16_t port)
{
    return s->connect(addr, port);
}

bool KcpSocketLikeImpl::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    return s->connect(hostName, port, dnsCache);
}

void KcpSocketLikeImpl::close()
{
    s->close();
}

void KcpSocketLikeImpl::abort()
{
    s->abort();
}

bool KcpSocketLikeImpl::listen(int backlog)
{
    return s->listen(backlog);
}

bool KcpSocketLikeImpl::setOption(Socket::SocketOption option, int value)
{
    return s->setOption(option, value);
}

int KcpSocketLikeImpl::option(Socket::SocketOption option) const
{
    return s->option(option);
}

int32_t KcpSocketLikeImpl::peek(char *data, int32_t size)
{
    return s->peek(data, size);
}

int32_t KcpSocketLikeImpl::peekRaw(char *data, int32_t size)
{
    return s->peekRaw(data, size);
}

int32_t KcpSocketLikeImpl::recv(char *data, int32_t size)
{
    return s->recv(data, size);
}

int32_t KcpSocketLikeImpl::recvall(char *data, int32_t size)
{
    return s->recvall(data, size);
}

int32_t KcpSocketLikeImpl::send(const char *data, int32_t size)
{
    return s->send(data, size);
}

int32_t KcpSocketLikeImpl::sendall(const char *data, int32_t size)
{
    return s->sendall(data, size);
}

string KcpSocketLikeImpl::recv(int32_t size)
{
    return s->recv(size);
}

string KcpSocketLikeImpl::recvall(int32_t size)
{
    return s->recvall(size);
}

int32_t KcpSocketLikeImpl::send(const string &data)
{
    return s->send(data);
}

int32_t KcpSocketLikeImpl::sendall(const string &data)
{
    return s->sendall(data);
}

}  // namespace

shared_ptr<SocketLike> asSocketLike(shared_ptr<KcpSocket> s)
{
    if (!s) {
        return shared_ptr<SocketLike>();
    }
    return make_shared<KcpSocketLikeImpl>(s);
}

shared_ptr<KcpSocket> convertSocketLikeToKcpSocket(shared_ptr<SocketLike> socket)
{
    shared_ptr<KcpSocketLikeImpl> impl = dynamic_pointer_cast<KcpSocketLikeImpl>(socket);
    if (!impl) {
        return shared_ptr<KcpSocket>();
    } else {
        return impl->s;
    }
}

}  // namespace qtng
