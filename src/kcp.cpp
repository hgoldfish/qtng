#include "qtng/private/kcp.h"
#include "qtng/socket_utils.h"
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
#include "qtng/utils/random.h"
#include "qtng/private/socket_p.h"
#include "./kcp/ikcp.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/string_utils.h"
#include <cstring>
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.kcp_stream");

namespace qtng {

const char PACKET_TYPE_UNCOMPRESSED_DATA = 0x01;
const char PACKET_TYPE_CREATE_MULTIPATH = 0x02;
const char PACKET_TYPE_CLOSE = 0X03;
const char PACKET_TYPE_KEEPALIVE = 0x04;


DatagramPath::DatagramPath()
{
}

DatagramPath::DatagramPath(const string &key)
    : m_key(key)
{
}

string DatagramPath::key() const { return m_key; }

bool DatagramPath::isNull() const
{
    return m_key.empty();
}

bool DatagramPath::operator==(const DatagramPath &other) const { return m_key == other.m_key; }
bool DatagramPath::operator<(const DatagramPath &other) const { return m_key < other.m_key; }

DatagramLink::~DatagramLink() {}
Socket::SocketError DatagramLink::error() const { return Socket::NoError; }
string DatagramLink::errorString() const { return string(); }

//#define DEBUG_PROTOCOL 1

class SlaveKcpStreamPrivate;
class KcpStreamPrivate
{
public:
    KcpStreamPrivate(KcpStream *q);
    virtual ~KcpStreamPrivate();
public:
    virtual Socket::SocketError getError() const = 0;
    virtual string getErrorString() const = 0;
    virtual bool isValid() const = 0;
public:
    virtual KcpStream *accept() = 0;
    virtual KcpStream *accept(const DatagramPath &remote) = 0;
    virtual bool connect(const DatagramPath &remote) = 0;
    virtual bool close(bool force) = 0;
    virtual bool listen(int backlog) = 0;
public:
    void setMode(KcpStream::Mode mode);
    int32_t send(const char *data, int32_t size, bool all);
    int32_t recv(char *data, int32_t size, bool all);
    int32_t peek(char *data, int32_t size);
    bool handleDatagram(const char *buf, uint32_t len);
    void updateKcp();
    void updateStatus();
    void doUpdate();
    virtual int32_t rawSend(const char *data, int32_t size) = 0;

    string makeDataPacket(const char *data, int32_t size);
    string makeShutdownPacket(uint32_t sessionId);
    string makeKeepalivePacket();
    string makeMultiPathPacket(uint32_t sessionId);
public:
    KcpStream * const q_ptr;
    NG_DECLARE_PUBLIC(KcpStream)
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
    uint32_t sessionId;

    DatagramPath remotePath;

    KcpStream::Mode mode;
    KcpStream::HeaderMode headerMode;
};


class MasterKcpStreamPrivate : public KcpStreamPrivate
{
public:
    explicit MasterKcpStreamPrivate(shared_ptr<DatagramLink> link, KcpStream *q);
    virtual ~MasterKcpStreamPrivate() override;
public:
    virtual Socket::SocketError getError() const override;
    virtual string getErrorString() const override;
    virtual bool isValid() const override;
public:
    virtual KcpStream *accept() override;
    virtual KcpStream *accept(const DatagramPath &remote) override;
    virtual bool connect(const DatagramPath &remote) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
public:
    virtual int32_t rawSend(const char *data, int32_t size) override;
public:
    void removeSlave(const string &originalHostAndPort) { receiversByHostAndPort.erase(originalHostAndPort); }
    void removeSlave(uint32_t sessionId) { receiversBySessionId.erase(sessionId); }
    uint32_t nextSessionId();
    void doReceive();
    void doAccept();
    bool startReceivingCoroutine();
public:
    map<string, SlaveKcpStreamPrivate *> receiversByHostAndPort;
    map<uint32_t, SlaveKcpStreamPrivate *> receiversBySessionId;
    shared_ptr<DatagramLink> link;
    Queue<KcpStream *> pendingSlaves;
};

class SlaveKcpStreamPrivate : public KcpStreamPrivate
{
public:
    SlaveKcpStreamPrivate(MasterKcpStreamPrivate *parent, const DatagramPath &remote, KcpStream *q);
    virtual ~SlaveKcpStreamPrivate() override;
public:
    static KcpStream *create(KcpStreamPrivate *d, const DatagramPath &remote, KcpStream::Mode mode);
    static SlaveKcpStreamPrivate *getPrivateHelper(KcpStream *s);
public:
    virtual Socket::SocketError getError() const override;
    virtual string getErrorString() const override;
    virtual bool isValid() const override;
public:
    virtual KcpStream *accept() override;
    virtual KcpStream *accept(const DatagramPath &remote) override;
    virtual bool connect(const DatagramPath &remote) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
public:
    virtual int32_t rawSend(const char *data, int32_t size) override;
public:
    string originalHostAndPort;
    MasterKcpStreamPrivate *parent;
};

KcpStream *SlaveKcpStreamPrivate::create(KcpStreamPrivate *d, const DatagramPath &remote, KcpStream::Mode mode)
{
    return new KcpStream(d, remote, mode);
}

SlaveKcpStreamPrivate *SlaveKcpStreamPrivate::getPrivateHelper(KcpStream *s)
{
    return static_cast<SlaveKcpStreamPrivate *>(s->d_ptr);
}

int kcp_callback(const char *buf, int len, ikcpcb *, void *user)
{
    KcpStreamPrivate *p = static_cast<KcpStreamPrivate *>(user);
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

KcpStreamPrivate::KcpStreamPrivate(KcpStream *q)
    : q_ptr(q)
    , operations(new CoroutineGroup)
    , state(Socket::UnconnectedState)
    , error(Socket::NoError)
    , zeroTimestamp(static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch()))
    , lastActiveTimestamp(zeroTimestamp)
    , lastKeepaliveTimestamp(zeroTimestamp)
    , tearDownTime(1000 * 30)
    , waterLine(1024)
    , sessionId(0)
    , mode(KcpStream::Internet)
    , headerMode(KcpStream::Builtin)
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

KcpStreamPrivate::~KcpStreamPrivate()
{
    delete operations;
    ikcp_release(kcp);
}

//{
// }

void KcpStreamPrivate::setMode(KcpStream::Mode mode)
{
    this->mode = mode;
    switch (mode) {
    case KcpStream::LargeDelayInternet:
        waterLine = 512;
        ikcp_nodelay(kcp, 0, 20, 32, 1);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 1024, 1024);
        break;
    case KcpStream::Internet:
        waterLine = 256;
        ikcp_nodelay(kcp, 1, 10, 16, 1);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 1024, 1024);
        kcp->rx_minrto = 30;
        // kcp->interval = 5;
        break;
    case KcpStream::FastInternet:
        waterLine = 192;
        ikcp_nodelay(kcp, 1, 10, 8, 1);
        ikcp_setmtu(kcp, 1400);
        ikcp_wndsize(kcp, 512, 512);
        kcp->rx_minrto = 20;
        // kcp->interval = 2;
        break;
    case KcpStream::Ethernet:
        waterLine = 64;
        ikcp_nodelay(kcp, 1, 10, 4, 1);
        ikcp_setmtu(kcp, 1024 * 32);
        ikcp_wndsize(kcp, 128, 128);
        kcp->rx_minrto = 10;
        // kcp->interval = 1;
        break;
    case KcpStream::Loopback:
        waterLine = 64;
        ikcp_nodelay(kcp, 1, 10, 1, 1);
        ikcp_setmtu(kcp, 1024 * 64 - 256);
        ikcp_wndsize(kcp, 128, 128);
        kcp->rx_minrto = 5;
        // kcp->interval = 1;
        break;
    }
}

int32_t KcpStreamPrivate::send(const char *data, int32_t size, bool all)
{
    if (size <= 0 || !isValid()) {
        return -1;
    }

    sendingQueueEmpty.clear();

    int count = 0;
    while (count < size) {
        if (state != Socket::ConnectedState) {
            error = Socket::SocketAccessError;
            errorString = "KcpStream is not connected.";
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

int32_t KcpStreamPrivate::recv(char *data, int32_t size, bool all)
{
    while (true) {
        if (state != Socket::ConnectedState) {
            error = Socket::SocketAccessError;
            errorString = "KcpStream is not connected.";
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

int32_t KcpStreamPrivate::peek(char *data, int32_t size)
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

bool KcpStreamPrivate::handleDatagram(const char *buf, uint32_t len)
{
    if (headerMode == KcpStream::External) {
        if (len < 1) {
            return true;
        }
    } else if (len < 5) {
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

void KcpStreamPrivate::doUpdate()
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
            errorString = "KcpStream is timeout.";
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

void KcpStreamPrivate::updateKcp()
{
    shared_ptr<Coroutine> t = operations->spawnWithName(
            "update_kcp", [this] { doUpdate(); }, false);
    kcp->updated = 0;
    forceToUpdate.open();
}

void KcpStreamPrivate::updateStatus()
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

string KcpStreamPrivate::makeDataPacket(const char *data, int32_t size)
{
    string packet(size + 1, '\0');
    packet[0] = PACKET_TYPE_UNCOMPRESSED_DATA;
    memcpy(&packet[1], data, static_cast<size_t>(size));
    if (headerMode == KcpStream::Builtin) {
        ngToBigEndian<uint32_t>(this->sessionId, &packet[1]);
    }
    return packet;
}

string KcpStreamPrivate::makeShutdownPacket(uint32_t sessionId)
{
    // Pad to a random length in [5, 64) so control packets are not fixed-size.
    const int size = 5 + static_cast<int>(utils::RandomGenerator::global().bounded(64 - 5));
    string packet = randomBytes(size);
    packet[0] = PACKET_TYPE_CLOSE;
    if (headerMode == KcpStream::Builtin) {
        ngToBigEndian<uint32_t>(sessionId, &packet[1]);
    }
    return packet;
}

string KcpStreamPrivate::makeKeepalivePacket()
{
    const int size = 5 + static_cast<int>(utils::RandomGenerator::global().bounded(64 - 5));
    string packet = randomBytes(size);
    packet[0] = PACKET_TYPE_KEEPALIVE;
    if (headerMode == KcpStream::Builtin) {
        ngToBigEndian<uint32_t>(this->sessionId, &packet[1]);
    }
    return packet;
}

string KcpStreamPrivate::makeMultiPathPacket(uint32_t sessionId)
{
    const int size = 5 + static_cast<int>(utils::RandomGenerator::global().bounded(64 - 5));
    string packet = randomBytes(size);
    packet[0] = PACKET_TYPE_CREATE_MULTIPATH;
    if (headerMode == KcpStream::Builtin) {
        ngToBigEndian<uint32_t>(sessionId, &packet[1]);
    }
    return packet;
}

MasterKcpStreamPrivate::MasterKcpStreamPrivate(shared_ptr<DatagramLink> link, KcpStream *q)
    : KcpStreamPrivate(q)
    , link(link)
{
}


MasterKcpStreamPrivate::~MasterKcpStreamPrivate()
{
    MasterKcpStreamPrivate::close(true);
}

Socket::SocketError MasterKcpStreamPrivate::getError() const
{
    if (error != Socket::NoError) {
        return error;
    } else {
        return link->error();
    }
}

string MasterKcpStreamPrivate::getErrorString() const
{
    if (!errorString.empty()) {
        return errorString;
    } else {
        return link->errorString();
    }
}

bool MasterKcpStreamPrivate::isValid() const
{
    return state == Socket::ConnectedState || state == Socket::BoundState || state == Socket::ListeningState;
}


bool MasterKcpStreamPrivate::close(bool force)
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
            const string &packet = makeShutdownPacket(this->sessionId);
            rawSend(packet.data(), packet.size());
        }
    } else if (state == Socket::ListeningState) {
        state = Socket::UnconnectedState;
        map<string, SlaveKcpStreamPrivate *> receiversByHostAndPort(this->receiversByHostAndPort);
        this->receiversByHostAndPort.clear();
        for (const auto &item : receiversByHostAndPort) {
            SlaveKcpStreamPrivate *receiver = item.second;
            if (receiver) {
                receiver->close(force);
            }
        }
        receiversBySessionId.clear();
    } else {  // BoundState
        state = Socket::UnconnectedState;
        link->abort();
        return true;
    }

    while (!pendingSlaves.isEmpty()) {
        delete pendingSlaves.get();
    }
    pendingSlaves.put(nullptr);

    // connected and listen state would do more cleaning work.
    operations->killall();
    // always kill operations before release resources.
    link->abort();
    //    if (force) {
    //        link->abort();
    //    } else {
    //        rawSocket->close();
    //    }
    // awake all pending recv()/send()
    receivingQueueNotEmpty.set();
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
#ifdef DEBUG_PROTOCOL
    ngDebug() << "MasterKcpStreamPrivate::close() done";
#endif
    return true;
}

bool MasterKcpStreamPrivate::listen(int backlog)
{
    if (state != Socket::BoundState || backlog <= 0) {
        return false;
    }
    state = Socket::ListeningState;
    pendingSlaves.setCapacity(static_cast<uint32_t>(backlog));
    return true;
}

uint32_t MasterKcpStreamPrivate::nextSessionId()
{
    uint32_t id;
    do {
        const string bytes = randomBytes(4);
        memcpy(&id, bytes.data(), sizeof(id));
    } while (receiversBySessionId.find(id) != receiversBySessionId.end());
    return id;
}

void MasterKcpStreamPrivate::doReceive()
{
    string buf(1024 * 64, '\0');
    while (true) {
        DatagramPath who;
        int32_t len = link->recvfrom(&buf[0], buf.size(), &who);
        if (len == 0) {
            continue;
        }
        if (len < 0 || who.isNull()) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "KcpStream can not receive packet." << link->errorString();
#endif
            MasterKcpStreamPrivate::close(true);
            return;
        }
        if (headerMode == KcpStream::External) {
            if (len < 1) {
                continue;
            }
            if (!handleDatagram(buf.data(), static_cast<uint32_t>(len))) {
                return;
            }
            continue;
        }
        if (len < 5) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "got invalid kcp packet smaller than 5 bytes." << string(buf.data(), len);
#endif
            continue;
        }

        const uint32_t packetSessionId = ngFromBigEndian<uint32_t>(buf.data() + 1);
        if (packetSessionId == 0) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "the kcp server side returns an invalid packet with zero session id.";
#endif
            continue;
        } else {
            if (this->sessionId == 0) {
                this->sessionId = packetSessionId;
            } else {
                if (packetSessionId != this->sessionId) {
#ifdef DEBUG_PROTOCOL
                    ngDebug() << "the kcp server side returns an invalid packet with mismatched session id.";
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

void MasterKcpStreamPrivate::doAccept()
{
    string buf(1024 * 64, '\0');
    while (true) {
        DatagramPath who;
        int32_t len = link->recvfrom(&buf[0], buf.size(), &who);
        if (len == 0) {
            continue;
        }
        if (len < 0 || who.isNull()) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "KcpStream can not receive packet." << link->errorString();
#endif
            MasterKcpStreamPrivate::close(true);
            return;
        }
        if (len < 5) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "got invalid kcp packet smaller than 5 bytes.";
#endif
            continue;
        }

        uint32_t sessionId = ngFromBigEndian<uint32_t>(buf.data() + 1);
        ngToBigEndian<uint32_t>(0, reinterpret_cast<uint8_t *>(&buf[1]));
        const string &key = who.key();
        SlaveKcpStreamPrivate *receiver = nullptr;
        auto hostIt = receiversByHostAndPort.find(key);
        if (hostIt != receiversByHostAndPort.end()) {
            receiver = hostIt->second;
        }
        if (receiver) {
            receiver->remotePath = who;
            if (sessionId != 0) {
                if (receiver->sessionId == 0) {
                    // only if the slave was created by accept(path), we had zero id.
                    // if this sessionId is unique in client. we add it to the receiversBySessionId map.
                    // if it is not, say sorry, and disable the multipath feature.
                    if (receiversBySessionId.find(sessionId) == receiversBySessionId.end()) {
                        // only happened in the newly accept(path) connections.
                        // or remote create new conn with the same path as old, and the old packet is received.
                        receiver->sessionId = sessionId;
                        receiversBySessionId[sessionId] = receiver;
                    }
                } else if (sessionId != receiver->sessionId) {
#ifdef DEBUG_PROTOCOL
                    ngDebug() << "the client sent a invalid session id";
#endif
                    continue;
                }
            }
            if (!receiver->handleDatagram(buf.data(), static_cast<uint32_t>(len))) {
                receiversByHostAndPort.erase(receiver->originalHostAndPort);
                receiversBySessionId.erase(receiver->sessionId);
            }
        } else {
            if (sessionId != 0) {  // a multipath packet.
                const auto it = receiversBySessionId.find(sessionId);
                receiver = (it != receiversBySessionId.end()) ? it->second : nullptr;
                if (!receiver) {
                    // it must be bad packet.
                    const string &closePacket = makeShutdownPacket(sessionId);
                    if (link->sendto(closePacket.data(), closePacket.size(), who) != static_cast<int32_t>(closePacket.size())) {
                        if (error == Socket::NoError) {
                            error = Socket::SocketResourceError;
                            errorString = "KcpStream can not send packet.";
                        }
#ifdef DEBUG_PROTOCOL
                        ngDebug() << errorString;
#endif
                        MasterKcpStreamPrivate::close(true);
                    }
                } else {
                    assert(sessionId == receiver->sessionId);
                    receiver->remotePath = who;
                    if (!receiver->handleDatagram(buf.data(), static_cast<uint32_t>(len))) {
#ifdef DEBUG_PROTOCOL
                        ngDebug() << "can not handle multipath packet.";
#endif
                        receiversByHostAndPort.erase(receiver->originalHostAndPort);
                        receiversBySessionId.erase(receiver->sessionId);
                    }
                }
            } else if (pendingSlaves.size() < pendingSlaves.capacity()) {  // not full. process new connection.
                unique_ptr<KcpStream> slave(SlaveKcpStreamPrivate::create(this, who, this->mode));
                SlaveKcpStreamPrivate *d = SlaveKcpStreamPrivate::getPrivateHelper(slave.get());
                d->originalHostAndPort = key;
                d->sessionId = nextSessionId();
                if (d->handleDatagram(buf.data(), static_cast<uint32_t>(len))) {
                    receiversByHostAndPort[key] = d;
                    receiversBySessionId[d->sessionId] = d;
                    pendingSlaves.put(slave.release());
                    const string &multiPathPacket = makeMultiPathPacket(d->sessionId);
                    if (link->sendto(multiPathPacket.data(), multiPathPacket.size(), who) != static_cast<int32_t>(multiPathPacket.size())) {
                        if (error == Socket::NoError) {
                            error = Socket::SocketResourceError;
                            errorString = "KcpStream can not send packet.";
                        }
#ifdef DEBUG_PROTOCOL
                        ngDebug() << errorString;
#endif
                        MasterKcpStreamPrivate::close(true);
                    }
                }
            }
        }
    }
}

bool MasterKcpStreamPrivate::startReceivingCoroutine()
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

KcpStream *MasterKcpStreamPrivate::accept()
{
    if (state != Socket::ListeningState) {
        return nullptr;
    }
    startReceivingCoroutine();
    return pendingSlaves.get();
}

KcpStream *MasterKcpStreamPrivate::accept(const DatagramPath &remote)
{
    if (state != Socket::ListeningState || remote.isNull()) {
        return nullptr;
    }
    startReceivingCoroutine();
    const string &key = remote.key();
    SlaveKcpStreamPrivate *receiver;
    receiver = receiversByHostAndPort.at(key);
    if (receiver && receiver->isValid()) {
        return nullptr;
    } else {
        unique_ptr<KcpStream> slave(SlaveKcpStreamPrivate::create(this, remote, this->mode));
        SlaveKcpStreamPrivate *d = SlaveKcpStreamPrivate::getPrivateHelper(slave.get());
        d->originalHostAndPort = key;
        d->updateKcp();
        receiversByHostAndPort[key] = d;
        // the sessionId is generated in server side. accept() is acually a connect().
        // receiversBySessionId[d->sessionId] = d;
        return slave.release();
    }
}


bool MasterKcpStreamPrivate::connect(const DatagramPath &remote)
{
    if ((state != Socket::UnconnectedState && state != Socket::BoundState) || remote.isNull()) {
        return false;
    }
    remotePath = remote;
    state = Socket::ConnectedState;
    return true;
}


int32_t MasterKcpStreamPrivate::rawSend(const char *data, int32_t size)
{
    lastKeepaliveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
    startReceivingCoroutine();
    return link->sendto(data, size, remotePath);
}


SlaveKcpStreamPrivate::SlaveKcpStreamPrivate(MasterKcpStreamPrivate *parent, const DatagramPath &remote, KcpStream *q)
    : KcpStreamPrivate(q)
    , parent(parent)
{
    remotePath = remote;
    state = Socket::ConnectedState;
}

SlaveKcpStreamPrivate::~SlaveKcpStreamPrivate()
{
    SlaveKcpStreamPrivate::close(true);
}

Socket::SocketError SlaveKcpStreamPrivate::getError() const
{
    if (error != Socket::NoError) {
        return error;
    } else {
        if (parent) {
            return parent->link->error();
        } else {
            return Socket::SocketAccessError;
        }
    }
}

string SlaveKcpStreamPrivate::getErrorString() const
{
    if (!errorString.empty()) {
        return errorString;
    } else {
        if (parent) {
            return parent->link->errorString();
        } else {
            return "Invalid socket descriptor";
        }
    }
}

bool SlaveKcpStreamPrivate::isValid() const
{
    return state == Socket::ConnectedState && parent;
}


bool SlaveKcpStreamPrivate::close(bool force)
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
            const string &packet = makeShutdownPacket(this->sessionId);
            rawSend(packet.data(), packet.size());
        }
    } else {  // there can be no other states.
        state = Socket::UnconnectedState;
    }
    operations->killall();
    if (parent) {
        parent->removeSlave(originalHostAndPort);
        parent->removeSlave(sessionId);
        parent = nullptr;
    }
    // await all pending recv()/send()
    receivingQueueNotEmpty.set();
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
    q_ptr->notBusy.set();
    q_ptr->busy.set();
#ifdef DEBUG_PROTOCOL
    ngDebug() << "SlaveKcpStreamPrivate::close() done.";
#endif
    return true;
}

bool SlaveKcpStreamPrivate::listen(int)
{
    return false;
}

KcpStream *SlaveKcpStreamPrivate::accept()
{
    return nullptr;
}

KcpStream *SlaveKcpStreamPrivate::accept(const DatagramPath &)
{
    return nullptr;
}


bool SlaveKcpStreamPrivate::connect(const DatagramPath &)
{
    return false;
}


int32_t SlaveKcpStreamPrivate::rawSend(const char *data, int32_t size)
{
    if (!parent) {
        return -1;
    } else {
        lastKeepaliveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
        return parent->link->sendto(data, size, remotePath);
    }
}



KcpStream::KcpStream(shared_ptr<DatagramLink> link)
    : d_ptr(new MasterKcpStreamPrivate(link, this))
{
}


KcpStream::KcpStream(KcpStreamPrivate *parent, const DatagramPath &remote, KcpStream::Mode mode)
    : d_ptr(new SlaveKcpStreamPrivate(static_cast<MasterKcpStreamPrivate *>(parent), remote, this))
{
    setMode(mode);
}

KcpStream::~KcpStream()
{
    delete d_ptr;
}

void KcpStream::setMode(Mode mode)
{
    NG_D(KcpStream);
    d->setMode(mode);
}

KcpStream::Mode KcpStream::mode() const
{
    NG_D(const KcpStream);
    return d->mode;
}

void KcpStream::setPacketSize(uint32_t udpPacketSize)
{
    NG_D(const KcpStream);
    if (udpPacketSize < 65535) {
        ikcp_setmtu(d->kcp, static_cast<int>(udpPacketSize));
    }
}

uint32_t KcpStream::packetSize() const
{
    NG_D(const KcpStream);
    return d->kcp->mtu;
}

void KcpStream::setSendQueueSize(uint32_t sendQueueSize)
{
    NG_D(KcpStream);
    d->waterLine = sendQueueSize;
}

uint32_t KcpStream::sendQueueSize() const
{
    NG_D(const KcpStream);
    return d->waterLine;
}

uint32_t KcpStream::payloadSizeHint() const
{
    NG_D(const KcpStream);
    return d->kcp->mss;
}

void KcpStream::setTearDownTime(float secs)
{
    NG_D(KcpStream);
    if (secs > 0) {
        d->tearDownTime = static_cast<uint64_t>(secs * 1000);
        if (d->tearDownTime < 1000) {
            d->tearDownTime = 1000;
        }
    }
}

float KcpStream::tearDownTime() const
{
    NG_D(const KcpStream);
    return d->tearDownTime / 1000.0f;
}

Socket::SocketError KcpStream::error() const
{
    NG_D(const KcpStream);
    return d->getError();
}

string KcpStream::errorString() const
{
    NG_D(const KcpStream);
    return d->getErrorString();
}

bool KcpStream::isValid() const
{
    NG_D(const KcpStream);
    return d->isValid();
}

Socket::SocketState KcpStream::state() const
{
    NG_D(const KcpStream);
    return d->state;
}

KcpStream *KcpStream::accept()
{
    NG_D(KcpStream);
    return d->accept();
}

void KcpStream::close()
{
    NG_D(KcpStream);
    d->close(false);
}

void KcpStream::abort()
{
    NG_D(KcpStream);
    d->close(true);
}

bool KcpStream::listen(int backlog)
{
    NG_D(KcpStream);
    return d->listen(backlog);
}

int32_t KcpStream::peek(char *data, int32_t size)
{
    NG_D(KcpStream);
    return d->peek(data, size);
}


int32_t KcpStream::recv(char *data, int32_t size)
{
    NG_D(KcpStream);
    return d->recv(data, size, false);
}

int32_t KcpStream::recvall(char *data, int32_t size)
{
    NG_D(KcpStream);
    return d->recv(data, size, true);
}

int32_t KcpStream::send(const char *data, int32_t size)
{
    NG_D(KcpStream);
    int32_t bytesSent = d->send(data, size, false);
    if (bytesSent == 0 && !d->isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}

int32_t KcpStream::sendall(const char *data, int32_t size)
{
    NG_D(KcpStream);
    return d->send(data, size, true);
}

string KcpStream::recv(int32_t size)
{
    NG_D(KcpStream);
    string bs(size, '\0');

    int32_t bytes = d->recv(&bs[0], bs.size(), false);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return string();
}

string KcpStream::recvall(int32_t size)
{
    NG_D(KcpStream);
    string bs(size, '\0');

    int32_t bytes = d->recv(&bs[0], bs.size(), true);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return string();
}

int32_t KcpStream::send(const string &data)
{
    NG_D(KcpStream);
    int32_t bytesSent = d->send(data.data(), data.size(), false);
    if (bytesSent == 0 && !d->isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}

int32_t KcpStream::sendall(const string &data)
{
    NG_D(KcpStream);
    return d->send(data.data(), data.size(), true);
}

shared_ptr<DatagramLink> KcpStream::link() const
{
    NG_D(const KcpStream);
    const MasterKcpStreamPrivate *master = dynamic_cast<const MasterKcpStreamPrivate *>(d);
    if (master) {
        return master->link;
    }
    const SlaveKcpStreamPrivate *slave = dynamic_cast<const SlaveKcpStreamPrivate *>(d);
    if (slave && slave->parent) {
        return slave->parent->link;
    }
    return shared_ptr<DatagramLink>();
}

void KcpStream::setHeaderMode(HeaderMode mode)
{
    NG_D(KcpStream);
    d->headerMode = mode;
}

KcpStream::HeaderMode KcpStream::headerMode() const
{
    NG_D(const KcpStream);
    return d->headerMode;
}

uint32_t KcpStream::sessionId() const
{
    NG_D(const KcpStream);
    return d->sessionId;
}

void KcpStream::setSessionId(uint32_t id)
{
    NG_D(KcpStream);
    d->sessionId = id;
}

DatagramPath KcpStream::peerPath() const
{
    NG_D(const KcpStream);
    return d->remotePath;
}

bool KcpStream::connect(const DatagramPath &remote)
{
    NG_D(KcpStream);
    return d->connect(remote);
}


bool KcpStream::markBound()
{
    NG_D(KcpStream);
    if (d->state != Socket::UnconnectedState) {
        return false;
    }
    d->state = Socket::BoundState;
    return true;
}

KcpStream *KcpStream::accept(const DatagramPath &remote)
{
    NG_D(KcpStream);
    return d->accept(remote);
}



}  // namespace qtng
