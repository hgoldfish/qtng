#include "qtng/udp.h"
#include "qtng/utp.h"
#include "qtng/private/udp_p.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/coroutine.h"
#include "qtng/coroutine_utils.h"
#include "qtng/network_interface.h"
#include "qtng/random.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/string_utils.h"

using namespace std;

NG_LOGGER("qtng.utp_stream");

namespace qtng {

namespace {

constexpr std::uint32_t kUtpHeaderSize = 20;
constexpr std::uint8_t kUtpVersion = 1;

constexpr std::uint8_t ST_DATA = 0;
constexpr std::uint8_t ST_FIN = 1;
constexpr std::uint8_t ST_STATE = 2;
constexpr std::uint8_t ST_RESET = 3;
constexpr std::uint8_t ST_SYN = 4;

constexpr std::uint32_t kMinPayloadSize = 150;
constexpr std::uint32_t kDefaultMaxWindow = 256 * 1024;
constexpr float kDefaultDelayTargetMs = 100.0f;
constexpr float kDefaultIdleTimeoutSec = 0.0f;
constexpr std::uint32_t kDefaultPacketSize = 1400;
constexpr std::uint32_t kDefaultReceiveBuffer = 256 * 1024;
constexpr std::uint32_t kInitialTimeoutMs = 1000;

inline std::uint64_t currentMicros()
{
    return static_cast<std::uint64_t>(utils::DateTime::currentMSecsSinceEpoch()) * 1000;
}

inline std::uint16_t seqInc(std::uint16_t seq, std::uint16_t delta = 1)
{
    return static_cast<std::uint16_t>(seq + delta);
}

// Signed 16-bit sequence distance: positive when `a` is ahead of `b` within
// half the sequence space, negative when behind. Comparisons like
// seqDiff(x, y) >= 0 are only correct with a signed result.
inline std::int16_t seqDiff(std::uint16_t a, std::uint16_t b)
{
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b));
}

struct UtpWireHeader
{
    std::uint8_t ver_type;
    std::uint8_t ext;
    std::uint16_t connId;
    std::uint32_t timestamp;
    std::uint32_t replyMicro;
    std::uint32_t wndSize;
    std::uint16_t seqNr;
    std::uint16_t ackNr;
};

bool decodeHeader(const char *data, std::int32_t len, UtpWireHeader *hdr)
{
    if (len < static_cast<std::int32_t>(kUtpHeaderSize) || !hdr) {
        return false;
    }
    const auto *p = reinterpret_cast<const std::uint8_t *>(data);
    hdr->ver_type = p[0];
    hdr->ext = p[1];
    hdr->connId = ngFromBigEndian<std::uint16_t>(p + 2);
    hdr->timestamp = ngFromBigEndian<std::uint32_t>(p + 4);
    hdr->replyMicro = ngFromBigEndian<std::uint32_t>(p + 8);
    hdr->wndSize = ngFromBigEndian<std::uint32_t>(p + 12);
    hdr->seqNr = ngFromBigEndian<std::uint16_t>(p + 16);
    hdr->ackNr = ngFromBigEndian<std::uint16_t>(p + 18);
    return true;
}

void encodeHeader(const UtpWireHeader &hdr, char *out)
{
    auto *p = reinterpret_cast<std::uint8_t *>(out);
    p[0] = hdr.ver_type;
    p[1] = hdr.ext;
    ngToBigEndian(hdr.connId, p + 2);
    ngToBigEndian(hdr.timestamp, p + 4);
    ngToBigEndian(hdr.replyMicro, p + 8);
    ngToBigEndian(hdr.wndSize, p + 12);
    ngToBigEndian(hdr.seqNr, p + 16);
    ngToBigEndian(hdr.ackNr, p + 18);
}

std::uint8_t packetType(std::uint8_t verType)
{
    return static_cast<std::uint8_t>(verType >> 4);
}

std::uint8_t makeVerType(std::uint8_t type)
{
    return static_cast<std::uint8_t>((type << 4) | (kUtpVersion & 0x0f));
}

struct SentPacket
{
    std::string bytes;
    std::uint16_t seq;
    std::uint64_t sentMicros;
    bool needAck;
    int transmitCount;
    std::uint32_t rto;  // per-packet retransmit timeout (usec-based backoff)
};

}  // namespace

class MasterUtpStreamPrivate;

class UtpStreamPrivate
{
public:
    explicit UtpStreamPrivate(UtpStream *q);
    virtual ~UtpStreamPrivate();

    virtual Socket::SocketError getError() const = 0;
    virtual std::string getErrorString() const = 0;
    virtual bool isValid() const = 0;
    virtual UtpStream *accept() = 0;
    virtual UtpStream *accept(const DatagramPath &remote) = 0;
    virtual bool connect(const DatagramPath &remote) = 0;
    virtual bool close(bool force) = 0;
    virtual bool listen(int backlog) = 0;
    virtual int32_t rawSend(const char *data, int32_t size) = 0;

    int32_t send(const char *data, int32_t size, bool all);
    int32_t recv(char *data, int32_t size, bool all);
    int32_t peek(char *data, int32_t size);

    void ackPackets(std::uint16_t ackNrNew);
    bool handleDatagram(const char *buf, int32_t len, const DatagramPath &remote);
    void doUpdate();
    void updateTimers();
    void flushOutgoing();
    void sendPacket(std::uint8_t type, std::uint16_t seqOverride, const char *payload, int32_t payloadLen,
                    bool advanceSeq);
    void sendAck();
    std::string buildPacket(std::uint8_t type, std::uint16_t seqOverride, const char *payload, int32_t payloadLen,
                            bool advanceSeq);
    void noteActivity();
    void enterConnected();
    std::uint32_t effectivePayloadSize() const;

    UtpStream * const q_ptr;
    CoroutineGroup *operations;
    std::string errorString;
    Socket::SocketState state;
    Socket::SocketError error;

    Event sendingQueueNotFull;
    Event sendingQueueEmpty;
    Event receivingQueueNotEmpty;
    Event connectedEvent;
    Gate forceToUpdate;

    std::string receivingBuffer;
    std::deque<SentPacket> sendQueue;
    std::map<std::uint16_t, std::string> outOfOrder;

    DatagramPath remotePath;
    MasterUtpStreamPrivate *parent;

    std::uint16_t seqNr;
    std::uint16_t ackNr;
    std::uint16_t recvConnId;
    std::uint16_t sendConnId;

    std::uint32_t curWindow;
    std::uint32_t maxWindow;
    std::uint32_t packetSize;
    std::uint32_t receiveBufferSize;
    float delayTargetMs;
    float idleTimeoutSec;

    std::uint32_t replyMicro;
    std::uint32_t baseDelay;  // min-RTT baseline in microseconds

    // Advertised receive window of the peer (bytes). Updated from every
    // incoming packet's wndSize field so the sender respects the peer's
    // application-level backpressure, not just its own congestion window.
    std::uint32_t peerWindow;

    std::uint32_t timeoutMs;

    std::uint64_t lastActivityMicros;

    bool finSent;
    bool finReceived;
    bool receivingCoroutineStarted = false;
};

class MasterUtpStreamPrivate : public UtpStreamPrivate
{
public:
    MasterUtpStreamPrivate(std::shared_ptr<DatagramLink> link, UtpStream *q);
    ~MasterUtpStreamPrivate() override;

    Socket::SocketError getError() const override;
    std::string getErrorString() const override;
    bool isValid() const override;
    UtpStream *accept() override;
    UtpStream *accept(const DatagramPath &remote) override;
    bool connect(const DatagramPath &remote) override;
    bool close(bool force) override;
    bool listen(int backlog) override;
    int32_t rawSend(const char *data, int32_t size) override;

    void doReceive();
    void doAccept();
    bool startReceivingCoroutine();
    UtpStream *findSlave(const DatagramPath &remote, std::uint16_t connId);
    void registerSlave(const std::string &pathKey, UtpStream *slave);
    void unregisterSlave(UtpStream *slave);
    void pumpDatagram();

    std::shared_ptr<DatagramLink> link;
    Queue<UtpStream *> pendingSlaves;
    std::map<std::string, UtpStream *> slavesByPath;
};

class SlaveUtpStreamPrivate : public UtpStreamPrivate
{
public:
    SlaveUtpStreamPrivate(MasterUtpStreamPrivate *parent, const DatagramPath &remote, UtpStream *q,
                          std::uint16_t synConnId, std::uint16_t synSeq);
    ~SlaveUtpStreamPrivate() override;

    Socket::SocketError getError() const override;
    std::string getErrorString() const override;
    bool isValid() const override;
    UtpStream *accept() override;
    UtpStream *accept(const DatagramPath &remote) override;
    bool connect(const DatagramPath &remote) override;
    bool close(bool force) override;
    bool listen(int backlog) override;
    int32_t rawSend(const char *data, int32_t size) override;

    std::string pathKey;
};

UtpStreamPrivate::UtpStreamPrivate(UtpStream *q)
    : q_ptr(q)
    , operations(new CoroutineGroup)
    , state(Socket::UnconnectedState)
    , error(Socket::NoError)
    , parent(nullptr)
    , seqNr(1)
    , ackNr(0)
    , recvConnId(0)
    , sendConnId(0)
    , curWindow(0)
    , maxWindow(kDefaultMaxWindow)
    , packetSize(kDefaultPacketSize)
    , receiveBufferSize(kDefaultReceiveBuffer)
    , delayTargetMs(kDefaultDelayTargetMs)
    , idleTimeoutSec(kDefaultIdleTimeoutSec)
    , replyMicro(0)
    , baseDelay(0)
    , peerWindow(kDefaultMaxWindow)
    , timeoutMs(1000)
    , lastActivityMicros(currentMicros())
    , finSent(false)
    , finReceived(false)
{
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
    receivingQueueNotEmpty.clear();
    connectedEvent.clear();
    q_ptr->busy.clear();
    q_ptr->notBusy.set();
}

UtpStreamPrivate::~UtpStreamPrivate()
{
    delete operations;
}

std::uint32_t UtpStreamPrivate::effectivePayloadSize() const
{
    if (packetSize <= kUtpHeaderSize + kMinPayloadSize) {
        return kMinPayloadSize;
    }
    return packetSize - kUtpHeaderSize;
}

void UtpStreamPrivate::noteActivity()
{
    lastActivityMicros = currentMicros();
}

void UtpStreamPrivate::enterConnected()
{
    if (state == Socket::ConnectedState) {
        return;
    }
    state = Socket::ConnectedState;
    connectedEvent.set();
    receivingQueueNotEmpty.set();
    operations->spawnWithName("update_utp", [this] { doUpdate(); }, false);
}

std::string UtpStreamPrivate::buildPacket(std::uint8_t type, std::uint16_t seqOverride, const char *payload,
                                          int32_t payloadLen, bool advanceSeq)
{
    UtpWireHeader hdr;
    hdr.ver_type = makeVerType(type);
    hdr.ext = 0;
    hdr.connId = (type == ST_SYN) ? recvConnId : sendConnId;
    hdr.timestamp = static_cast<std::uint32_t>(currentMicros() & 0xffffffffu);
    hdr.replyMicro = replyMicro;
    const std::uint32_t recvAvail = receiveBufferSize > receivingBuffer.size()
            ? static_cast<std::uint32_t>(receiveBufferSize - receivingBuffer.size())
            : 0;
    hdr.wndSize = recvAvail;
    if (seqOverride != 0) {
        hdr.seqNr = seqOverride;
    } else {
        hdr.seqNr = seqNr;
        if (advanceSeq && type != ST_STATE && type != ST_SYN) {
            seqNr = seqInc(seqNr);
        }
    }
    hdr.ackNr = ackNr;

    std::string packet(kUtpHeaderSize + static_cast<size_t>(payloadLen > 0 ? payloadLen : 0), '\0');
    encodeHeader(hdr, &packet[0]);
    if (payloadLen > 0 && payload) {
        memcpy(&packet[kUtpHeaderSize], payload, static_cast<size_t>(payloadLen));
    }
    return packet;
}

void UtpStreamPrivate::sendPacket(std::uint8_t type, std::uint16_t seqOverride, const char *payload, int32_t payloadLen,
                                  bool advanceSeq)
{
    const std::string packet = buildPacket(type, seqOverride, payload, payloadLen, advanceSeq);
    if (type == ST_DATA || type == ST_FIN) {
        SentPacket sp;
        sp.bytes = packet;
        sp.seq = ngFromBigEndian<std::uint16_t>(packet.data() + 16);
        sp.sentMicros = currentMicros();
        sp.needAck = true;
        sp.transmitCount = 1;
        sp.rto = timeoutMs;
        curWindow += static_cast<std::uint32_t>(packet.size());
        sendQueue.push_back(sp);
        sendingQueueEmpty.clear();
    }
    rawSend(packet.data(), static_cast<int32_t>(packet.size()));
}

void UtpStreamPrivate::sendAck()
{
    sendPacket(ST_STATE, 0, nullptr, 0, false);
}

void UtpStreamPrivate::ackPackets(std::uint16_t ackNrNew)
{
    while (!sendQueue.empty() && seqDiff(ackNrNew, sendQueue.front().seq) >= 0) {
        curWindow -= static_cast<std::uint32_t>(sendQueue.front().bytes.size());
        sendQueue.pop_front();
    }
    if (sendQueue.empty()) {
        sendingQueueNotFull.set();
        sendingQueueEmpty.set();
        q_ptr->busy.clear();
        q_ptr->notBusy.set();
        // All outstanding data acknowledged: the retransmit backoff must reset,
        // otherwise a single early loss would leave the RTO inflated for the
        // rest of the connection and make every later loss recovery very slow.
        timeoutMs = kInitialTimeoutMs;
    } else if (timeoutMs > kInitialTimeoutMs) {
        // Progress on a busy window: decay the backoff toward the base RTO.
        timeoutMs = max(kInitialTimeoutMs, timeoutMs / 2);
    }
}

void UtpStreamPrivate::flushOutgoing()
{
    const std::uint64_t now = currentMicros();
    for (auto &pkt : sendQueue) {
        if (!pkt.needAck) {
            continue;
        }
        const std::uint64_t elapsedMs = (now - pkt.sentMicros) / 1000;
        if (elapsedMs > pkt.rto) {
            pkt.transmitCount++;
            if (pkt.transmitCount > 5) {
                error = Socket::SocketTimeoutError;
                errorString = "uTP retransmit timeout";
                close(true);
                return;
            }
            // Per-packet backoff: retransmitting one packet must not inflate the
            // RTO of every other outstanding packet (that would make recovering
            // many lost packets take exponentially long).
            pkt.rto = min(pkt.rto * 2, 60000u);
            pkt.sentMicros = now;
            rawSend(pkt.bytes.data(), static_cast<int32_t>(pkt.bytes.size()));
        }
    }
}

void UtpStreamPrivate::updateTimers()
{
    const std::uint64_t now = currentMicros();
    if (idleTimeoutSec > 0 && state == Socket::ConnectedState) {
        const std::int64_t deltaUs =
                static_cast<std::int64_t>(now) - static_cast<std::int64_t>(lastActivityMicros);
        if (deltaUs > 0) {
            const float idleSec = static_cast<float>(deltaUs / 1000000.0);
            if (idleSec > idleTimeoutSec) {
                error = Socket::SocketTimeoutError;
                errorString = "uTP idle timeout";
                close(true);
                return;
            }
        }
    }
    flushOutgoing();
}

bool UtpStreamPrivate::handleDatagram(const char *buf, int32_t len, const DatagramPath &remote)
{
    UtpWireHeader hdr;
    if (!decodeHeader(buf, len, &hdr)) {
        return true;
    }
    const std::uint8_t type = packetType(hdr.ver_type);

    if (type != ST_SYN && recvConnId != 0 && hdr.connId != recvConnId) {
        return true;
    }

    noteActivity();
    remotePath = remote;
    replyMicro = hdr.timestamp;

    // LEDBAT-style congestion control. The peer echoes the timestamp we put in
    // the packet it is acknowledging, so (now - replyMicro) is a genuine RTT
    // sample; queuing delay is the excess over the minimum observed RTT. The
    // old code compared two wall-clock timestamps of our own sends, which grew
    // monotonically with connection age and collapsed maxWindow to the floor.
    peerWindow = hdr.wndSize;
    if (hdr.replyMicro > 0) {
        const std::uint32_t nowUs = static_cast<std::uint32_t>(currentMicros() & 0xffffffffu);
        const std::uint32_t rtt = nowUs - hdr.replyMicro;
        if (rtt <= 10000000) {  // ignore stale samples (>10s)
            if (baseDelay == 0 || rtt < baseDelay) {
                baseDelay = rtt;
            }
            const std::uint32_t ourDelay = rtt > baseDelay ? rtt - baseDelay : 0;
            const std::uint32_t targetUs = static_cast<std::uint32_t>(delayTargetMs * 1000.0f);
            if (ourDelay > targetUs && maxWindow > kMinPayloadSize * 4) {
                maxWindow = maxWindow * 3 / 4;
            } else if (ourDelay < targetUs) {
                maxWindow = min(maxWindow + kMinPayloadSize, kDefaultMaxWindow * 2);
            }
        }
    }

    if (type == ST_SYN) {
        recvConnId = static_cast<std::uint16_t>(hdr.connId + 1);
        sendConnId = hdr.connId;
        ackNr = hdr.seqNr;
        sendAck();
        enterConnected();
        return true;
    }

    if (state == Socket::ConnectingState && type == ST_STATE) {
        // Match libutp: SYN-ACK completes the active side; do not emit a STATE
        // that reuses the next DATA sequence number.
        ackNr = static_cast<std::uint16_t>(hdr.seqNr - 1);
        enterConnected();
        return true;
    }

    if (type == ST_RESET) {
        error = Socket::RemoteHostClosedError;
        errorString = "uTP reset";
        close(true);
        return false;
    }

    if (state != Socket::ConnectedState && state != Socket::ConnectingState) {
        return true;
    }

    if (seqDiff(hdr.ackNr, ackNr) > 0 || hdr.ackNr == ackNr) {
        ackPackets(hdr.ackNr);
    }

    if (type == ST_DATA || type == ST_FIN) {
        const int32_t payloadLen = len - static_cast<int32_t>(kUtpHeaderSize);
        const std::uint16_t expected = seqInc(ackNr);
        if (hdr.seqNr == expected) {
            if (payloadLen > 0) {
                receivingBuffer.append(buf + kUtpHeaderSize, static_cast<size_t>(payloadLen));
            }
            ackNr = hdr.seqNr;
            while (!outOfOrder.empty()) {
                auto it = outOfOrder.find(seqInc(ackNr));
                if (it == outOfOrder.end()) {
                    break;
                }
                receivingBuffer.append(it->second);
                ackNr = it->first;
                outOfOrder.erase(it);
            }
            receivingQueueNotEmpty.set();
            sendAck();
        } else if (seqDiff(hdr.seqNr, expected) > 0 && seqDiff(hdr.seqNr, expected) < 0x8000) {
            if (payloadLen >= 0) {
                outOfOrder[hdr.seqNr] = std::string(buf + kUtpHeaderSize, static_cast<size_t>(payloadLen));
            }
            sendAck();
        } else if (payloadLen > 0) {
            // Duplicate (already received) DATA: the original ACK may have been
            // lost, so re-ack it. Without this the sender would retransmit the
            // packet forever and eventually hit the retransmit limit.
            sendAck();
        }
        if (type == ST_FIN) {
            finReceived = true;
        }
    }

    return true;
}

void UtpStreamPrivate::doUpdate()
{
    while (state == Socket::ConnectedState || state == Socket::ConnectingState) {
        updateTimers();
        // forceToUpdate is a Gate: a Gate starts open and Gate::tryWait() returns
        // immediately on an open gate without consuming it, so the wait above would
        // never yield and doUpdate would spin on the event loop forever. Close the
        // gate first so tryWait(50) actually sleeps (same pattern as KCP doUpdate).
        forceToUpdate.close();
        forceToUpdate.tryWait(50);
    }
}

int32_t UtpStreamPrivate::send(const char *data, int32_t size, bool all)
{
    if (size <= 0 || !isValid()) {
        return -1;
    }
    if (state != Socket::ConnectedState) {
        error = Socket::SocketAccessError;
        errorString = "UtpStream is not connected.";
        return -1;
    }

    int count = 0;
    while (count < size) {
        const std::uint32_t effectiveWindow = min(maxWindow, peerWindow);
        if (curWindow >= effectiveWindow) {
            flushOutgoing();
            if (!sendingQueueNotFull.tryWait(500)) {
                return count > 0 ? count : -1;
            }
        }
        const int32_t chunk = min<int32_t>(static_cast<int32_t>(effectivePayloadSize()), size - count);
        sendPacket(ST_DATA, 0, data + count, chunk, true);
        count += chunk;
        if (!all) {
            return count;
        }
    }
    return isValid() ? count : -1;
}

int32_t UtpStreamPrivate::recv(char *data, int32_t size, bool all)
{
    while (true) {
        if (finReceived && receivingBuffer.empty()) {
            return 0;
        }
        if (state != Socket::ConnectedState && !(finReceived && !receivingBuffer.empty())) {
            error = Socket::SocketAccessError;
            errorString = "UtpStream is not connected.";
            return -1;
        }
        if (!receivingBuffer.empty()) {
            if (!all || static_cast<int32_t>(receivingBuffer.size()) >= size) {
                const int32_t len = min<int32_t>(size, static_cast<int32_t>(receivingBuffer.size()));
                memcpy(data, receivingBuffer.data(), static_cast<size_t>(len));
                receivingBuffer.erase(0, static_cast<size_t>(len));
                return len;
            }
        }
        if (parent) {
            parent->pumpDatagram();
            if (!receivingBuffer.empty()) {
                continue;
            }
        }
        receivingQueueNotEmpty.clear();
        if (!receivingQueueNotEmpty.tryWait(10)) {
            if (parent) {
                parent->pumpDatagram();
            }
        }
    }
}

int32_t UtpStreamPrivate::peek(char *data, int32_t size)
{
    if (receivingBuffer.empty()) {
        return 0;
    }
    const int32_t len = min<int32_t>(size, static_cast<int32_t>(receivingBuffer.size()));
    memcpy(data, receivingBuffer.data(), static_cast<size_t>(len));
    return len;
}

MasterUtpStreamPrivate::MasterUtpStreamPrivate(std::shared_ptr<DatagramLink> link, UtpStream *q)
    : UtpStreamPrivate(q)
    , link(std::move(link))
{
}

MasterUtpStreamPrivate::~MasterUtpStreamPrivate()
{
    close(true);
}

Socket::SocketError MasterUtpStreamPrivate::getError() const
{
    return error != Socket::NoError ? error : link->error();
}

std::string MasterUtpStreamPrivate::getErrorString() const
{
    return !errorString.empty() ? errorString : link->errorString();
}

bool MasterUtpStreamPrivate::isValid() const
{
    return state == Socket::ConnectedState || state == Socket::BoundState || state == Socket::ListeningState
            || state == Socket::ConnectingState;
}

bool MasterUtpStreamPrivate::listen(int backlog)
{
    if (state != Socket::BoundState || backlog <= 0) {
        return false;
    }
    state = Socket::ListeningState;
    pendingSlaves.setCapacity(static_cast<std::uint32_t>(backlog));
    if (!startReceivingCoroutine()) {
        return false;
    }
    return true;
}

bool MasterUtpStreamPrivate::startReceivingCoroutine()
{
    if (receivingCoroutineStarted) {
        return true;
    }
    if (state == Socket::ConnectedState || state == Socket::ConnectingState) {
        receivingCoroutineStarted = true;
        operations->spawnWithName("receiving", [this] { doReceive(); }, false);
        return true;
    }
    if (state == Socket::ListeningState) {
        receivingCoroutineStarted = true;
        operations->spawnWithName("receiving", [this] { doAccept(); }, false);
        return true;
    }
    return false;
}

bool MasterUtpStreamPrivate::connect(const DatagramPath &remote)
{
    if (state != Socket::BoundState && state != Socket::UnconnectedState) {
        return false;
    }
    remotePath = remote;
    state = Socket::ConnectingState;

    const string rnd = randomBytes(2);
    memcpy(&recvConnId, rnd.data(), 2);
    if (recvConnId == 0) {
        recvConnId = 1;
    }
    sendConnId = static_cast<std::uint16_t>(recvConnId + 1);

    const string seqRnd = randomBytes(2);
    std::uint16_t synSeq = 0;
    memcpy(&synSeq, seqRnd.data(), 2);
    if (synSeq == 0) {
        synSeq = 1;
    }
    sendPacket(ST_SYN, synSeq, nullptr, 0, false);
    seqNr = seqInc(synSeq);
    std::uint64_t lastSynSentMs = utils::DateTime::currentMSecsSinceEpoch();
    std::uint64_t synTimeoutMs = 1000;
    Coroutine::msleep(0);

    // Do not block on link->recvfrom() in a loop: the receive is unbounded (the UDP
    // socket waits for a readable fd forever), so the deadline check below would never
    // be reached and connect() would hang when the SYN goes unanswered. Instead hand
    // the link over to the receiving coroutine (doReceive pumps while ConnectingState)
    // and wait on connectedEvent, which enterConnected() sets on SYN-ACK.
    startReceivingCoroutine();

    const std::uint64_t deadlineMs = utils::DateTime::currentMSecsSinceEpoch() + 10000;
    while (state == Socket::ConnectingState) {
        const std::uint64_t nowMs = utils::DateTime::currentMSecsSinceEpoch();
        if (nowMs >= deadlineMs) {
            break;
        }
        // The SYN (or its SYN-ACK) can be lost on a lossy path; retransmit it
        // with exponential backoff so connect() survives packet loss.
        if (nowMs > lastSynSentMs && nowMs - lastSynSentMs >= synTimeoutMs) {
            sendPacket(ST_SYN, synSeq, nullptr, 0, false);
            lastSynSentMs = nowMs;
            synTimeoutMs = min<std::uint64_t>(synTimeoutMs * 2, 5000);
        }
        connectedEvent.clear();
        connectedEvent.tryWait(static_cast<std::uint32_t>(min<std::uint64_t>(deadlineMs - nowMs, 50)));
    }

    if (state == Socket::ConnectedState) {
        startReceivingCoroutine();
    }

    if (state != Socket::ConnectedState) {
        error = Socket::SocketTimeoutError;
        errorString = "uTP connect timeout";
        close(true);
        return false;
    }
    return true;
}

int32_t MasterUtpStreamPrivate::rawSend(const char *data, int32_t size)
{
    return link->sendto(data, size, remotePath);
}

void MasterUtpStreamPrivate::doReceive()
{
    while (state == Socket::ConnectedState || state == Socket::ConnectingState) {
        pumpDatagram();
        if (state != Socket::ConnectedState && state != Socket::ConnectingState) {
            return;
        }
        Coroutine::msleep(1);
    }
}

void MasterUtpStreamPrivate::doAccept()
{
    while (state == Socket::ListeningState) {
        pumpDatagram();
        Coroutine::msleep(1);
    }
}

UtpStream *MasterUtpStreamPrivate::findSlave(const DatagramPath &remote, std::uint16_t connId)
{
    auto it = slavesByPath.find(remote.key());
    if (it != slavesByPath.end()) {
        return it->second;
    }
    (void) connId;
    return nullptr;
}

void MasterUtpStreamPrivate::registerSlave(const std::string &pathKey, UtpStream *slave)
{
    slavesByPath[pathKey] = slave;
}

void MasterUtpStreamPrivate::unregisterSlave(UtpStream *slave)
{
    if (!slave) {
        return;
    }
    for (auto it = slavesByPath.begin(); it != slavesByPath.end();) {
        if (it->second == slave) {
            it = slavesByPath.erase(it);
        } else {
            ++it;
        }
    }
}

void MasterUtpStreamPrivate::pumpDatagram()
{
    if (!link) {
        return;
    }
    string buf(64 * 1024, '\0');
    DatagramPath who;
    const int32_t len = link->recvfrom(&buf[0], static_cast<int32_t>(buf.size()), &who);
    if (len <= 0 || who.isNull()) {
        return;
    }
    UtpWireHeader hdr;
    if (!decodeHeader(buf.data(), len, &hdr)) {
        return;
    }
    const std::uint8_t type = packetType(hdr.ver_type);
    UtpStream *target = findSlave(who, hdr.connId);
    if (target) {
        target->feedDatagram(buf.data(), len, who);
        return;
    }
    if (state == Socket::ListeningState) {
        if (type != ST_SYN || pendingSlaves.size() >= pendingSlaves.capacity()) {
            return;
        }
        // A retransmitted SYN (connect() retries on loss) must not create a
        // duplicate slave: feed it to the existing one, which replies with
        // another SYN-ACK.
        UtpStream *existing = findSlave(who, hdr.connId);
        if (existing) {
            existing->feedDatagram(buf.data(), len, who);
            return;
        }
        UtpStream *stream =
                new UtpStream(static_cast<UtpStreamPrivate *>(this), who, hdr.connId, hdr.seqNr);
        registerSlave(who.key(), stream);
        pendingSlaves.put(stream);
        return;
    }
    if (state == Socket::ConnectedState || state == Socket::ConnectingState) {
        handleDatagram(buf.data(), len, who);
    }
}

UtpStream *MasterUtpStreamPrivate::accept()
{
    if (state != Socket::ListeningState) {
        return nullptr;
    }
    startReceivingCoroutine();
    return pendingSlaves.get();
}

UtpStream *MasterUtpStreamPrivate::accept(const DatagramPath &remote)
{
    while (true) {
        UtpStream *s = accept();
        if (!s) {
            return nullptr;
        }
        if (s->peerPath() == remote) {
            return s;
        }
        pendingSlaves.put(s);
    }
}

bool MasterUtpStreamPrivate::close(bool force)
{
    if (state == Socket::UnconnectedState) {
        return true;
    }
    if (state == Socket::ConnectedState || state == Socket::ConnectingState) {
        if (!force && error == Socket::NoError && !finSent) {
            sendPacket(ST_FIN, 0, nullptr, 0, true);
            finSent = true;
        }
        state = Socket::UnconnectedState;
    } else if (state == Socket::ListeningState) {
        state = Socket::UnconnectedState;
        vector<UtpStream *> slaves;
        for (const auto &item : slavesByPath) {
            if (item.second) {
                slaves.push_back(item.second);
            }
        }
        slavesByPath.clear();
        if (force) {
            // force-close is called from destructors and must not block (CoroutineGroup::each
            // joins; joining from a killed coroutine raises CoroutineExitException out of a
            // destructor -> terminate). SlaveUtpStreamPrivate::close(true) is non-blocking.
            for (UtpStream *s : slaves) {
                if (s) {
                    s->abort();
                }
            }
        } else {
            try {
                CoroutineGroup::each<UtpStream *>([](UtpStream *s) {
                    if (s) {
                        s->close();
                    }
                }, slaves, 10);
            } catch (...) {
                // close() can run from a destructor while the current coroutine is being killed.
                // each() joins, and joining raises the kill exception out of the destructor. The
                // streams are aborted non-blockingly instead; abort() is idempotent.
                for (UtpStream *s : slaves) {
                    if (s) {
                        s->abort();
                    }
                }
            }
        }
    } else {
        state = Socket::UnconnectedState;
    }

    while (!pendingSlaves.isEmpty()) {
        delete pendingSlaves.get();
    }
    pendingSlaves.put(nullptr);
    operations->killall();
    link->abort();
    receivingQueueNotEmpty.set();
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
    q_ptr->notBusy.set();
    q_ptr->busy.set();
    return true;
}

SlaveUtpStreamPrivate::SlaveUtpStreamPrivate(MasterUtpStreamPrivate *p, const DatagramPath &remote, UtpStream *q,
                                           std::uint16_t synConnId, std::uint16_t synSeq)
    : UtpStreamPrivate(q)
    , pathKey(remote.key())
{
    parent = p;
    remotePath = remote;
    recvConnId = static_cast<std::uint16_t>(synConnId + 1);
    sendConnId = synConnId;
    ackNr = synSeq;
    sendAck();
    enterConnected();
}

SlaveUtpStreamPrivate::~SlaveUtpStreamPrivate()
{
    close(true);
}

Socket::SocketError SlaveUtpStreamPrivate::getError() const
{
    if (error != Socket::NoError) {
        return error;
    }
    return parent ? parent->getError() : Socket::UnknownSocketError;
}

std::string SlaveUtpStreamPrivate::getErrorString() const
{
    if (!errorString.empty()) {
        return errorString;
    }
    return parent ? parent->getErrorString() : string();
}

bool SlaveUtpStreamPrivate::isValid() const
{
    return state == Socket::ConnectedState;
}

UtpStream *SlaveUtpStreamPrivate::accept()
{
    return nullptr;
}

UtpStream *SlaveUtpStreamPrivate::accept(const DatagramPath &)
{
    return nullptr;
}

bool SlaveUtpStreamPrivate::connect(const DatagramPath &)
{
    return false;
}

bool SlaveUtpStreamPrivate::listen(int)
{
    return false;
}

int32_t SlaveUtpStreamPrivate::rawSend(const char *data, int32_t size)
{
    if (!parent) {
        return -1;
    }
    return parent->link->sendto(data, size, remotePath);
}

bool SlaveUtpStreamPrivate::close(bool force)
{
    if (state == Socket::UnconnectedState) {
        return true;
    }
    if (state == Socket::ConnectedState && !force && error == Socket::NoError && !finSent) {
        sendPacket(ST_FIN, 0, nullptr, 0, true);
        finSent = true;
    }
    state = Socket::UnconnectedState;
    operations->killall();
    if (parent) {
        parent->unregisterSlave(q_ptr);
        parent = nullptr;
    }
    receivingQueueNotEmpty.set();
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
    q_ptr->notBusy.set();
    q_ptr->busy.set();
    return true;
}

UtpStream::UtpStream(std::shared_ptr<DatagramLink> link)
    : d_ptr(new MasterUtpStreamPrivate(std::move(link), this))
{
}

UtpStream::UtpStream(UtpStreamPrivate *parent, const DatagramPath &remote, std::uint16_t synConnId,
                     std::uint16_t synSeq)
    : d_ptr(new SlaveUtpStreamPrivate(static_cast<MasterUtpStreamPrivate *>(parent), remote, this, synConnId, synSeq))
{
}

UtpStream::~UtpStream()
{
    delete d_ptr;
}

std::shared_ptr<DatagramLink> UtpStream::link() const
{
    NG_D(const UtpStream);
    const MasterUtpStreamPrivate *master = dynamic_cast<const MasterUtpStreamPrivate *>(d);
    if (master) {
        return master->link;
    }
    const SlaveUtpStreamPrivate *slave = dynamic_cast<const SlaveUtpStreamPrivate *>(d);
    if (slave && slave->parent) {
        return slave->parent->link;
    }
    return std::shared_ptr<DatagramLink>();
}

void UtpStream::setDelayTarget(float milliseconds)
{
    NG_D(UtpStream);
    if (milliseconds > 0) {
        d->delayTargetMs = milliseconds;
    }
}

float UtpStream::delayTarget() const
{
    NG_D(const UtpStream);
    return d->delayTargetMs;
}

void UtpStream::setMaxWindow(std::uint32_t bytes)
{
    NG_D(UtpStream);
    if (bytes >= kMinPayloadSize) {
        d->maxWindow = bytes;
    }
}

std::uint32_t UtpStream::maxWindow() const
{
    NG_D(const UtpStream);
    return d->maxWindow;
}

void UtpStream::setPacketSize(std::uint32_t bytes)
{
    NG_D(UtpStream);
    if (bytes > kUtpHeaderSize + kMinPayloadSize) {
        d->packetSize = bytes;
    }
}

std::uint32_t UtpStream::packetSize() const
{
    NG_D(const UtpStream);
    return d->packetSize;
}

std::uint32_t UtpStream::payloadSizeHint() const
{
    NG_D(const UtpStream);
    return d->effectivePayloadSize();
}

void UtpStream::setReceiveBufferSize(std::uint32_t bytes)
{
    NG_D(UtpStream);
    d->receiveBufferSize = bytes;
}

std::uint32_t UtpStream::receiveBufferSize() const
{
    NG_D(const UtpStream);
    return d->receiveBufferSize;
}

void UtpStream::setIdleTimeout(float seconds)
{
    NG_D(UtpStream);
    if (seconds > 0) {
        d->idleTimeoutSec = seconds;
    }
}

float UtpStream::idleTimeout() const
{
    NG_D(const UtpStream);
    return d->idleTimeoutSec;
}

Socket::SocketError UtpStream::error() const
{
    NG_D(const UtpStream);
    return d->getError();
}

std::string UtpStream::errorString() const
{
    NG_D(const UtpStream);
    return d->getErrorString();
}

bool UtpStream::isValid() const
{
    NG_D(const UtpStream);
    return d->isValid();
}

DatagramPath UtpStream::peerPath() const
{
    NG_D(const UtpStream);
    return d->remotePath;
}

Socket::SocketState UtpStream::state() const
{
    NG_D(const UtpStream);
    return d->state;
}

UtpStream *UtpStream::accept()
{
    NG_D(UtpStream);
    return d->accept();
}

UtpStream *UtpStream::accept(const DatagramPath &remote)
{
    NG_D(UtpStream);
    return d->accept(remote);
}

bool UtpStream::connect(const DatagramPath &remote)
{
    NG_D(UtpStream);
    return d->connect(remote);
}

bool UtpStream::markBound()
{
    NG_D(UtpStream);
    if (d->state != Socket::UnconnectedState) {
        return false;
    }
    d->state = Socket::BoundState;
    return true;
}

void UtpStream::close()
{
    NG_D(UtpStream);
    d->close(false);
}

void UtpStream::abort()
{
    NG_D(UtpStream);
    d->close(true);
}

bool UtpStream::listen(int backlog)
{
    NG_D(UtpStream);
    return d->listen(backlog);
}

int32_t UtpStream::peek(char *data, int32_t size)
{
    NG_D(UtpStream);
    return d->peek(data, size);
}

int32_t UtpStream::recv(char *data, int32_t size)
{
    NG_D(UtpStream);
    return d->recv(data, size, false);
}

int32_t UtpStream::recvall(char *data, int32_t size)
{
    NG_D(UtpStream);
    return d->recv(data, size, true);
}

int32_t UtpStream::send(const char *data, int32_t size)
{
    NG_D(UtpStream);
    const int32_t n = d->send(data, size, false);
    return (n == 0 && !d->isValid()) ? -1 : n;
}

int32_t UtpStream::sendall(const char *data, int32_t size)
{
    NG_D(UtpStream);
    return d->send(data, size, true);
}

std::string UtpStream::recv(int32_t size)
{
    NG_D(UtpStream);
    string bs(size, '\0');
    const int32_t bytes = d->recv(&bs[0], bs.size(), false);
    if (bytes > 0) {
        bs.resize(static_cast<size_t>(bytes));
        return bs;
    }
    return string();
}

std::string UtpStream::recvall(int32_t size)
{
    NG_D(UtpStream);
    string bs(size, '\0');
    const int32_t bytes = d->recv(&bs[0], bs.size(), true);
    if (bytes > 0) {
        bs.resize(static_cast<size_t>(bytes));
        return bs;
    }
    return string();
}

int32_t UtpStream::send(const std::string &data)
{
    NG_D(UtpStream);
    const int32_t n = d->send(data.data(), static_cast<int32_t>(data.size()), false);
    return (n == 0 && !d->isValid()) ? -1 : n;
}

int32_t UtpStream::sendall(const std::string &data)
{
    NG_D(UtpStream);
    return d->send(data.data(), static_cast<int32_t>(data.size()), true);
}

bool UtpStream::feedDatagram(const char *data, int32_t len, const DatagramPath &remote)
{
    NG_D(UtpStream);
    return d->handleDatagram(data, len, remote);
}

class UtpSocketPrivate
{
public:
    UtpSocketPrivate(shared_ptr<DatagramLink> link, shared_ptr<UdpDatagramLink> udp, shared_ptr<UtpStream> stream)
        : link(std::move(link))
        , udp(std::move(udp))
        , stream(std::move(stream))
    {
    }

    shared_ptr<DatagramLink> link;
    shared_ptr<UdpDatagramLink> udp;
    shared_ptr<UtpStream> stream;
    bool ownsFilter = false;
};

static void installUtpFilter(UtpSocket *socket, UtpSocketPrivate *d)
{
    UdpDatagramLink *udp = d->udp.get();
    if (!udp) {
        return;
    }
    udp->setFilter([socket](char *data, int32_t *len, HostAddress *addr, uint16_t *port) {
        return socket->filter(data, len, addr, port);
    });
    d->ownsFilter = true;
}

static void uninstallUtpFilter(UtpSocketPrivate *d)
{
    if (!d->ownsFilter || !d->udp) {
        return;
    }
    d->udp->setFilter({});
    d->ownsFilter = false;
}

static UtpSocketPrivate *makeUtpPrivateRaw(shared_ptr<UdpDatagramLink> udp)
{
    shared_ptr<UtpStream> stream = make_shared<UtpStream>(udp);
    return new UtpSocketPrivate(udp, udp, stream);
}

UtpSocket::UtpSocket(HostAddress::NetworkLayerProtocol protocol)
    : d_ptr(makeUtpPrivateRaw(make_shared<UdpDatagramLink>(protocol)))
{
    installUtpFilter(this, d_ptr);
}

UtpSocket::UtpSocket(intptr_t socketDescriptor)
    : d_ptr(makeUtpPrivateRaw(make_shared<UdpDatagramLink>(socketDescriptor)))
{
    installUtpFilter(this, d_ptr);
}

UtpSocket::UtpSocket(shared_ptr<Socket> rawSocket)
    : d_ptr(makeUtpPrivateRaw(make_shared<UdpDatagramLink>(rawSocket)))
{
    installUtpFilter(this, d_ptr);
}

UtpSocket::UtpSocket(shared_ptr<UtpStream> stream)
    : d_ptr(new UtpSocketPrivate(stream->link(), dynamic_pointer_cast<UdpDatagramLink>(stream->link()), stream))
{
}

UtpSocket *wrapUtpStreamAsSocket(shared_ptr<UtpStream> stream)
{
    if (!stream) {
        return nullptr;
    }
    return new UtpSocket(std::move(stream));
}

UtpSocket::~UtpSocket()
{
    uninstallUtpFilter(d_ptr);
    delete d_ptr;
}

void UtpSocket::setDelayTarget(float milliseconds)
{
    d_ptr->stream->setDelayTarget(milliseconds);
}

float UtpSocket::delayTarget() const
{
    return d_ptr->stream->delayTarget();
}

void UtpSocket::setMaxWindow(uint32_t bytes)
{
    d_ptr->stream->setMaxWindow(bytes);
}

uint32_t UtpSocket::maxWindow() const
{
    return d_ptr->stream->maxWindow();
}

void UtpSocket::setPacketSize(uint32_t bytes)
{
    d_ptr->stream->setPacketSize(bytes);
}

uint32_t UtpSocket::packetSize() const
{
    return d_ptr->stream->packetSize();
}

uint32_t UtpSocket::payloadSizeHint() const
{
    return d_ptr->stream->payloadSizeHint();
}

void UtpSocket::setReceiveBufferSize(uint32_t bytes)
{
    d_ptr->stream->setReceiveBufferSize(bytes);
}

uint32_t UtpSocket::receiveBufferSize() const
{
    return d_ptr->stream->receiveBufferSize();
}

void UtpSocket::setIdleTimeout(float seconds)
{
    d_ptr->stream->setIdleTimeout(seconds);
}

float UtpSocket::idleTimeout() const
{
    return d_ptr->stream->idleTimeout();
}

Socket::SocketError UtpSocket::error() const
{
    return d_ptr->stream->error();
}

string UtpSocket::errorString() const
{
    return d_ptr->stream->errorString();
}

bool UtpSocket::isValid() const
{
    return d_ptr->stream->isValid();
}

HostAddress UtpSocket::localAddress() const
{
    return d_ptr->udp ? d_ptr->udp->localAddress() : HostAddress();
}

uint16_t UtpSocket::localPort() const
{
    return d_ptr->udp ? d_ptr->udp->localPort() : 0;
}

HostAddress UtpSocket::peerAddress() const
{
    return UdpDatagramPath(d_ptr->stream->peerPath()).address();
}

string UtpSocket::peerName() const
{
    return UdpDatagramPath(d_ptr->stream->peerPath()).address().toString();
}

uint16_t UtpSocket::peerPort() const
{
    return UdpDatagramPath(d_ptr->stream->peerPath()).port();
}

Socket::SocketType UtpSocket::type() const
{
    return Socket::UtpSocket;
}

Socket::SocketState UtpSocket::state() const
{
    return d_ptr->stream->state();
}

HostAddress::NetworkLayerProtocol UtpSocket::protocol() const
{
    return d_ptr->udp ? d_ptr->udp->protocol() : HostAddress::UnknownNetworkLayerProtocol;
}

string UtpSocket::localAddressURI() const
{
    const HostAddress &addr = localAddress();
    string host = (addr.protocol() == HostAddress::IPv6Protocol)
            ? utils::formatMessage("[%1]", {addr.toString()})
            : addr.toString();
    return utils::formatMessage("%1:%2", {host, utils::number(localPort())});
}

string UtpSocket::peerAddressURI() const
{
    const HostAddress &addr = peerAddress();
    string host = (addr.protocol() == HostAddress::IPv6Protocol)
            ? utils::formatMessage("[%1]", {addr.toString()})
            : addr.toString();
    return utils::formatMessage("%1:%2", {host, utils::number(peerPort())});
}

UtpSocket *UtpSocket::accept()
{
    return wrapUtpStreamAsSocket(shared_ptr<UtpStream>(d_ptr->stream->accept()));
}

UtpSocket *UtpSocket::accept(const HostAddress &addr, uint16_t port)
{
    return wrapUtpStreamAsSocket(
            shared_ptr<UtpStream>(d_ptr->stream->accept(UdpDatagramPath(addr, port).toPath())));
}

UtpSocket *UtpSocket::accept(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    vector<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.push_back(t);
    } else if (!dnsCache) {
        addresses = Socket::resolve(hostName);
    } else {
        addresses = dnsCache->resolve(hostName);
    }
    const HostAddress::NetworkLayerProtocol prefer = protocol();
    for (const HostAddress &addr : addresses) {
        if (prefer == HostAddress::IPv4Protocol && addr.protocol() == HostAddress::IPv6Protocol) {
            continue;
        }
        if (prefer == HostAddress::IPv6Protocol && addr.protocol() == HostAddress::IPv4Protocol) {
            continue;
        }
        return accept(addr, port);
    }
    return nullptr;
}

bool UtpSocket::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    if (!d_ptr->udp || !d_ptr->udp->bind(address, port, mode)) {
        return false;
    }
    return d_ptr->stream->markBound();
}

bool UtpSocket::bind(uint16_t port, Socket::BindMode mode)
{
    if (!d_ptr->udp || !d_ptr->udp->bind(port, mode)) {
        return false;
    }
    return d_ptr->stream->markBound();
}

bool UtpSocket::connect(const HostAddress &addr, uint16_t port)
{
    return d_ptr->stream->connect(UdpDatagramPath(addr, port).toPath());
}

bool UtpSocket::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    vector<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.push_back(t);
    } else if (!dnsCache) {
        addresses = Socket::resolve(hostName);
    } else {
        addresses = dnsCache->resolve(hostName);
    }
    const HostAddress::NetworkLayerProtocol prefer = protocol();
    for (const HostAddress &addr : addresses) {
        if (prefer == HostAddress::IPv4Protocol && addr.protocol() == HostAddress::IPv6Protocol) {
            continue;
        }
        if (prefer == HostAddress::IPv6Protocol && addr.protocol() == HostAddress::IPv4Protocol) {
            continue;
        }
        if (connect(addr, port)) {
            return true;
        }
    }
    return false;
}

void UtpSocket::close()
{
    d_ptr->stream->close();
}

void UtpSocket::abort()
{
    d_ptr->stream->abort();
}

bool UtpSocket::listen(int backlog)
{
    return d_ptr->stream->listen(backlog);
}

bool UtpSocket::setOption(Socket::SocketOption option, int value)
{
    return d_ptr->udp ? d_ptr->udp->setOption(option, value) : false;
}

int UtpSocket::option(Socket::SocketOption option) const
{
    return d_ptr->udp ? d_ptr->udp->option(option) : -1;
}

bool UtpSocket::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->udp ? d_ptr->udp->joinMulticastGroup(groupAddress, iface) : false;
}

bool UtpSocket::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->udp ? d_ptr->udp->leaveMulticastGroup(groupAddress, iface) : false;
}

NetworkInterface UtpSocket::multicastInterface() const
{
    return d_ptr->udp ? d_ptr->udp->multicastInterface() : NetworkInterface();
}

bool UtpSocket::setMulticastInterface(const NetworkInterface &iface)
{
    return d_ptr->udp ? d_ptr->udp->setMulticastInterface(iface) : false;
}

int32_t UtpSocket::peek(char *data, int32_t size)
{
    return d_ptr->stream->peek(data, size);
}

int32_t UtpSocket::peekRaw(char *data, int32_t size)
{
    return d_ptr->udp ? d_ptr->udp->peek(data, size) : -1;
}

int32_t UtpSocket::recv(char *data, int32_t size)
{
    return d_ptr->stream->recv(data, size);
}

int32_t UtpSocket::recvall(char *data, int32_t size)
{
    return d_ptr->stream->recvall(data, size);
}

int32_t UtpSocket::send(const char *data, int32_t size)
{
    return d_ptr->stream->send(data, size);
}

int32_t UtpSocket::sendall(const char *data, int32_t size)
{
    return d_ptr->stream->sendall(data, size);
}

string UtpSocket::recv(int32_t size)
{
    return d_ptr->stream->recv(size);
}

string UtpSocket::recvall(int32_t size)
{
    return d_ptr->stream->recvall(size);
}

int32_t UtpSocket::send(const string &data)
{
    return d_ptr->stream->send(data);
}

int32_t UtpSocket::sendall(const string &data)
{
    return d_ptr->stream->sendall(data);
}

bool UtpSocket::filter(char *data, int32_t *len, HostAddress *addr, uint16_t *port)
{
    (void) data;
    (void) len;
    (void) addr;
    (void) port;
    return false;
}

int32_t UtpSocket::udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port)
{
    return d_ptr->udp ? d_ptr->udp->sendto(data, size, UdpDatagramPath(addr, port).toPath()) : -1;
}

UtpSocket *UtpSocket::createConnection(const HostAddress &host, uint16_t port, Socket::SocketError *error,
                                       int allowProtocol)
{
    return qtng::createConnection<UtpSocket>(host, port, error, allowProtocol, MakeSocketType<UtpSocket>);
}

UtpSocket *UtpSocket::createConnection(const string &hostName, uint16_t port, Socket::SocketError *error,
                                       shared_ptr<SocketDnsCache> dnsCache, int allowProtocol)
{
    return qtng::createConnection<UtpSocket>(hostName, port, error, dnsCache, allowProtocol, MakeSocketType<UtpSocket>);
}

UtpSocket *UtpSocket::createServer(const HostAddress &host, uint16_t port, int backlog)
{
    return qtng::createServer<UtpSocket>(host, port, backlog, MakeSocketType<UtpSocket>);
}

class UtpSocketLikeImpl : public SocketLike
{
public:
    UtpSocketLikeImpl(shared_ptr<UtpSocket> s)
        : s(s)
    {
    }

    virtual Socket::SocketError error() const override { return s->error(); }
    virtual string errorString() const override { return s->errorString(); }
    virtual bool isValid() const override { return s->isValid(); }
    virtual HostAddress localAddress() const override { return s->localAddress(); }
    virtual uint16_t localPort() const override { return s->localPort(); }
    virtual HostAddress peerAddress() const override { return s->peerAddress(); }
    virtual string peerName() const override { return s->peerName(); }
    virtual uint16_t peerPort() const override { return s->peerPort(); }
    virtual intptr_t fileno() const override { return -1; }
    virtual Socket::SocketType type() const override { return s->type(); }
    virtual Socket::SocketState state() const override { return s->state(); }
    virtual HostAddress::NetworkLayerProtocol protocol() const override { return s->protocol(); }
    virtual string localAddressURI() const override { return s->localAddressURI(); }
    virtual string peerAddressURI() const override { return s->peerAddressURI(); }
    virtual Socket *acceptRaw() override { return nullptr; }
    virtual shared_ptr<SocketLike> accept() override { return asSocketLike(s->accept()); }
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override
    {
        return s->bind(address, port, mode);
    }
    virtual bool bind(uint16_t port, Socket::BindMode mode) override { return s->bind(port, mode); }
    virtual bool connect(const HostAddress &addr, uint16_t port) override { return s->connect(addr, port); }
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override
    {
        return s->connect(hostName, port, dnsCache);
    }
    virtual void close() override { s->close(); }
    virtual void abort() override { s->abort(); }
    virtual bool listen(int backlog) override { return s->listen(backlog); }
    virtual bool setOption(Socket::SocketOption option, int value) override { return s->setOption(option, value); }
    virtual int option(Socket::SocketOption option) const override { return s->option(option); }
    virtual int32_t peek(char *data, int32_t size) override { return s->peek(data, size); }
    virtual int32_t peekRaw(char *data, int32_t size) override { return s->peekRaw(data, size); }
    virtual int32_t recv(char *data, int32_t size) override { return s->recv(data, size); }
    virtual int32_t recvall(char *data, int32_t size) override { return s->recvall(data, size); }
    virtual int32_t send(const char *data, int32_t size) override { return s->send(data, size); }
    virtual int32_t sendall(const char *data, int32_t size) override { return s->sendall(data, size); }
    virtual string recv(int32_t size) override { return s->recv(size); }
    virtual string recvall(int32_t size) override { return s->recvall(size); }
    virtual int32_t send(const string &data) override { return s->send(data); }
    virtual int32_t sendall(const string &data) override { return s->sendall(data); }

    shared_ptr<UtpSocket> s;
};

shared_ptr<SocketLike> asSocketLike(shared_ptr<UtpSocket> s)
{
    if (!s) {
        return shared_ptr<SocketLike>();
    }
    return make_shared<UtpSocketLikeImpl>(s);
}

shared_ptr<UtpSocket> convertSocketLikeToUtpSocket(shared_ptr<SocketLike> socket)
{
    shared_ptr<UtpSocketLikeImpl> impl = dynamic_pointer_cast<UtpSocketLikeImpl>(socket);
    if (impl) {
        return impl->s;
    }
    return shared_ptr<UtpSocket>();
}

}  // namespace qtng
