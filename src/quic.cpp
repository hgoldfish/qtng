#include "qtng/quic.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "qtng/coroutine.h"
#include "qtng/private/quic_p.h"
#include "qtng/private/quic_tls.h"
#include "qtng/random.h"
#include "qtng/socket_utils.h"
#include "qtng/udp.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

namespace {

const uint32_t kQuicVersion1 = 0x00000001;
const size_t kMaxStreamChunk = 1000;
const size_t kMaxCryptoChunk = 1000;

uint64_t quicNowUs()
{
    return static_cast<uint64_t>(chrono::duration_cast<chrono::microseconds>(
            chrono::steady_clock::now().time_since_epoch()).count());
}

// Process-wide key sealing RETRY address-validation tokens (RFC 9000 §8.1).
string quicRetryTokenKey()
{
    static const string key = randomBytes(16);
    return key;
}

string makeCid(size_t len = 8)
{
    string s = randomBytes(static_cast<int>(len));
    return s;
}

class SocketDatagramLink : public DatagramLink
{
public:
    explicit SocketDatagramLink(shared_ptr<Socket> sock)
        : m_sock(sock)
    {
    }

    int32_t recvfrom(char *data, int32_t size, DatagramPath *who) override
    {
        HostAddress addr;
        uint16_t port = 0;
        int32_t n = m_sock->recvfrom(data, size, &addr, &port);
        if (n > 0 && who) {
            string key;
            if (addr.protocol() == HostAddress::IPv6Protocol) {
                key = "[" + addr.toString() + "]:" + utils::number(port);
            } else {
                key = addr.toString() + ":" + utils::number(port);
            }
            *who = DatagramPath(key);
        }
        return n;
    }

    int32_t sendto(const char *data, int32_t size, const DatagramPath &who) override
    {
        const string &key = who.key();
        size_t pos = key.rfind(':');
        if (pos == string::npos) {
            return -1;
        }
        string host = key.substr(0, pos);
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
        HostAddress addr;
        if (!addr.setAddress(host)) {
            return -1;
        }
        bool ok = false;
        unsigned long port = static_cast<unsigned long>(utils::parseInt(key.substr(pos + 1), &ok));
        if (!ok || port > 65535) {
            return -1;
        }
        return m_sock->sendto(data, size, addr, static_cast<uint16_t>(port));
    }

    void close() override { m_sock->close(); }
    void abort() override { m_sock->abort(); }
    bool isValid() const override { return m_sock && m_sock->isValid(); }
    Socket::SocketError error() const override { return m_sock->error(); }
    string errorString() const override { return m_sock->errorString(); }

    shared_ptr<Socket> socket() const { return m_sock; }
private:
    shared_ptr<Socket> m_sock;
};

DatagramPath makePath(const HostAddress &addr, uint16_t port)
{
    string key;
    if (addr.protocol() == HostAddress::IPv6Protocol) {
        key = "[" + addr.toString() + "]:" + utils::number(port);
    } else {
        key = addr.toString() + ":" + utils::number(port);
    }
    return DatagramPath(key);
}

void pathToAddr(const DatagramPath &path, HostAddress *addr, uint16_t *port)
{
    const string &key = path.key();
    size_t pos = key.rfind(':');
    if (pos == string::npos) {
        return;
    }
    string host = key.substr(0, pos);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    addr->setAddress(host);
    bool ok = false;
    *port = static_cast<uint16_t>(utils::parseInt(key.substr(pos + 1), &ok));
}

}  // namespace

void quicBuildAckFrame(const set<uint64_t> &received, QuicFrame *ack)
{
    if (received.empty()) {
        return;
    }
    ack->type = QuicFrame::Ack;
    ack->largestAcknowledged = *received.rbegin();
    ack->ackDelay = 0;
    // First range: consecutive PNs ending at the largest.
    uint64_t low = ack->largestAcknowledged;
    while (low > 0 && received.count(low - 1)) {
        --low;
    }
    ack->firstAckRange = ack->largestAcknowledged - low;
    uint64_t prevLow = low;
    while (prevLow > 0) {
        if (received.count(prevLow - 1)) {
            break;  // contiguous with the previous range, no gap
        }
        // Gap of missing PNs below prevLow.
        uint64_t gapLow = prevLow - 1;
        while (gapLow > 0 && !received.count(gapLow - 1)) {
            --gapLow;
        }
        if (gapLow == 0) {
            break;  // everything below is missing
        }
        uint64_t rangeHigh = gapLow - 1;
        uint64_t rangeLow = rangeHigh;
        while (rangeLow > 0 && received.count(rangeLow - 1)) {
            --rangeLow;
        }
        QuicAckRange r;
        r.gap = (prevLow - 1) - rangeHigh - 1;
        r.ackRangeLength = rangeHigh - rangeLow;
        ack->ackRanges.push_back(r);
        if (ack->ackRanges.size() >= 255) {
            break;
        }
        prevLow = rangeLow;
    }
}

QuicConfiguration::QuicConfiguration()
    : m_idleTimeout(30.0f)
    , m_verifyPeer(false)
    , m_maxData(1024 * 1024)
    , m_maxStreamData(256 * 1024)
    , m_requireAddressValidation(false)
{
    m_alpn.push_back("hq-interop");
}

void QuicConfiguration::setAlpnProtocols(const vector<string> &protocols)
{
    m_alpn = protocols;
}
vector<string> QuicConfiguration::alpnProtocols() const
{
    return m_alpn;
}
void QuicConfiguration::setIdleTimeout(float seconds)
{
    m_idleTimeout = seconds;
}
float QuicConfiguration::idleTimeout() const
{
    return m_idleTimeout;
}
void QuicConfiguration::setVerifyPeer(bool verify)
{
    m_verifyPeer = verify;
}
bool QuicConfiguration::verifyPeer() const
{
    return m_verifyPeer;
}
void QuicConfiguration::setPrivateKey(const PrivateKey &key)
{
    m_privateKey = key;
}
PrivateKey QuicConfiguration::privateKey() const
{
    return m_privateKey;
}
void QuicConfiguration::setLocalCertificate(const Certificate &cert)
{
    m_localCertificate = cert;
}
Certificate QuicConfiguration::localCertificate() const
{
    return m_localCertificate;
}
void QuicConfiguration::setMaxData(uint64_t bytes)
{
    m_maxData = bytes;
}
uint64_t QuicConfiguration::maxData() const
{
    return m_maxData;
}
void QuicConfiguration::setMaxStreamData(uint64_t bytes)
{
    m_maxStreamData = bytes;
}
uint64_t QuicConfiguration::maxStreamData() const
{
    return m_maxStreamData;
}
void QuicConfiguration::setCongestionController(shared_ptr<QuicCongestionControl> cc)
{
    m_congestionController = std::move(cc);
}
shared_ptr<QuicCongestionControl> QuicConfiguration::congestionController() const
{
    return m_congestionController;
}
void QuicConfiguration::setRequireAddressValidation(bool require)
{
    m_requireAddressValidation = require;
}
bool QuicConfiguration::requireAddressValidation() const
{
    return m_requireAddressValidation;
}

class QuicStreamPrivate
{
public:
    QuicStreamPrivate(QuicConnectionPrivate *conn, uint64_t id)
        : connection(conn)
        , streamId(id)
        , recvOffset(0)
        , sendOffset(0)
        , finSent(false)
        , finReceived(false)
        , reset(false)
        , error(QuicConnection::NoError)
    {
    }

    QuicConnectionPrivate *connection;
    uint64_t streamId;
    string recvBuf;
    uint64_t recvOffset;
    uint64_t sendOffset;
    bool finSent;
    bool finReceived;
    bool reset;
    QuicConnection::Error error;
    string errorString;
    Event dataReady;
    Event sendReady;
    // Out-of-order reassembly buffer: offset -> fragment.
    map<uint64_t, string> recvFragments;
    // FIN boundary remembered when FIN arrives before the data completes.
    bool hasFinBoundary = false;
    uint64_t finBoundary = 0;
    // 0-RTT replay buffer: data sent before the handshake completed, replayed
    // with 1-RTT keys if the server rejects early data.
    uint64_t zeroRttStartOffset = 0;
    bool sentZeroRtt = false;
    string zeroRttData;
};

class QuicConnectionPrivate
{
public:
    QuicConnectionPrivate(QuicConnection *q, shared_ptr<DatagramLink> link)
        : q_ptr(q)
        , link(link)
        , state(QuicConnection::UnconnectedState)
        , error(QuicConnection::NoError)
        , isClient(true)
        , nextClientBidi(0)
        , nextServerBidi(1)
        , nextPnInitial(0)
        , nextPnHandshake(0)
        , nextPnApp(0)
        , largestRecvInitial(0)
        , largestRecvHandshake(0)
        , largestRecvApp(0)
        , hasRecvInitial(false)
        , hasRecvHandshake(false)
        , hasRecvApp(false)
        , closed(false)
        , readerStarted(false)
        , handshakeDoneEvent(new Event())
        , cc(make_shared<QuicRenoCongestionControl>())
    {
    }

    struct QuicOutgoingPacket {
        QuicPacketNumberSpace space = QuicPnApplication;
        uint64_t pn = 0;
        string raw;
        bool ackEliciting = true;
    };

    QuicConnection *q_ptr;
    shared_ptr<DatagramLink> link;
    shared_ptr<Socket> udpSocket;
    QuicConfiguration config;
    QuicConnection::State state;
    QuicConnection::Error error;
    string errorString;
    bool isClient;
    DatagramPath peerPath;
    HostAddress localAddr;
    uint16_t localPort = 0;
    HostAddress peerAddr;
    uint16_t peerPort = 0;
    string peerName;

    QuicConnectionId localCid;
    QuicConnectionId remoteCid;
    QuicConnectionId odcid;

    unique_ptr<QuicTlsHandshake> tls;
    QuicTrafficKeys clientInitialKeys;
    QuicTrafficKeys serverInitialKeys;
    QuicTrafficKeys clientHsKeys;
    QuicTrafficKeys serverHsKeys;
    QuicTrafficKeys clientAppKeys;
    QuicTrafficKeys serverAppKeys;
    QuicTrafficKeys clientEarlyKeys;
    bool hasHsKeys = false;
    bool hasAppKeys = false;
    bool hasEarlyKeys = false;

    map<uint64_t, shared_ptr<QuicStream>> streams;
    Queue<shared_ptr<QuicConnection>> incomingConns;
    uint64_t nextClientBidi;
    uint64_t nextServerBidi;
    uint64_t nextClientUni = 2;
    uint64_t nextServerUni = 3;

    uint64_t nextPnInitial;
    uint64_t nextPnHandshake;
    uint64_t nextPnApp;
    uint64_t largestRecvInitial;
    uint64_t largestRecvHandshake;
    uint64_t largestRecvApp;
    bool hasRecvInitial;
    bool hasRecvHandshake;
    bool hasRecvApp;

    string cryptoSendInitial;
    string cryptoSendHandshake;
    string cryptoSendApp;
    string cryptoRecvInitial;
    string cryptoRecvHandshake;
    string cryptoRecvApp;
    uint64_t cryptoSendOffInitial = 0;
    uint64_t cryptoSendOffHandshake = 0;
    uint64_t cryptoSendOffApp = 0;
    uint64_t cryptoRecvOffInitial = 0;
    uint64_t cryptoRecvOffHandshake = 0;
    uint64_t cryptoRecvOffApp = 0;
    map<uint64_t, string> cryptoRecvFragments[2];

    QuicLossRecovery recovery;
    shared_ptr<QuicCongestionControl> cc;
    bool closed;
    bool readerStarted;
    shared_ptr<Event> handshakeDoneEvent;
    shared_ptr<Coroutine> reader;
    shared_ptr<Coroutine> ptoTimer;
    shared_ptr<Coroutine> sendLoopCoroutine;
    Lock sendLock;

    // Send queue (congestion-controlled packets).
    Queue<QuicOutgoingPacket> sendQueue;

    // ACK scheduling state.
    set<uint64_t> receivedPn[3];
    uint32_t ackElicitingCount[3] = {0, 0, 0};
    uint64_t ackDueUs[3] = {0, 0, 0};
    shared_ptr<Coroutine> ackLoopCoroutine;

    // Send-side flow control (limits advertised by the peer).
    uint64_t maxDataRemote = 0;
    uint64_t dataSent = 0;
    map<uint64_t, uint64_t> maxStreamDataRemote;
    uint64_t streamInitialLimitRemote = 0;
    uint64_t maxStreamsBidiRemote = 0;
    uint64_t maxStreamsUniRemote = 0;
    uint64_t bidiStreamsOpened = 0;
    uint64_t uniStreamsOpened = 0;
    uint64_t lastBlockedSentUs = 0;

    // Receive-side flow control (limits we advertise to the peer).
    uint64_t maxDataLocal = 0;
    uint64_t maxStreamDataLocal = 0;
    uint64_t recvDataTotal = 0;
    uint64_t maxStreamsLocalBidi = 100;
    uint64_t maxStreamsLocalUni = 100;
    set<uint64_t> acceptedStreams;

    // Protocol housekeeping.
    bool handshakeConfirmed = false;
    vector<string> newTokens;
    uint64_t lastActivityUs = 0;
    uint64_t transportErrorCode = QuicErrNoError;

    // Session resumption ticket to present on the next handshake (0-RTT).
    QuicSessionTicket pendingSessionTicket;

    // CID management (RFC 9000 §5.1, §19.15-19.16).
    uint64_t nextCidSeq = 0;
    map<uint64_t, QuicConnectionId> peerCids;
    map<uint64_t, string> peerCidTokens;
    bool issuedPeerCids = false;
    map<string, string> localCidTokens;  // our CID -> stateless reset token

    // Path validation (RFC 9000 §8.2): challenge data -> candidate path key.
    map<string, string> pendingPathChallenges;
    uint64_t pathValidationDeadlineUs = 0;

    // Key update (RFC 9001 §6).
    string sendAppSecret;
    string recvAppSecret;
    QuicTrafficKeys sendAppKeys;
    QuicTrafficKeys recvAppKeys;
    bool sendKeyPhase = false;
    bool recvKeyPhase = false;
    string nextRecvSecret;
    QuicTrafficKeys nextRecvAppKeys;
    bool hasNextRecvAppKeys = false;
    uint32_t appPacketsSent = 0;

    void maybeUpdateSendKeys();
    void deriveNextRecvKeys();

    // Address validation via RETRY (RFC 9000 §8.1).
    bool requireAddressValidation = false;
    string retryToken;  // client: token received in the RETRY packet
    bool sentRetry = false;
    QuicConnectionId retryScid;
    QuicConnectionId clientInitialScid;  // server: SCID of the client's first Initial

    void issueNewConnectionIds();
    void onPossibleMigration(const DatagramPath &who);
    void sendRetryPacket();
    bool validateRetryToken(const string &token, const DatagramPath &peer);
    void handleRetryPacket(const char *data, size_t size);
    void sendStatelessReset(const string &cid);
    bool isStatelessReset(const char *data, size_t size) const;

    bool setupTls(bool client);
    bool sendPacket(QuicPacketNumberSpace space, const string &payload, bool ackEliciting = true,
                    bool cwndControlled = true);
    bool sendZeroRttPacket(const string &payload, bool ackEliciting = true);
    bool sendNow(QuicPacketNumberSpace space, uint64_t pn, const string &packet, bool ackEliciting);
    void sendLoop();
    bool flushCrypto();
    void maybeSendAck(QuicPacketNumberSpace space);
    void buildAckFrame(QuicPacketNumberSpace space, QuicFrame *ack);
    void scheduleAck(QuicPacketNumberSpace space);
    void ackLoop();
    void handlePayload(QuicPacketNumberSpace space, const string &payload, uint64_t pn);
    void handleFrame(QuicPacketNumberSpace space, const QuicFrame &frame);
    void onCryptoFrame(QuicPacketNumberSpace space, const QuicFrame &frame);
    void onStreamFrame(QuicPacketNumberSpace space, const QuicFrame &frame);
    void driveTls();
    void startReader();
    void readerLoop();
    void ptoLoop();
    void setError(QuicConnection::Error e, const string &msg, uint64_t transportError = QuicErrNoError);
    QuicTrafficKeys *keysForSend(QuicPacketNumberSpace space);
    QuicTrafficKeys *keysForRecv(QuicPacketNumberSpace space);
    uint64_t largestReceivedPn(QuicPacketNumberSpace space) const;
    bool hasReceivedPn(QuicPacketNumberSpace space) const;
    shared_ptr<QuicStream> getOrCreateStream(uint64_t id);
    void replayZeroRtt(QuicStreamPrivate *sd);
    string buildAddressValidationToken();

    uint64_t streamSendLimit(uint64_t streamId) const;
    size_t waitForSendWindow(QuicStreamPrivate *sd);
    void sendBlockedIfNeeded(uint64_t streamId, uint64_t offset, uint64_t limit);
    void waitSendDrained();
    void maybeIncreaseMaxData();
    void maybeIncreaseMaxStreamData(uint64_t streamId);
};

bool QuicConnectionPrivate::setupTls(bool client)
{
    isClient = client;
    QuicTransportParams params;
    params.maxIdleTimeoutMs = static_cast<uint64_t>(config.idleTimeout() * 1000);
    params.initialMaxData = config.maxData();
    params.initialMaxStreamDataBidiLocal = config.maxStreamData();
    params.initialMaxStreamDataBidiRemote = config.maxStreamData();
    params.initialMaxStreamDataUni = config.maxStreamData();
    params.initialSourceConnectionId = localCid.bytes;
    if (!client) {
        params.originalDestinationConnectionId = odcid.bytes;
    }
    tls = make_unique<QuicTlsHandshake>(client ? QuicTlsHandshake::Client : QuicTlsHandshake::Server, params);
    tls->setAlpn(config.alpnProtocols());
    tls->setVerifyPeer(config.verifyPeer());
    if (!client) {
        tls->setCredentials(config.privateKey(), config.localCertificate());
    }
    if (client && !peerName.empty()) {
        tls->setServerName(peerName);
    }
    if (client && pendingSessionTicket.isValid()) {
        tls->setSessionTicket(pendingSessionTicket.ticket, pendingSessionTicket.ticketNonce,
                              pendingSessionTicket.resumptionSecret);
    }
    maxDataLocal = config.maxData();
    maxStreamDataLocal = config.maxStreamData();
    return true;
}

QuicTrafficKeys *QuicConnectionPrivate::keysForSend(QuicPacketNumberSpace space)
{
    if (space == QuicPnInitial) {
        return isClient ? &clientInitialKeys : &serverInitialKeys;
    }
    if (space == QuicPnHandshake) {
        return isClient ? &clientHsKeys : &serverHsKeys;
    }
    return &sendAppKeys;
}

QuicTrafficKeys *QuicConnectionPrivate::keysForRecv(QuicPacketNumberSpace space)
{
    if (space == QuicPnInitial) {
        return isClient ? &serverInitialKeys : &clientInitialKeys;
    }
    if (space == QuicPnHandshake) {
        return isClient ? &serverHsKeys : &clientHsKeys;
    }
    return &recvAppKeys;
}

uint64_t QuicConnectionPrivate::largestReceivedPn(QuicPacketNumberSpace space) const
{
    if (space == QuicPnInitial) {
        return largestRecvInitial;
    }
    if (space == QuicPnHandshake) {
        return largestRecvHandshake;
    }
    return largestRecvApp;
}

bool QuicConnectionPrivate::hasReceivedPn(QuicPacketNumberSpace space) const
{
    if (space == QuicPnInitial) {
        return hasRecvInitial;
    }
    if (space == QuicPnHandshake) {
        return hasRecvHandshake;
    }
    return hasRecvApp;
}

bool QuicConnectionPrivate::sendPacket(QuicPacketNumberSpace space, const string &payload, bool ackEliciting,
                                       bool cwndControlled)
{
    QuicTrafficKeys *keys = keysForSend(space);
    if (!keys || !keys->valid()) {
        return false;
    }
    uint64_t *pnCounter = &nextPnApp;
    QuicLongPacketType longType = QuicLongHandshake;
    bool useLong = true;
    if (space == QuicPnInitial) {
        pnCounter = &nextPnInitial;
        longType = QuicLongInitial;
    } else if (space == QuicPnHandshake) {
        pnCounter = &nextPnHandshake;
        longType = QuicLongHandshake;
    } else {
        useLong = false;
    }
    const uint64_t pn = (*pnCounter)++;
    const int pnLen = 2;
    string header;
    if (useLong) {
        header = quicBuildLongHeader(longType, kQuicVersion1, remoteCid, localCid,
                                     (space == QuicPnInitial) ? retryToken : string(), pn, pnLen,
                                     payload.size() + 16);
    } else {
        header = quicBuildShortHeader(remoteCid, pn, pnLen, sendKeyPhase);
    }
    string packet;
    if (!quicProtectPacket(*keys, header, payload, pn, pnLen, &packet)) {
        return false;
    }
    if (cwndControlled) {
        QuicOutgoingPacket out;
        out.space = space;
        out.pn = pn;
        out.raw = packet;
        out.ackEliciting = ackEliciting;
        sendQueue.put(std::move(out));
        return true;
    }
    return sendNow(space, pn, packet, ackEliciting);
}

bool QuicConnectionPrivate::sendNow(QuicPacketNumberSpace space, uint64_t pn, const string &packet,
                                    bool ackEliciting)
{
    if (link->sendto(packet.data(), static_cast<int32_t>(packet.size()), peerPath)
        != static_cast<int32_t>(packet.size())) {
        setError(QuicConnection::SocketError, "sendto failed");
        return false;
    }
    if (space == QuicPnApplication && hasAppKeys && ++appPacketsSent >= 10000) {
        // Automatically roll the key phase after many packets (RFC 9001 §6).
        maybeUpdateSendKeys();
    }
    QuicSentPacket sent;
    sent.pn = pn;
    sent.space = space;
    sent.raw = packet;
    sent.ackEliciting = ackEliciting;
    sent.inFlight = true;
    sent.timeSentUs = quicNowUs();
    recovery.onPacketSent(sent);
    return true;
}

void QuicConnectionPrivate::sendLoop()
{
    while (!closed) {
        if (sendQueue.isEmpty()) {
            Coroutine::sleep(0.005f);
            continue;
        }
        // Drain as many packets as the congestion window allows in one go; the
        // queue is only re-polled when the window closes. Sleeping between every
        // packet would cap throughput at ~200 packets/s regardless of RTT.
        bool drained = false;
        while (!sendQueue.isEmpty()) {
            if (!cc->canSend(recovery.bytesInFlight())) {
                drained = true;
                break;  // window full: wait for an ACK to open it again
            }
            QuicOutgoingPacket out = sendQueue.get();
            if (out.raw.empty()) {
                continue;
            }
            if (!sendNow(out.space, out.pn, out.raw, out.ackEliciting)) {
                return;
            }
        }
        if (drained) {
            // Window closed: poll briefly so an arriving ACK re-opens it quickly.
            Coroutine::sleep(0.001f);
        }
    }
}

bool QuicConnectionPrivate::sendZeroRttPacket(const string &payload, bool ackEliciting)
{
    if (!clientEarlyKeys.valid()) {
        return false;
    }
    // 0-RTT and 1-RTT share the application-data packet number space (RFC 9001 §5.7).
    const uint64_t pn = nextPnApp++;
    const int pnLen = 2;
    const string header = quicBuildLongHeader(QuicLongZeroRtt, kQuicVersion1, remoteCid, localCid, string(), pn,
                                              pnLen, payload.size() + 16);
    string packet;
    if (!quicProtectPacket(clientEarlyKeys, header, payload, pn, pnLen, &packet)) {
        return false;
    }
    QuicOutgoingPacket out;
    out.space = QuicPnApplication;
    out.pn = pn;
    out.raw = packet;
    out.ackEliciting = ackEliciting;
    sendQueue.put(std::move(out));
    return true;
}

void QuicConnectionPrivate::issueNewConnectionIds()
{
    if (!hasAppKeys || issuedPeerCids) {
        return;
    }
    issuedPeerCids = true;
    // Publish a couple of extra connection IDs so the peer can migrate without
    // reusing the original one (RFC 9000 §5.1).
    for (int i = 0; i < 2; ++i) {
        QuicFrame f;
        f.type = QuicFrame::NewConnectionId;
        f.sequenceNumber = nextCidSeq++;
        f.retirePriorTo = 0;
        f.connectionId.bytes = makeCid(8);
        f.statelessResetToken = randomBytes(16);
        localCidTokens[f.connectionId.bytes] = f.statelessResetToken;
        string payload;
        quicEncodeFrame(f, &payload);
        sendPacket(QuicPnApplication, payload, true, false);
    }
}

string quicBuildStatelessReset(const string &token)
{
    if (token.size() != 16) {
        return string();
    }
    // RFC 9000 §10.3: fixed bits 01, unpredictable bits, then the 16-byte token.
    const string rnd = randomBytes(21);
    string out;
    out.push_back(static_cast<char>(0x40 | (static_cast<unsigned char>(rnd[0]) & 0x3f)));
    out.append(rnd, 1, 4);
    out.append(rnd, 5, 16);
    out.append(token);
    return out;
}

bool quicIsStatelessReset(const char *data, size_t size, const vector<string> &tokens)
{
    if (!data || size < 21) {
        return false;
    }
    for (const string &t : tokens) {
        if (t.size() == 16 && memcmp(data + size - 16, t.data(), 16) == 0) {
            return true;
        }
    }
    return false;
}

void QuicConnectionPrivate::sendStatelessReset(const string &cid)
{
    // RFC 9000 §10.3: short header, unpredictable bits, then the token.
    auto it = localCidTokens.find(cid);
    if (it == localCidTokens.end()) {
        return;
    }
    const string packet = quicBuildStatelessReset(it->second);
    link->sendto(packet.data(), static_cast<int32_t>(packet.size()), peerPath);
}

bool QuicConnectionPrivate::isStatelessReset(const char *data, size_t size) const
{
    vector<string> tokens;
    for (const auto &kv : peerCidTokens) {
        tokens.push_back(kv.second);
    }
    return quicIsStatelessReset(data, size, tokens);
}

void QuicConnectionPrivate::onPossibleMigration(const DatagramPath &who)
{
    if (!hasAppKeys || !pendingPathChallenges.empty()) {
        return;
    }
    // Challenge the new path before switching (RFC 9000 §8.2).
    QuicFrame f;
    f.type = QuicFrame::PathChallenge;
    f.pathData = randomBytes(8);
    string payload;
    quicEncodeFrame(f, &payload);
    if (sendPacket(QuicPnApplication, payload, true, false)) {
        pendingPathChallenges[f.pathData] = who.key();
        pathValidationDeadlineUs = quicNowUs() + 5 * 1000 * 1000;
    }
}

string QuicConnectionPrivate::buildAddressValidationToken()
{
    const string plain = peerPath.key() + "|" + std::to_string(quicNowUs());
    Aead aead(Aead::Aes128Gcm);
    if (!aead.setKey(quicRetryTokenKey())) {
        return string();
    }
    const string nonce = randomBytes(12);
    string sealed;
    if (!aead.seal(nonce, string(), plain, &sealed)) {
        return string();
    }
    return nonce + sealed;
}

void QuicConnectionPrivate::sendRetryPacket()
{
    if (sentRetry) {
        return;
    }
    const string token = buildAddressValidationToken();
    if (token.empty()) {
        return;
    }
    retryScid.bytes = makeCid(8);
    // RFC 9000 §17.2.5: the RETRY DCID must be the client's Initial SCID so the
    // client can validate it against its own source CID.
    const QuicConnectionId retryDcid = clientInitialScid.empty() ? odcid : clientInitialScid;
    const string retry = quicBuildRetryPacket(kQuicVersion1, retryDcid, retryScid, token, odcid);
    if (retry.empty()) {
        return;
    }
    link->sendto(retry.data(), static_cast<int32_t>(retry.size()), peerPath);
    sentRetry = true;
}

bool QuicConnectionPrivate::validateRetryToken(const string &token, const DatagramPath &peer)
{
    if (token.size() < 12 + 16) {
        return false;
    }
    Aead aead(Aead::Aes128Gcm);
    if (!aead.setKey(quicRetryTokenKey())) {
        return false;
    }
    string out;
    if (!aead.open(token.substr(0, 12), string(), token.substr(12), &out)) {
        return false;
    }
    const size_t pos = out.rfind('|');
    if (pos == string::npos) {
        return false;
    }
    if (out.substr(0, pos) != peer.key()) {
        return false;
    }
    bool ok = false;
    const uint64_t ts = static_cast<uint64_t>(utils::parseInt(out.substr(pos + 1), &ok));
    if (!ok) {
        return false;
    }
    return quicNowUs() - ts < 30 * 1000 * 1000;
}

void QuicConnectionPrivate::handleRetryPacket(const char *data, size_t size)
{
    uint32_t version = 0;
    QuicConnectionId dcid, scid;
    string token;
    if (!quicParseRetryPacket(data, size, odcid, &version, &dcid, &scid, &token)) {
        setError(QuicConnection::ProtocolError, "bad retry packet", QuicErrInvalidToken);
        return;
    }
    if (version != 0 && version != kQuicVersion1) {
        setError(QuicConnection::UnsupportedVersion, "unsupported version");
        return;
    }
    // RFC 9000 §17.2.5: the RETRY DCID must echo the client's Initial SCID.
    if (dcid.bytes != localCid.bytes) {
        setError(QuicConnection::ProtocolError, "retry dcid mismatch", QuicErrInvalidToken);
        return;
    }
    remoteCid = scid;
    retryToken = token;
    // Re-send the ClientHello in a fresh Initial with the new DCID and token.
    cryptoSendOffInitial = 0;
    cryptoSendOffHandshake = 0;
    nextPnInitial = 0;
    nextPnHandshake = 0;
    flushCrypto();
}

void QuicConnectionPrivate::maybeUpdateSendKeys()
{
    if (!hasAppKeys) {
        return;
    }
    // RFC 9001 §6: new secret = HKDF-Expand-Label(secret, "quic ku", "", 32).
    sendAppSecret = hkdfExpandLabel(MessageDigest::Sha256, sendAppSecret, "quic ku", string(), 32);
    sendAppKeys = quicDeriveTrafficKeys(sendAppSecret);
    sendKeyPhase = !sendKeyPhase;
    appPacketsSent = 0;
}

void QuicConnectionPrivate::deriveNextRecvKeys()
{
    if (!hasAppKeys || hasNextRecvAppKeys) {
        return;
    }
    nextRecvSecret = hkdfExpandLabel(MessageDigest::Sha256, recvAppSecret, "quic ku", string(), 32);
    nextRecvAppKeys = quicDeriveTrafficKeys(nextRecvSecret);
    hasNextRecvAppKeys = true;
}

void QuicConnectionPrivate::replayZeroRtt(QuicStreamPrivate *sd)
{
    sd->sendOffset = sd->zeroRttStartOffset;
    size_t off = 0;
    while (off < sd->zeroRttData.size()) {
        const size_t chunk = min<size_t>(sd->zeroRttData.size() - off, kMaxStreamChunk);
        QuicFrame f;
        f.type = QuicFrame::Stream;
        f.streamId = sd->streamId;
        f.offset = sd->sendOffset;
        f.data = sd->zeroRttData.substr(off, chunk);
        f.hasLength = true;
        f.fin = false;
        string payload;
        quicEncodeFrame(f, &payload);
        sendPacket(QuicPnApplication, payload, true, true);
        sd->sendOffset += chunk;
        off += chunk;
    }
    sd->zeroRttData.clear();
    if (sd->finSent) {
        QuicFrame f;
        f.type = QuicFrame::Stream;
        f.streamId = sd->streamId;
        f.offset = sd->sendOffset;
        f.hasLength = true;
        f.fin = true;
        string payload;
        quicEncodeFrame(f, &payload);
        sendPacket(QuicPnApplication, payload, true, true);
    }
}

void QuicConnectionPrivate::buildAckFrame(QuicPacketNumberSpace space, QuicFrame *ack)
{
    quicBuildAckFrame(receivedPn[space], ack);
}

void QuicConnectionPrivate::maybeSendAck(QuicPacketNumberSpace space)
{
    if (closed || !hasReceivedPn(space)) {
        return;
    }
    QuicFrame ack;
    buildAckFrame(space, &ack);
    string payload;
    if (quicEncodeFrame(ack, &payload)) {
        sendPacket(space, payload, false, false);
    }
}

void QuicConnectionPrivate::scheduleAck(QuicPacketNumberSpace space)
{
    if (closed) {
        return;
    }
    if (ackDueUs[space] != 0) {
        // RFC 9000 §13.2.1: ACK immediately after every second ack-eliciting packet.
        ++ackElicitingCount[space];
        if (ackElicitingCount[space] >= 2) {
            ackDueUs[space] = 0;
            ackElicitingCount[space] = 0;
            maybeSendAck(space);
        }
        return;
    }
    ackElicitingCount[space] = 1;
    ackDueUs[space] = quicNowUs() + 25 * 1000;
}

void QuicConnectionPrivate::ackLoop()
{
    while (!closed) {
        Coroutine::sleep(0.01f);
        const uint64_t now = quicNowUs();
        for (QuicPacketNumberSpace space : {QuicPnInitial, QuicPnHandshake, QuicPnApplication}) {
            if (ackDueUs[space] != 0 && now >= ackDueUs[space]) {
                ackDueUs[space] = 0;
                ackElicitingCount[space] = 0;
                maybeSendAck(space);
            }
        }
    }
}

bool QuicConnectionPrivate::flushCrypto()
{
    auto sendCrypto = [&](QuicPacketNumberSpace space, string *buf, uint64_t *off) {
        while (!buf->empty()) {
            const size_t chunk = min<size_t>(buf->size(), kMaxCryptoChunk);
            QuicFrame f;
            f.type = QuicFrame::Crypto;
            f.offset = *off;
            f.data = buf->substr(0, chunk);
            string payload;
            quicEncodeFrame(f, &payload);
            // RFC 9000: UDP datagrams containing client Initial must be >= 1200 bytes.
            if (space == QuicPnInitial && isClient) {
                const size_t hdrBudget = 28;
                const size_t minPlain = 1200 - hdrBudget - 16;
                while (payload.size() < minPlain) {
                    payload.push_back('\0');
                }
            }
            if (!sendPacket(space, payload, true, true)) {
                return false;
            }
            *off += chunk;
            buf->erase(0, chunk);
        }
        return true;
    };
    if (!sendCrypto(QuicPnInitial, &cryptoSendInitial, &cryptoSendOffInitial)) {
        return false;
    }
    if (hasHsKeys && !sendCrypto(QuicPnHandshake, &cryptoSendHandshake, &cryptoSendOffHandshake)) {
        return false;
    }
    if (hasAppKeys && !sendCrypto(QuicPnApplication, &cryptoSendApp, &cryptoSendOffApp)) {
        return false;
    }
    return true;
}

shared_ptr<QuicStream> QuicConnectionPrivate::getOrCreateStream(uint64_t id)
{
    auto it = streams.find(id);
    if (it != streams.end()) {
        return it->second;
    }
    // Enforce the peer stream count limit (RFC 9000 §4.6).
    const bool peerInitiated = isClient ? ((id & 1) == 1) : ((id & 1) == 0);
    if (peerInitiated) {
        const uint64_t index = id >> 2;
        const uint64_t maxStreams = (id & 2) ? maxStreamsLocalUni : maxStreamsLocalBidi;
        if (index >= maxStreams) {
            setError(QuicConnection::ProtocolError, "flow control: too many peer streams", QuicErrStreamLimitError);
            return shared_ptr<QuicStream>();
        }
    }
    QuicStreamPrivate *sd = new QuicStreamPrivate(this, id);
    shared_ptr<QuicStream> s(new QuicStream(sd));
    streams[id] = s;
    return s;
}

void QuicConnectionPrivate::onCryptoFrame(QuicPacketNumberSpace space, const QuicFrame &frame)
{
    if (space == QuicPnApplication) {
        // App-space CRYPTO (NewSessionTicket) arrives in order in practice.
        if (frame.offset + frame.data.size() <= cryptoRecvOffApp) {
            return;
        }
        if (frame.offset > cryptoRecvOffApp) {
            return;  // out-of-order app CRYPTO: ignore for now
        }
        const size_t skip = static_cast<size_t>(cryptoRecvOffApp - frame.offset);
        cryptoRecvApp.append(frame.data.substr(skip));
        cryptoRecvOffApp += frame.data.size() - skip;
        driveTls();
        return;
    }
    const int idx = (space == QuicPnInitial) ? 0 : 1;
    uint64_t &off = (space == QuicPnInitial) ? cryptoRecvOffInitial : cryptoRecvOffHandshake;
    string *buf = (space == QuicPnInitial) ? &cryptoRecvInitial : &cryptoRecvHandshake;
    if (frame.data.empty()) {
        return;
    }
    if (frame.offset + frame.data.size() <= off) {
        return;  // fully duplicate
    }
    cryptoRecvFragments[idx][frame.offset] = frame.data;
    while (cryptoRecvFragments[idx].count(off)) {
        auto it = cryptoRecvFragments[idx].find(off);
        buf->append(it->second);
        off += it->second.size();
        cryptoRecvFragments[idx].erase(it);
    }
    driveTls();
}

void QuicConnectionPrivate::driveTls()
{
    if (!tls) {
        return;
    }
    string err;
    if (!cryptoRecvInitial.empty()) {
        string chunk = cryptoRecvInitial;
        cryptoRecvInitial.clear();
        if (!tls->feedCryptoData(chunk, &err)) {
            setError(QuicConnection::HandshakeError, err);
            return;
        }
    }
    if (!cryptoRecvHandshake.empty()) {
        string chunk = cryptoRecvHandshake;
        cryptoRecvHandshake.clear();
        if (!tls->feedCryptoData(chunk, &err)) {
            setError(QuicConnection::HandshakeError, err);
            return;
        }
    }
    if (!cryptoRecvApp.empty()) {
        string chunk = cryptoRecvApp;
        cryptoRecvApp.clear();
        if (!tls->feedCryptoData(chunk, &err)) {
            setError(QuicConnection::HandshakeError, err);
            return;
        }
    }

    string out = tls->takeCryptoToSend();
    if (!out.empty()) {
        size_t off = 0;
        while (off + 4 <= out.size()) {
            const uint8_t type = static_cast<uint8_t>(out[off]);
            const uint32_t len = (static_cast<uint32_t>(static_cast<unsigned char>(out[off + 1])) << 16)
                    | (static_cast<uint32_t>(static_cast<unsigned char>(out[off + 2])) << 8)
                    | static_cast<uint32_t>(static_cast<unsigned char>(out[off + 3]));
            if (off + 4 + len > out.size()) {
                break;
            }
            string msg = out.substr(off, 4 + len);
            off += 4 + len;
            if (type == 2 || type == 1) {
                cryptoSendInitial.append(msg);
            } else if (type == 4) {
                cryptoSendApp.append(msg);  // NewSessionTicket -> 1-RTT
            } else {
                cryptoSendHandshake.append(msg);
            }
        }
    }

    if (!hasEarlyKeys && tls->hasEarlyTrafficSecret()) {
        clientEarlyKeys = quicDeriveTrafficKeys(tls->clientEarlyTrafficSecret());
        hasEarlyKeys = true;
    }

    if (tls->secrets().handshakeReady && !hasHsKeys) {
        clientHsKeys = quicDeriveTrafficKeys(tls->secrets().clientHandshakeSecret);
        serverHsKeys = quicDeriveTrafficKeys(tls->secrets().serverHandshakeSecret);
        hasHsKeys = true;
    }
    if (tls->secrets().appReady && !hasAppKeys) {
        clientAppKeys = quicDeriveTrafficKeys(tls->secrets().clientAppSecret);
        serverAppKeys = quicDeriveTrafficKeys(tls->secrets().serverAppSecret);
        hasAppKeys = true;
        sendAppSecret = isClient ? tls->secrets().clientAppSecret : tls->secrets().serverAppSecret;
        recvAppSecret = isClient ? tls->secrets().serverAppSecret : tls->secrets().clientAppSecret;
        sendAppKeys = isClient ? clientAppKeys : serverAppKeys;
        recvAppKeys = isClient ? serverAppKeys : clientAppKeys;
        deriveNextRecvKeys();
        state = QuicConnection::ConnectedState;
        const QuicTransportParams &pp = tls->peerParams();
        maxDataRemote = pp.initialMaxData;
        streamInitialLimitRemote = isClient ? pp.initialMaxStreamDataBidiRemote : pp.initialMaxStreamDataBidiLocal;
        maxStreamsBidiRemote = pp.initialMaxStreamsBidi;
        maxStreamsUniRemote = pp.initialMaxStreamsUni;
        // If the server rejected our 0-RTT data, replay it with 1-RTT keys.
        if (isClient && hasEarlyKeys && !tls->earlyDataAccepted()) {
            for (auto &kv : streams) {
                QuicStreamPrivate *sd = kv.second->d_func();
                if (sd->sentZeroRtt && sd->sendOffset > sd->zeroRttStartOffset) {
                    replayZeroRtt(sd);
                }
            }
            hasEarlyKeys = false;
        }
        q_ptr->handshakeDone()->set();
        if (!isClient) {
            issueNewConnectionIds();
            // HANDSHAKE_DONE (RFC 9000 §17.2.3) lets the client know the handshake
            // is fully complete.
            QuicFrame hd;
            hd.type = QuicFrame::HandshakeDone;
            string hdPayload;
            quicEncodeFrame(hd, &hdPayload);
            sendPacket(QuicPnApplication, hdPayload, true, false);
            // NEW_TOKEN carries an address-validation token for future connections.
            const string tkn = buildAddressValidationToken();
            if (!tkn.empty()) {
                QuicFrame nt;
                nt.type = QuicFrame::NewToken;
                nt.data = tkn;
                string ntPayload;
                quicEncodeFrame(nt, &ntPayload);
                sendPacket(QuicPnApplication, ntPayload, true, false);
            }
        }
    }
    flushCrypto();
}

void QuicConnectionPrivate::onStreamFrame(QuicPacketNumberSpace space, const QuicFrame &frame)
{
    (void) space;
    shared_ptr<QuicStream> s = getOrCreateStream(frame.streamId);
    if (!s) {
        return;
    }
    QuicStreamPrivate *sd = s->d_func();
    // Receive-side flow control: per-stream limit.
    if (frame.offset + frame.data.size() > maxStreamDataLocal) {
        setError(QuicConnection::ProtocolError, "flow control: stream data exceeds limit", QuicErrFlowControlError);
        return;
    }
    recvDataTotal += frame.data.size();
    if (frame.offset + frame.data.size() > sd->recvOffset) {
        // Only reassemble data that is not already fully consumed.
        uint64_t newDataStart = max(frame.offset, sd->recvOffset);
        if (frame.offset + frame.data.size() > newDataStart) {
            const size_t skip = static_cast<size_t>(newDataStart - frame.offset);
            string data = frame.data.substr(skip);
            sd->recvFragments[newDataStart] = data;
        }
        bool madeProgress = false;
        while (sd->recvFragments.count(sd->recvOffset)) {
            auto it = sd->recvFragments.find(sd->recvOffset);
            sd->recvBuf.append(it->second);
            sd->recvOffset += it->second.size();
            sd->recvFragments.erase(it);
            madeProgress = true;
        }
        if (madeProgress) {
            sd->dataReady.set();
            // Reassembly alone does not advance the flow-control base; only the
            // application consuming bytes does (see QuicStream::recv).
        }
    }
    if (frame.fin) {
        // Remember the FIN boundary; data may still be out of order.
        const uint64_t finEnd = frame.offset + frame.data.size();
        if (!sd->finReceived && finEnd > sd->finBoundary) {
            sd->finBoundary = finEnd;
            sd->hasFinBoundary = true;
        }
    }
    // Declare FIN once the reassembled offset reaches the boundary.
    if (!sd->finReceived && sd->hasFinBoundary && sd->recvOffset >= sd->finBoundary) {
        sd->finReceived = true;
        sd->dataReady.set();
    }
}

void QuicConnectionPrivate::handleFrame(QuicPacketNumberSpace space, const QuicFrame &frame)
{
    switch (frame.type) {
    case QuicFrame::Ack:
    case QuicFrame::AckEcn: {
        vector<QuicSentPacket> acked;
        vector<string> lost;
        recovery.onAckReceived(space, frame, quicNowUs(), &acked, &lost);
        size_t bytesAcked = 0;
        for (const QuicSentPacket &p : acked) {
            bytesAcked += p.raw.size();
        }
        if (bytesAcked > 0) {
            cc->onAckReceived(bytesAcked, recovery.bytesInFlight());
        }
        // RFC 9002 §7.3: congestion control reacts once per loss-detection event,
        // sized by the lost bytes and the bytes in flight *before* the loss.
        size_t lostBytes = 0;
        for (const string &pkt : lost) {
            lostBytes += pkt.size();
        }
        if (lostBytes > 0) {
            cc->onLossDetected(lostBytes, recovery.bytesInFlight() + lostBytes);
            for (const string &pkt : lost) {
                link->sendto(pkt.data(), static_cast<int32_t>(pkt.size()), peerPath);
            }
        }
        break;
    }
    case QuicFrame::Crypto:
        onCryptoFrame(space, frame);
        break;
    case QuicFrame::Stream:
        onStreamFrame(space, frame);
        break;
    case QuicFrame::ConnectionClose:
    case QuicFrame::ConnectionCloseApp:
        setError(QuicConnection::ClosedError, frame.reasonPhrase.empty() ? "connection close" : frame.reasonPhrase);
        closed = true;
        break;
    case QuicFrame::HandshakeDone:
        // Server confirms the handshake is complete (RFC 9000 §17.2.3).
        handshakeConfirmed = true;
        break;
    case QuicFrame::Ping:
        break;
    case QuicFrame::PathChallenge: {
        if (frame.pathData.size() != 8) {
            break;
        }
        QuicFrame resp;
        resp.type = QuicFrame::PathResponse;
        resp.pathData = frame.pathData;
        string payload;
        if (quicEncodeFrame(resp, &payload)) {
            sendPacket(space, payload, true, false);
        }
        break;
    }
    case QuicFrame::PathResponse: {
        // Only accept a PATH_RESPONSE matching an outstanding challenge.
        auto it = pendingPathChallenges.find(frame.pathData);
        if (it != pendingPathChallenges.end()) {
            peerPath = DatagramPath(it->second);
            pathToAddr(peerPath, &peerAddr, &peerPort);
            pendingPathChallenges.clear();
            pathValidationDeadlineUs = 0;
            // Use a fresh peer CID if the peer published any.
            if (!peerCids.empty() && !(peerCids.begin()->second == remoteCid)) {
                remoteCid = peerCids.begin()->second;
            }
        }
        break;
    }
    case QuicFrame::MaxData:
        if (frame.maxData > maxDataRemote) {
            maxDataRemote = frame.maxData;
        }
        break;
    case QuicFrame::MaxStreamData: {
        uint64_t &v = maxStreamDataRemote[frame.streamId];
        if (frame.maxData > v) {
            v = frame.maxData;
        }
        break;
    }
    case QuicFrame::MaxStreamsBidi:
        if (frame.maxData > maxStreamsBidiRemote) {
            maxStreamsBidiRemote = frame.maxData;
        }
        break;
    case QuicFrame::MaxStreamsUni:
        if (frame.maxData > maxStreamsUniRemote) {
            maxStreamsUniRemote = frame.maxData;
        }
        break;
    case QuicFrame::DataBlocked:
        maybeIncreaseMaxData();
        break;
    case QuicFrame::StreamDataBlocked:
        maybeIncreaseMaxStreamData(frame.streamId);
        break;
    case QuicFrame::NewConnectionId: {
        peerCids[frame.sequenceNumber] = frame.connectionId;
        peerCidTokens[frame.sequenceNumber] = frame.statelessResetToken;
        // Retire CIDs the peer asked us to retire.
        for (auto it = peerCids.begin(); it != peerCids.end();) {
            if (it->first < frame.retirePriorTo) {
                QuicFrame r;
                r.type = QuicFrame::RetireConnectionId;
                r.sequenceNumber = it->first;
                string payload;
                quicEncodeFrame(r, &payload);
                sendPacket(QuicPnApplication, payload, true, false);
                peerCidTokens.erase(it->first);
                it = peerCids.erase(it);
            } else {
                ++it;
            }
        }
        break;
    }
    case QuicFrame::RetireConnectionId:
        // Peer retired one of our CIDs; track the highest retired sequence.
        if (frame.sequenceNumber + 1 > nextCidSeq) {
            nextCidSeq = frame.sequenceNumber + 1;
        }
        break;
    case QuicFrame::Padding:
    case QuicFrame::NewToken:
        if (!frame.data.empty()) {
            newTokens.push_back(frame.data);
        }
        break;
    case QuicFrame::StreamsBlockedBidi:
    case QuicFrame::StreamsBlockedUni:
    default:
        break;
    }
}

void QuicConnectionPrivate::handlePayload(QuicPacketNumberSpace space, const string &payload, uint64_t pn)
{
    if (space == QuicPnInitial) {
        largestRecvInitial = max(largestRecvInitial, pn);
        hasRecvInitial = true;
    } else if (space == QuicPnHandshake) {
        largestRecvHandshake = max(largestRecvHandshake, pn);
        hasRecvHandshake = true;
    } else {
        largestRecvApp = max(largestRecvApp, pn);
        hasRecvApp = true;
    }
    receivedPn[space].insert(pn);
    vector<QuicFrame> frames;
    if (!quicDecodeFrames(payload.data(), payload.size(), &frames)) {
        setError(QuicConnection::ProtocolError, "bad frames", QuicErrFrameEncodingError);
        return;
    }
    bool ackEliciting = false;
    for (const QuicFrame &f : frames) {
        if (f.type != QuicFrame::Ack && f.type != QuicFrame::AckEcn && f.type != QuicFrame::Padding) {
            ackEliciting = true;
        }
        handleFrame(space, f);
    }
    if (ackEliciting) {
        scheduleAck(space);
    }
}

void QuicConnectionPrivate::setError(QuicConnection::Error e, const string &msg, uint64_t tec)
{
    error = e;
    errorString = msg;
    transportErrorCode = tec;
}

void QuicConnectionPrivate::startReader()
{
    if (readerStarted) {
        return;
    }
    readerStarted = true;
    reader.reset(Coroutine::spawn([this] { readerLoop(); }));
    ptoTimer.reset(Coroutine::spawn([this] { ptoLoop(); }));
    sendLoopCoroutine.reset(Coroutine::spawn([this] { sendLoop(); }));
    ackLoopCoroutine.reset(Coroutine::spawn([this] { ackLoop(); }));
}

void QuicConnectionPrivate::readerLoop()
{
    char buf[2048];
    while (!closed && link && link->isValid()) {
        DatagramPath who;
        int32_t n = link->recvfrom(buf, sizeof(buf), &who);
        if (n <= 0) {
            if (closed) {
                break;
            }
            Coroutine::sleep(0.01f);
            continue;
        }
        if (!who.isNull()) {
            if (hasAppKeys && !(who == peerPath)) {
                // Packet from an unexpected address: start path validation but keep
                // sending on the validated path until PATH_RESPONSE confirms it.
                onPossibleMigration(who);
            } else {
                peerPath = who;
                pathToAddr(who, &peerAddr, &peerPort);
            }
        }
        lastActivityUs = quicNowUs();

        QuicPacketHeader peek;
        if (quicParsePacketHeader(buf, static_cast<size_t>(n), &peek, true) && peek.isLong
            && peek.longType == QuicLongInitial) {
            if (!clientInitialKeys.valid()) {
                odcid = peek.dcid;
                clientInitialScid = peek.scid;
                if (requireAddressValidation && peek.token.empty() && !sentRetry) {
                    // Validate the client's address with a RETRY first.
                    sendRetryPacket();
                    continue;
                }
                if (requireAddressValidation && !peek.token.empty() && !validateRetryToken(peek.token, peerPath)) {
                    continue;  // invalid or stale token: ignore this Initial
                }
                remoteCid = peek.scid;
                clientInitialKeys = quicDeriveTrafficKeys(quicDeriveInitialSecret(odcid, true));
                serverInitialKeys = quicDeriveTrafficKeys(quicDeriveInitialSecret(odcid, false));
                if (!isClient && !tls) {
                    setupTls(false);
                }
            }
        }

        size_t offset = 0;
        const size_t datagramLen = static_cast<size_t>(n);
        while (offset < datagramLen) {
            const char *pkt = buf + offset;
            const size_t remain = datagramLen - offset;
            if (static_cast<unsigned char>(pkt[0]) == 0) {
                break;
            }

            QuicPacketHeader peekHdr;
            if (!quicParsePacketHeader(pkt, remain, &peekHdr, true)) {
                break;
            }

            QuicPacketNumberSpace space = QuicPnApplication;
            bool isZeroRtt = false;
            if (peekHdr.isLong) {
                if (peekHdr.longType == QuicLongInitial) {
                    space = QuicPnInitial;
                } else if (peekHdr.longType == QuicLongHandshake) {
                    space = QuicPnHandshake;
                } else if (peekHdr.longType == QuicLongRetry) {
                    handleRetryPacket(pkt, remain);
                    break;
                } else if (peekHdr.longType == QuicLongZeroRtt) {
                    // 0-RTT packets use the client early keys; application-data space.
                    isZeroRtt = true;
                    space = QuicPnApplication;
                } else {
                    break;
                }
            }

            QuicTrafficKeys *keys = nullptr;
            if (isZeroRtt) {
                if (!hasEarlyKeys) {
                    break;  // no early keys yet: cannot decrypt, drop
                }
                keys = &clientEarlyKeys;
            } else {
                keys = keysForRecv(space);
                if (!keys || !keys->valid()) {
                    break;
                }
            }

            QuicPacketHeader hdr;
            string payload;
            if (space == QuicPnApplication) {
                hdr.dcid = localCid;
            }
            size_t consumed = 0;
            const bool firstTry = quicUnprotectPacket(*keys, pkt, remain, &hdr, &payload, largestReceivedPn(space),
                                                      &consumed)
                    && consumed > 0;
            bool decrypted = firstTry;
            // On failure, try the next key. The key phase bit read with the old
            // header-protection key is unreliable after a rotation (RFC 9001 §6),
            // so try unconditionally when a next key is available.
            if (!firstTry && !isZeroRtt && space == QuicPnApplication && hasNextRecvAppKeys) {
                QuicPacketHeader hdr2;
                hdr2.dcid = localCid;
                size_t consumed2 = 0;
                if (quicUnprotectPacket(nextRecvAppKeys, pkt, remain, &hdr2, &payload, largestReceivedPn(space),
                                        &consumed2)
                    && consumed2 > 0) {
                    recvKeyPhase = hdr2.keyPhase;
                    recvAppKeys = nextRecvAppKeys;
                    recvAppSecret = nextRecvSecret;
                    hasNextRecvAppKeys = false;
                    deriveNextRecvKeys();
                    hdr = hdr2;
                    consumed = consumed2;
                    decrypted = true;
                }
            }
            if (!decrypted) {
                if (!isZeroRtt && space == QuicPnApplication) {
                    if (isStatelessReset(pkt, remain)) {
                        setError(QuicConnection::ClosedError, "stateless reset");
                        closed = true;
                        return;
                    }
                    // Server: an undecryptable short-header packet for one of our
                    // CIDs triggers a stateless reset (RFC 9000 §10.3).
                    if (!isClient && remain >= 1 + static_cast<int>(localCid.bytes.size())) {
                        const string dcidBytes(pkt + 1, localCid.bytes.size());
                        if (localCidTokens.count(dcidBytes)) {
                            sendStatelessReset(dcidBytes);
                        }
                    }
                }
                break;
            }
            if (hdr.isLong && hdr.version != 0 && hdr.version != kQuicVersion1) {
                setError(QuicConnection::UnsupportedVersion, "unsupported version");
                return;
            }
            if (hdr.isLong && !hdr.scid.empty()) {
                remoteCid = hdr.scid;
            }
            handlePayload(space, payload, hdr.packetNumber);
            offset += consumed;
        }
    }
}

void QuicConnectionPrivate::ptoLoop()
{
    while (!closed) {
        Coroutine::sleep(0.02f);
        const uint64_t now = quicNowUs();
        // Idle timeout (RFC 9000 §10.1).
        if (lastActivityUs != 0 && config.idleTimeout() > 0) {
            const uint64_t idleUs = static_cast<uint64_t>(config.idleTimeout() * 1000 * 1000);
            if (now - lastActivityUs > idleUs) {
                setError(QuicConnection::TimeoutError, "idle timeout");
                closed = true;
                return;
            }
        }
        // Path validation timeout (RFC 9000 §8.2.3).
        if (!pendingPathChallenges.empty() && now > pathValidationDeadlineUs) {
            pendingPathChallenges.clear();
            pathValidationDeadlineUs = 0;
        }
        for (QuicPacketNumberSpace space : {QuicPnInitial, QuicPnHandshake, QuicPnApplication}) {
            // Loss-detection alarm (time threshold).
            const uint64_t lossTime = recovery.earliestLostTimeUs(space);
            if (lossTime > 0 && now >= lossTime) {
                vector<QuicSentPacket> lost;
                recovery.detectLostOnAlarm(space, now, &lost);
                for (const QuicSentPacket &p : lost) {
                    cc->onLossDetected(p.raw.size(), recovery.bytesInFlight());
                    link->sendto(p.raw.data(), static_cast<int32_t>(p.raw.size()), peerPath);
                }
            }
            // PTO.
            if (!recovery.hasInFlight(space)) {
                continue;
            }
            const uint64_t lastSent = recovery.lastSentTimeUs(space);
            if (lastSent > 0 && now >= lastSent + recovery.ptoTimeoutUs(space)) {
                recovery.onPtoExpired(space);
                if (recovery.ptoCount(space) >= 2) {
                    cc->onPersistentCongestion();
                }
                vector<string> pkts = recovery.packetsToRetransmitOnPto(space);
                if (!pkts.empty()) {
                    link->sendto(pkts.front().data(), static_cast<int32_t>(pkts.front().size()), peerPath);
                }
            }
        }
    }
}

// --- QuicConnection ---

QuicConnection::QuicConnection(shared_ptr<DatagramLink> link)
    : d_ptr(new QuicConnectionPrivate(this, link))
{
}

QuicConnection::QuicConnection(HostAddress::NetworkLayerProtocol protocol)
    : d_ptr(new QuicConnectionPrivate(this, shared_ptr<DatagramLink>()))
{
    NG_D(QuicConnection);
    d->udpSocket = make_shared<Socket>(protocol, Socket::UdpSocket);
    d->link = make_shared<SocketDatagramLink>(d->udpSocket);
}

QuicConnection::QuicConnection(QuicConnectionPrivate *priv)
    : d_ptr(priv)
{
}

QuicConnection::~QuicConnection()
{
    NG_D(QuicConnection);
    d->closed = true;
    if (d->link) {
        d->link->abort();
    }
    // All internal loop coroutines poll `closed`, so joining without kill() lets
    // them exit naturally. kill() is asynchronous (callLater) and can outlive
    // the Coroutine object, which would crash the event loop.
    if (d->reader) {
        d->reader->join();
        d->reader.reset();
    }
    if (d->ptoTimer) {
        d->ptoTimer->join();
        d->ptoTimer.reset();
    }
    if (d->sendLoopCoroutine) {
        d->sendLoopCoroutine->join();
        d->sendLoopCoroutine.reset();
    }
    if (d->ackLoopCoroutine) {
        d->ackLoopCoroutine->join();
        d->ackLoopCoroutine.reset();
    }
    delete d_ptr;
}

std::shared_ptr<Event> QuicConnection::handshakeDone() const
{
    NG_D(const QuicConnection);
    return d->handshakeDoneEvent;
}

QuicSessionTicket QuicConnection::takeSessionTicket() const
{
    const NG_D(QuicConnection);
    QuicSessionTicket t;
    if (d->tls && d->tls->hasSessionTicket()) {
        t.ticket = d->tls->sessionTicket();
        t.ticketNonce = d->tls->sessionTicketNonce();
        t.resumptionSecret = d->tls->sessionResumptionSecret();
    }
    return t;
}

bool QuicConnection::setSessionTicket(const QuicSessionTicket &ticket)
{
    if (!ticket.isValid()) {
        return false;
    }
    NG_D(QuicConnection);
    if (d->tls) {
        d->tls->setSessionTicket(ticket.ticket, ticket.ticketNonce, ticket.resumptionSecret);
    } else {
        d->pendingSessionTicket = ticket;
    }
    return true;
}

bool QuicConnection::earlyDataAccepted() const
{
    const NG_D(QuicConnection);
    return d->tls && d->tls->earlyDataAccepted();
}

void QuicConnection::updateKeys()
{
    NG_D(QuicConnection);
    if (d->hasAppKeys && d->appPacketsSent > 0) {
        d->maybeUpdateSendKeys();
    }
}

void QuicConnection::setConfiguration(const QuicConfiguration &config)
{
    NG_D(QuicConnection);
    d->config = config;
    if (d->config.congestionController()) {
        d->cc = d->config.congestionController();
    }
    d->requireAddressValidation = config.requireAddressValidation();
}

QuicConfiguration QuicConnection::configuration() const
{
    const NG_D(QuicConnection);
    return d->config;
}

QuicConnection::Error QuicConnection::error() const
{
    const NG_D(QuicConnection);
    return d->error;
}

string QuicConnection::errorString() const
{
    const NG_D(QuicConnection);
    return d->errorString;
}

QuicConnection::State QuicConnection::state() const
{
    const NG_D(QuicConnection);
    return d->state;
}

bool QuicConnection::isValid() const
{
    const NG_D(QuicConnection);
    return d->link && d->link->isValid() && d->error == NoError;
}

bool QuicConnection::isClientSide() const
{
    const NG_D(QuicConnection);
    return d->isClient;
}

HostAddress QuicConnection::localAddress() const
{
    const NG_D(QuicConnection);
    return d->localAddr;
}
uint16_t QuicConnection::localPort() const
{
    const NG_D(QuicConnection);
    return d->localPort;
}
HostAddress QuicConnection::peerAddress() const
{
    const NG_D(QuicConnection);
    return d->peerAddr;
}
uint16_t QuicConnection::peerPort() const
{
    const NG_D(QuicConnection);
    return d->peerPort;
}
string QuicConnection::peerName() const
{
    const NG_D(QuicConnection);
    return d->peerName;
}

bool QuicConnection::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    NG_D(QuicConnection);
    if (!d->udpSocket) {
        d->setError(SocketError, "no udp socket");
        return false;
    }
    if (!d->udpSocket->bind(address, port, mode)) {
        d->setError(SocketError, d->udpSocket->errorString());
        return false;
    }
    d->localAddr = d->udpSocket->localAddress();
    d->localPort = d->udpSocket->localPort();
    d->state = BoundState;
    return true;
}

bool QuicConnection::bind(uint16_t port, Socket::BindMode mode)
{
    return bind(HostAddress(HostAddress::Any), port, mode);
}

bool QuicConnection::listen(int backlog)
{
    NG_D(QuicConnection);
    (void) backlog;
    if (d->config.privateKey().isNull() || d->config.localCertificate().isNull()) {
        d->setError(HandshakeError, "server requires certificate and key");
        return false;
    }
    d->localCid.bytes = makeCid(8);
    d->isClient = false;
    d->state = ListeningState;
    d->startReader();
    return true;
}

shared_ptr<QuicConnection> QuicConnection::accept()
{
    NG_D(QuicConnection);
    if (d->state != ListeningState) {
        return shared_ptr<QuicConnection>();
    }
    if (d->incomingConns.isEmpty()) {
        Coroutine::sleep(0.05f);
    }
    if (d->incomingConns.isEmpty()) {
        return shared_ptr<QuicConnection>();
    }
    return d->incomingConns.get();
}

bool QuicConnection::serve()
{
    NG_D(QuicConnection);
    if (d->config.privateKey().isNull() || d->config.localCertificate().isNull()) {
        d->setError(HandshakeError, "server requires certificate and key");
        return false;
    }
    if (d->localCid.empty()) {
        d->localCid.bytes = makeCid(8);
    }
    d->isClient = false;
    d->state = ConnectingState;
    d->startReader();
    float waited = 0;
    while (d->state != ConnectedState && d->error == NoError && waited < 5.0f) {
        Coroutine::sleep(0.01f);
        waited += 0.01f;
    }
    return d->state == ConnectedState;
}

bool QuicConnection::connect(const HostAddress &addr, uint16_t port, const string &serverName)
{
    NG_D(QuicConnection);
    d->peerAddr = addr;
    d->peerPort = port;
    d->peerPath = makePath(addr, port);
    d->peerName = serverName.empty() ? addr.toString() : serverName;
    if (d->udpSocket && d->state != BoundState) {
        if (!d->udpSocket->bind(HostAddress(HostAddress::AnyIPv4), 0)) {
            d->setError(SocketError, d->udpSocket->errorString());
            return false;
        }
        d->localAddr = d->udpSocket->localAddress();
        d->localPort = d->udpSocket->localPort();
    }
    d->localCid.bytes = makeCid(8);
    d->remoteCid.bytes = makeCid(8);
    d->odcid = d->remoteCid;
    d->clientInitialKeys = quicDeriveTrafficKeys(quicDeriveInitialSecret(d->remoteCid, true));
    d->serverInitialKeys = quicDeriveTrafficKeys(quicDeriveInitialSecret(d->remoteCid, false));
    d->setupTls(true);
    d->state = ConnectingState;
    d->startReader();
    string err;
    if (!d->tls->startClientHello(&err)) {
        d->setError(HandshakeError, err);
        return false;
    }
    d->driveTls();
    const float deadline = 5.0f;
    float waited = 0;
    while (d->state != ConnectedState && d->error == NoError && waited < deadline) {
        Coroutine::sleep(0.01f);
        waited += 0.01f;
    }
    return d->state == ConnectedState;
}

bool QuicConnection::connect(const string &hostName, uint16_t port, const string &serverName,
                             shared_ptr<SocketDnsCache> dnsCache)
{
    HostAddress addr;
    unique_ptr<Socket> probe(Socket::createConnection(hostName, port, nullptr, dnsCache));
    if (!probe) {
        NG_D(QuicConnection);
        d->setError(SocketError, "dns/connect failed");
        return false;
    }
    addr = probe->peerAddress();
    probe.reset();
    return connect(addr, port, serverName.empty() ? hostName : serverName);
}

bool QuicConnection::connect(const DatagramPath &peer, const string &serverName)
{
    NG_D(QuicConnection);
    d->peerPath = peer;
    pathToAddr(peer, &d->peerAddr, &d->peerPort);
    d->peerName = serverName.empty() ? peer.key() : serverName;
    d->localCid.bytes = makeCid(8);
    d->remoteCid.bytes = makeCid(8);
    d->odcid = d->remoteCid;
    d->clientInitialKeys = quicDeriveTrafficKeys(quicDeriveInitialSecret(d->remoteCid, true));
    d->serverInitialKeys = quicDeriveTrafficKeys(quicDeriveInitialSecret(d->remoteCid, false));
    d->setupTls(true);
    d->state = ConnectingState;
    d->startReader();
    string err;
    if (!d->tls->startClientHello(&err)) {
        d->setError(HandshakeError, err);
        return false;
    }
    d->driveTls();
    float waited = 0;
    while (d->state != ConnectedState && d->error == NoError && waited < 5.0f) {
        Coroutine::sleep(0.01f);
        waited += 0.01f;
    }
    return d->state == ConnectedState;
}

shared_ptr<QuicStream> QuicConnection::openStream()
{
    NG_D(QuicConnection);
    if (d->state != ConnectedState && !(d->isClient && d->hasEarlyKeys)) {
        return shared_ptr<QuicStream>();
    }
    uint64_t id = d->isClient ? d->nextClientBidi : d->nextServerBidi;
    if (d->state == ConnectedState && d->bidiStreamsOpened >= d->maxStreamsBidiRemote) {
        return shared_ptr<QuicStream>();
    }
    if (d->isClient) {
        d->nextClientBidi += 4;
    } else {
        d->nextServerBidi += 4;
    }
    ++d->bidiStreamsOpened;
    return d->getOrCreateStream(id);
}

shared_ptr<QuicStream> QuicConnection::openUniStream()
{
    NG_D(QuicConnection);
    if (d->state != ConnectedState) {
        return shared_ptr<QuicStream>();
    }
    uint64_t id = d->isClient ? d->nextClientUni : d->nextServerUni;
    if (d->uniStreamsOpened >= d->maxStreamsUniRemote) {
        return shared_ptr<QuicStream>();
    }
    if (d->isClient) {
        d->nextClientUni += 4;
    } else {
        d->nextServerUni += 4;
    }
    ++d->uniStreamsOpened;
    return d->getOrCreateStream(id);
}

shared_ptr<QuicStream> QuicConnection::acceptStream()
{
    NG_D(QuicConnection);
    float waited = 0;
    while (waited < 5.0f && d->error == NoError) {
        for (auto &kv : d->streams) {
            // Only peer-initiated bidirectional streams are "accepted" streams.
            // Unidirectional streams (control/QPACK in HTTP/3, id & 2) are
            // application-level and must not be handed out here.
            if (kv.first & 2) {
                continue;
            }
            const bool peerInitiated = d->isClient ? ((kv.first & 1) == 1) : ((kv.first & 1) == 0);
            if (peerInitiated && !d->acceptedStreams.count(kv.first)) {
                d->acceptedStreams.insert(kv.first);
                return kv.second;
            }
        }
        Coroutine::sleep(0.01f);
        waited += 0.01f;
    }
    return shared_ptr<QuicStream>();
}

void QuicConnection::close()
{
    NG_D(QuicConnection);
    if (d->hasAppKeys) {
        QuicFrame f;
        f.type = QuicFrame::ConnectionClose;
        f.errorCode = d->transportErrorCode;
        f.frameType = 0;
        f.reasonPhrase = "close";
        string payload;
        quicEncodeFrame(f, &payload);
        d->sendPacket(QuicPnApplication, payload, true, false);
    }
    d->closed = true;
    d->state = ClosingState;
    if (d->link) {
        d->link->close();
    }
}

void QuicConnection::abort()
{
    NG_D(QuicConnection);
    d->closed = true;
    if (d->link) {
        d->link->abort();
    }
    d->state = UnconnectedState;
}

// --- flow control helpers ---

uint64_t QuicConnectionPrivate::streamSendLimit(uint64_t streamId) const
{
    auto it = maxStreamDataRemote.find(streamId);
    if (it != maxStreamDataRemote.end()) {
        return it->second;
    }
    return streamInitialLimitRemote;
}

// Wait until at least one byte of send window is available, returning the number
// of bytes the peer currently allows (per-stream and connection level combined).
// Returns 0 on error/timeout.
size_t QuicConnectionPrivate::waitForSendWindow(QuicStreamPrivate *sd)
{
    if (!hasAppKeys) {
        return 0;
    }
    float waited = 0;
    while (!closed && error == QuicConnection::NoError) {
        const uint64_t limit = streamSendLimit(sd->streamId);
        const size_t streamAvail = (limit > sd->sendOffset) ? static_cast<size_t>(limit - sd->sendOffset) : 0;
        const size_t connAvail = (maxDataRemote > dataSent) ? static_cast<size_t>(maxDataRemote - dataSent) : 0;
        if (streamAvail > 0 && connAvail > 0) {
            return min(streamAvail, connAvail);
        }
        sendBlockedIfNeeded(sd->streamId, sd->sendOffset, limit);
        Coroutine::sleep(0.01f);
        waited += 0.01f;
        if (waited > 30.0f) {
            return 0;
        }
    }
    return 0;
}

void QuicConnectionPrivate::sendBlockedIfNeeded(uint64_t streamId, uint64_t offset, uint64_t limit)
{
    const uint64_t now = quicNowUs();
    if (now - lastBlockedSentUs < 500 * 1000) {
        return;
    }
    lastBlockedSentUs = now;
    if (offset + 1 > limit) {
        QuicFrame f;
        f.type = QuicFrame::StreamDataBlocked;
        f.streamId = streamId;
        f.maxData = limit;
        string payload;
        if (quicEncodeFrame(f, &payload)) {
            sendPacket(QuicPnApplication, payload, true, false);
        }
    } else if (dataSent + 1 > maxDataRemote) {
        QuicFrame f;
        f.type = QuicFrame::DataBlocked;
        f.maxData = maxDataRemote;
        string payload;
        if (quicEncodeFrame(f, &payload)) {
            sendPacket(QuicPnApplication, payload, true, false);
        }
    }
}

void QuicConnectionPrivate::waitSendDrained()
{
    float waited = 0;
    while (!closed && !sendQueue.isEmpty() && error == QuicConnection::NoError) {
        Coroutine::sleep(0.005f);
        waited += 0.005f;
        if (waited > 30.0f) {
            return;
        }
    }
}

void QuicConnectionPrivate::maybeIncreaseMaxData()
{
    // Connection-level consumption is the sum of what the application has read
    // across all streams (RFC 9000 §4.1). Window slides: limit = consumed + window.
    uint64_t consumedTotal = 0;
    for (const auto &kv : streams) {
        QuicStreamPrivate *sd = kv.second->d_func();
        consumedTotal += sd->recvOffset - sd->recvBuf.size();
    }
    const uint64_t window = max<uint64_t>(config.maxData(), 1024 * 1024);
    if (consumedTotal + window > maxDataLocal) {
        maxDataLocal = consumedTotal + window;
        QuicFrame f;
        f.type = QuicFrame::MaxData;
        f.maxData = maxDataLocal;
        string payload;
        if (quicEncodeFrame(f, &payload)) {
            sendPacket(QuicPnApplication, payload, true, false);
        }
    }
}

void QuicConnectionPrivate::maybeIncreaseMaxStreamData(uint64_t streamId)
{
    auto it = streams.find(streamId);
    if (it == streams.end()) {
        return;
    }
    QuicStreamPrivate *sd = it->second->d_func();
    // Per-stream consumption base, not the received offset: data sitting in the
    // reassembly buffer must not inflate the window (RFC 9000 §4.1).
    const uint64_t consumed = sd->recvOffset - sd->recvBuf.size();
    const uint64_t window = max<uint64_t>(config.maxStreamData(), 256 * 1024);
    if (consumed + window > maxStreamDataLocal) {
        maxStreamDataLocal = consumed + window;
        QuicFrame f;
        f.type = QuicFrame::MaxStreamData;
        f.streamId = streamId;
        f.maxData = maxStreamDataLocal;
        string payload;
        if (quicEncodeFrame(f, &payload)) {
            sendPacket(QuicPnApplication, payload, true, false);
        }
    }
}

// --- QuicStream ---

QuicStream::QuicStream(QuicStreamPrivate *d)
    : d_ptr(d)
{
}

QuicStream::~QuicStream()
{
    delete d_ptr;
}

uint64_t QuicStream::streamId() const
{
    const NG_D(QuicStream);
    return d->streamId;
}

bool QuicStream::isValid() const
{
    const NG_D(QuicStream);
    return !d->reset && d->connection && !d->connection->closed;
}

QuicConnection::Error QuicStream::error() const
{
    const NG_D(QuicStream);
    return d->error;
}

string QuicStream::errorString() const
{
    const NG_D(QuicStream);
    return d->errorString;
}

int32_t QuicStream::recv(char *data, int32_t size)
{
    NG_D(QuicStream);
    while (d->recvBuf.empty() && !d->finReceived && !d->reset && d->connection->error == QuicConnection::NoError) {
        d->dataReady.clear();
        d->dataReady.tryWait(100);
    }
    if (d->recvBuf.empty()) {
        return d->finReceived ? 0 : -1;
    }
    const int32_t n = min(size, static_cast<int32_t>(d->recvBuf.size()));
    memcpy(data, d->recvBuf.data(), static_cast<size_t>(n));
    d->recvBuf.erase(0, static_cast<size_t>(n));
    // Consumption advanced the flow-control base: slide the window so the peer
    // can keep sending (RFC 9000 §4.1 flow control is consumption-based).
    if (d->connection && d->connection->hasAppKeys) {
        d->connection->maybeIncreaseMaxStreamData(d->streamId);
        d->connection->maybeIncreaseMaxData();
    }
    return n;
}

int32_t QuicStream::recvall(char *data, int32_t size)
{
    int32_t got = 0;
    while (got < size) {
        int32_t n = recv(data + got, size - got);
        if (n <= 0) {
            return got > 0 ? got : n;
        }
        got += n;
    }
    return got;
}

int32_t QuicStream::send(const char *data, int32_t size)
{
    NG_D(QuicStream);
    if (d->finSent || d->reset) {
        return -1;
    }
    if (size <= 0) {
        return 0;
    }
    QuicConnectionPrivate *conn = d->connection;
    // 0-RTT path: data sent before the handshake completes, replayed if rejected.
    if (!conn->hasAppKeys && conn->isClient && conn->hasEarlyKeys) {
        if (!d->sentZeroRtt) {
            d->zeroRttStartOffset = d->sendOffset;
            d->sentZeroRtt = true;
        }
        int32_t sent = 0;
        while (sent < size) {
            const size_t chunk = min<size_t>(static_cast<size_t>(size - sent), kMaxStreamChunk);
            QuicFrame f;
            f.type = QuicFrame::Stream;
            f.streamId = d->streamId;
            f.offset = d->sendOffset;
            f.data.assign(data + sent, chunk);
            f.hasLength = true;
            f.fin = false;
            string payload;
            quicEncodeFrame(f, &payload);
            if (!conn->sendZeroRttPacket(payload)) {
                return sent > 0 ? sent : -1;
            }
            d->sendOffset += chunk;
            d->zeroRttData.append(data + sent, chunk);
            sent += static_cast<int32_t>(chunk);
        }
        return size;
    }
    if (!conn->hasAppKeys) {
        return -1;
    }
    int32_t sent = 0;
    while (sent < size) {
        // Send in window-sized chunks: the peer's flow-control window slides as
        // it consumes, so a large send must be broken up across multiple windows.
        const size_t avail = conn->waitForSendWindow(d);
        if (avail == 0) {
            return sent > 0 ? sent : -1;
        }
        const size_t chunk = min<size_t>({static_cast<size_t>(size - sent), avail, kMaxStreamChunk});
        conn->dataSent += chunk;
        QuicFrame f;
        f.type = QuicFrame::Stream;
        f.streamId = d->streamId;
        f.offset = d->sendOffset;
        f.data.assign(data + sent, chunk);
        f.hasLength = true;
        f.fin = false;
        string payload;
        quicEncodeFrame(f, &payload);
        if (!conn->sendPacket(QuicPnApplication, payload, true, true)) {
            return sent > 0 ? sent : -1;
        }
        d->sendOffset += chunk;
        sent += static_cast<int32_t>(chunk);
    }
    conn->waitSendDrained();
    return size;
}

int32_t QuicStream::sendall(const char *data, int32_t size)
{
    return send(data, size);
}

string QuicStream::recv(int32_t size)
{
    string buf(static_cast<size_t>(size), '\0');
    int32_t n = recv(&buf[0], size);
    if (n < 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

string QuicStream::recvall(int32_t size)
{
    string buf(static_cast<size_t>(size), '\0');
    int32_t n = recvall(&buf[0], size);
    if (n < 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

int32_t QuicStream::send(const string &data)
{
    return send(data.data(), static_cast<int32_t>(data.size()));
}

int32_t QuicStream::sendall(const string &data)
{
    return sendall(data.data(), static_cast<int32_t>(data.size()));
}

void QuicStream::close()
{
    NG_D(QuicStream);
    if (d->finSent) {
        return;
    }
    QuicConnectionPrivate *conn = d->connection;
    if (!conn->hasAppKeys && conn->isClient && conn->hasEarlyKeys) {
        // 0-RTT FIN.
        QuicFrame f;
        f.type = QuicFrame::Stream;
        f.streamId = d->streamId;
        f.offset = d->sendOffset;
        f.hasLength = true;
        f.fin = true;
        string payload;
        quicEncodeFrame(f, &payload);
        if (conn->sendZeroRttPacket(payload)) {
            d->finSent = true;
        }
        return;
    }
    if (!conn->hasAppKeys) {
        d->finSent = true;
        return;
    }
    // Wait for at least one byte of window so the FIN-carrying frame can be sent
    // (an empty window blocks even the FIN, per RFC 9000 §4.1).
    if (conn->waitForSendWindow(d) == 0) {
        d->finSent = true;
        return;
    }
    QuicFrame f;
    f.type = QuicFrame::Stream;
    f.streamId = d->streamId;
    f.offset = d->sendOffset;
    f.hasLength = true;
    f.fin = true;
    string payload;
    quicEncodeFrame(f, &payload);
    conn->sendPacket(QuicPnApplication, payload, true, true);
    conn->waitSendDrained();
    d->finSent = true;
}

void QuicStream::reset(uint64_t applicationErrorCode)
{
    NG_D(QuicStream);
    QuicFrame f;
    f.type = QuicFrame::ResetStream;
    f.streamId = d->streamId;
    f.applicationErrorCode = applicationErrorCode;
    f.finalSize = d->sendOffset;
    string payload;
    quicEncodeFrame(f, &payload);
    if (d->connection->hasAppKeys) {
        d->connection->sendPacket(QuicPnApplication, payload, true, true);
    }
    d->reset = true;
}

namespace {

class QuicStreamSocketLike : public SocketLike
{
public:
    explicit QuicStreamSocketLike(shared_ptr<QuicStream> s)
        : m_s(s)
    {
    }
    Socket::SocketError error() const override
    {
        return m_s->error() == QuicConnection::NoError ? Socket::NoError : Socket::UnknownSocketError;
    }
    string errorString() const override { return m_s->errorString(); }
    bool isValid() const override { return m_s && m_s->isValid(); }
    HostAddress localAddress() const override { return HostAddress(); }
    uint16_t localPort() const override { return 0; }
    HostAddress peerAddress() const override { return HostAddress(); }
    string peerName() const override { return string(); }
    uint16_t peerPort() const override { return 0; }
    intptr_t fileno() const override { return -1; }
    Socket::SocketType type() const override { return Socket::UnknownSocketType; }
    Socket::SocketState state() const override
    {
        return m_s && m_s->isValid() ? Socket::ConnectedState : Socket::UnconnectedState;
    }
    HostAddress::NetworkLayerProtocol protocol() const override { return HostAddress::UnknownNetworkLayerProtocol; }
    string localAddressURI() const override { return string(); }
    string peerAddressURI() const override { return string(); }
    shared_ptr<SocketLike> accept() override { return shared_ptr<SocketLike>(); }
    Socket *acceptRaw() override { return nullptr; }
    bool bind(const HostAddress &, uint16_t, Socket::BindMode) override { return false; }
    bool bind(uint16_t, Socket::BindMode) override { return false; }
    bool connect(const HostAddress &, uint16_t) override { return false; }
    bool connect(const string &, uint16_t, shared_ptr<SocketDnsCache>) override { return false; }
    void close() override
    {
        if (m_s) {
            m_s->close();
        }
    }
    void abort() override
    {
        if (m_s) {
            m_s->reset();
        }
    }
    bool listen(int) override { return false; }
    bool setOption(Socket::SocketOption, int) override { return false; }
    int option(Socket::SocketOption) const override { return -1; }
    int32_t peek(char *, int32_t) override { return -1; }
    int32_t peekRaw(char *, int32_t) override { return -1; }
    int32_t recv(char *data, int32_t size) override { return m_s->recv(data, size); }
    int32_t recvall(char *data, int32_t size) override { return m_s->recvall(data, size); }
    int32_t send(const char *data, int32_t size) override { return m_s->send(data, size); }
    int32_t sendall(const char *data, int32_t size) override { return m_s->sendall(data, size); }
    string recv(int32_t size) override { return m_s->recv(size); }
    string recvall(int32_t size) override { return m_s->recvall(size); }
    int32_t send(const string &data) override { return m_s->send(data); }
    int32_t sendall(const string &data) override { return m_s->sendall(data); }
private:
    shared_ptr<QuicStream> m_s;
};

}  // namespace

shared_ptr<SocketLike> asSocketLike(shared_ptr<QuicStream> s)
{
    return make_shared<QuicStreamSocketLike>(s);
}

}  // namespace qtng
