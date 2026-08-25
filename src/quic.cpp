#include "qtng/quic.h"

#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "qtng/coroutine.h"
#include "qtng/private/quic_p.h"
#include "qtng/private/quic_tls.h"
#include "qtng/random.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

namespace {

const uint32_t kQuicVersion1 = 0x00000001;

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

QuicConfiguration::QuicConfiguration()
    : m_idleTimeout(30.0f)
    , m_verifyPeer(false)
    , m_maxData(1024 * 1024)
    , m_maxStreamData(256 * 1024)
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
    {
    }

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
    bool hasHsKeys = false;
    bool hasAppKeys = false;

    map<uint64_t, shared_ptr<QuicStream>> streams;
    Queue<shared_ptr<QuicConnection>> incomingConns;
    uint64_t nextClientBidi;
    uint64_t nextServerBidi;

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
    uint64_t cryptoSendOffInitial = 0;
    uint64_t cryptoSendOffHandshake = 0;
    uint64_t cryptoRecvOffInitial = 0;
    uint64_t cryptoRecvOffHandshake = 0;

    QuicLossRecovery recovery;
    bool closed;
    bool readerStarted;
    shared_ptr<Event> handshakeDoneEvent;
    shared_ptr<Coroutine> reader;
    shared_ptr<Coroutine> ptoTimer;
    Lock sendLock;

    bool setupTls(bool client);
    bool sendPacket(QuicPacketNumberSpace space, const string &payload, bool ackEliciting = true);
    bool flushCrypto();
    void maybeSendAck(QuicPacketNumberSpace space);
    void handlePayload(QuicPacketNumberSpace space, const string &payload, uint64_t pn);
    void handleFrame(QuicPacketNumberSpace space, const QuicFrame &frame);
    void onCryptoFrame(QuicPacketNumberSpace space, const QuicFrame &frame);
    void driveTls();
    void startReader();
    void readerLoop();
    void ptoLoop();
    void setError(QuicConnection::Error e, const string &msg);
    QuicTrafficKeys *keysForSend(QuicPacketNumberSpace space);
    QuicTrafficKeys *keysForRecv(QuicPacketNumberSpace space);
    // Largest PN seen in space, or 0 if none yet (for truncated PN reconstruction / ACK).
    uint64_t largestReceivedPn(QuicPacketNumberSpace space) const;
    bool hasReceivedPn(QuicPacketNumberSpace space) const;
    shared_ptr<QuicStream> getOrCreateStream(uint64_t id);
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
    return isClient ? &clientAppKeys : &serverAppKeys;
}

QuicTrafficKeys *QuicConnectionPrivate::keysForRecv(QuicPacketNumberSpace space)
{
    if (space == QuicPnInitial) {
        return isClient ? &serverInitialKeys : &clientInitialKeys;
    }
    if (space == QuicPnHandshake) {
        return isClient ? &serverHsKeys : &clientHsKeys;
    }
    return isClient ? &serverAppKeys : &clientAppKeys;
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

bool QuicConnectionPrivate::sendPacket(QuicPacketNumberSpace space, const string &payload, bool ackEliciting)
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
        header = quicBuildLongHeader(longType, kQuicVersion1, remoteCid, localCid, string(), pn, pnLen,
                                     payload.size() + 16);
    } else {
        header = quicBuildShortHeader(remoteCid, pn, pnLen, false);
    }
    string packet;
    if (!quicProtectPacket(*keys, header, payload, pn, pnLen, &packet)) {
        return false;
    }
    if (link->sendto(packet.data(), static_cast<int32_t>(packet.size()), peerPath)
        != static_cast<int32_t>(packet.size())) {
        setError(QuicConnection::SocketError, "sendto failed");
        return false;
    }
    QuicSentPacket sent;
    sent.pn = pn;
    sent.space = space;
    sent.raw = packet;
    sent.ackEliciting = ackEliciting;
    sent.inFlight = true;
    recovery.onPacketSent(sent);
    return true;
}

void QuicConnectionPrivate::maybeSendAck(QuicPacketNumberSpace space)
{
    if (!hasReceivedPn(space)) {
        return;
    }
    const uint64_t largest = largestReceivedPn(space);
    QuicFrame ack;
    ack.type = QuicFrame::Ack;
    ack.largestAcknowledged = largest;
    ack.ackDelay = 0;
    ack.firstAckRange = largest;  // acknowledge 0..largest for MVP
    string payload;
    quicEncodeFrame(ack, &payload);
    sendPacket(space, payload, false);
}

bool QuicConnectionPrivate::flushCrypto()
{
    auto sendCrypto = [&](QuicPacketNumberSpace space, string *buf, uint64_t *off) {
        while (!buf->empty()) {
            const size_t chunk = min<size_t>(buf->size(), 1000);
            QuicFrame f;
            f.type = QuicFrame::Crypto;
            f.offset = *off;
            f.data = buf->substr(0, chunk);
            string payload;
            quicEncodeFrame(f, &payload);
            // RFC 9000: UDP datagrams containing client Initial must be >= 1200 bytes.
            // Pad with PADDING frames (0x00) in AEAD plaintext. Long header ~28-40 bytes;
            // use a tight budget so ciphertext+header clears 1200.
            if (space == QuicPnInitial && isClient) {
                const size_t hdrBudget = 28;
                const size_t minPlain = 1200 - hdrBudget - 16;
                while (payload.size() < minPlain) {
                    payload.push_back('\0');
                }
            }
            if (!sendPacket(space, payload, true)) {
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
    return true;
}

shared_ptr<QuicStream> QuicConnectionPrivate::getOrCreateStream(uint64_t id)
{
    auto it = streams.find(id);
    if (it != streams.end()) {
        return it->second;
    }
    QuicStreamPrivate *sd = new QuicStreamPrivate(this, id);
    shared_ptr<QuicStream> s(new QuicStream(sd));
    streams[id] = s;
    return s;
}

void QuicConnectionPrivate::onCryptoFrame(QuicPacketNumberSpace space, const QuicFrame &frame)
{
    string *buf = (space == QuicPnInitial) ? &cryptoRecvInitial : &cryptoRecvHandshake;
    uint64_t *off = (space == QuicPnInitial) ? &cryptoRecvOffInitial : &cryptoRecvOffHandshake;
    if (frame.offset != *off) {
        // MVP: require in-order CRYPTO
        if (frame.offset > *off) {
            return;
        }
    }
    if (frame.offset + frame.data.size() <= *off) {
        return;
    }
    const size_t skip = static_cast<size_t>(*off - frame.offset);
    buf->append(frame.data.substr(skip));
    *off += (frame.data.size() - skip);
    driveTls();
}

void QuicConnectionPrivate::driveTls()
{
    if (!tls) {
        return;
    }
    // Feed all available crypto
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

    string out = tls->takeCryptoToSend();
    if (!out.empty()) {
        // Split: ServerHello (type 2) -> Initial; rest -> Handshake. Client Finished -> Handshake.
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
            if (type == 2 || (type == 1)) {
                cryptoSendInitial.append(msg);
            } else {
                cryptoSendHandshake.append(msg);
            }
        }
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
        state = QuicConnection::ConnectedState;
        q_ptr->handshakeDone()->set();
    }
    flushCrypto();
}

void QuicConnectionPrivate::handleFrame(QuicPacketNumberSpace space, const QuicFrame &frame)
{
    switch (frame.type) {
    case QuicFrame::Ack:
    case QuicFrame::AckEcn: {
        vector<string> lost;
        recovery.onAckReceived(space, frame, &lost);
        for (const string &pkt : lost) {
            link->sendto(pkt.data(), static_cast<int32_t>(pkt.size()), peerPath);
        }
        break;
    }
    case QuicFrame::Crypto:
        onCryptoFrame(space, frame);
        break;
    case QuicFrame::Stream: {
        shared_ptr<QuicStream> s = getOrCreateStream(frame.streamId);
        QuicStreamPrivate *sd = s->d_func();
        // MVP: in-order only; out-of-order STREAM frames are dropped until a real reassembly buffer exists.
        if (frame.offset == sd->recvOffset) {
            sd->recvBuf.append(frame.data);
            sd->recvOffset += frame.data.size();
            if (frame.fin) {
                sd->finReceived = true;
            }
            sd->dataReady.set();
        }
        break;
    }
    case QuicFrame::ConnectionClose:
    case QuicFrame::ConnectionCloseApp:
        setError(QuicConnection::ClosedError, frame.reasonPhrase.empty() ? "connection close" : frame.reasonPhrase);
        closed = true;
        break;
    case QuicFrame::HandshakeDone:
        break;
    case QuicFrame::Ping:
        maybeSendAck(space);
        break;
    case QuicFrame::PathChallenge: {
        // RFC 9000 §8.2: MUST respond with PATH_RESPONSE carrying the same data.
        if (frame.pathData.size() != 8) {
            break;
        }
        QuicFrame resp;
        resp.type = QuicFrame::PathResponse;
        resp.pathData = frame.pathData;
        string payload;
        if (quicEncodeFrame(resp, &payload)) {
            sendPacket(space, payload, true);
        }
        break;
    }
    case QuicFrame::MaxData:
    case QuicFrame::MaxStreamData:
    case QuicFrame::Padding:
    case QuicFrame::PathResponse:
    case QuicFrame::NewConnectionId:
    case QuicFrame::RetireConnectionId:
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
    vector<QuicFrame> frames;
    if (!quicDecodeFrames(payload.data(), payload.size(), &frames)) {
        setError(QuicConnection::ProtocolError, "bad frames");
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
        maybeSendAck(space);
    }
}

void QuicConnectionPrivate::setError(QuicConnection::Error e, const string &msg)
{
    error = e;
    errorString = msg;
}

void QuicConnectionPrivate::startReader()
{
    if (readerStarted) {
        return;
    }
    readerStarted = true;
    reader.reset(Coroutine::spawn([this] { readerLoop(); }));
    ptoTimer.reset(Coroutine::spawn([this] { ptoLoop(); }));
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
            peerPath = who;
            pathToAddr(who, &peerAddr, &peerPort);
        }

        // Ensure Initial keys exist (server learns ODCID from first long-header DCID).
        QuicPacketHeader peek;
        if (quicParsePacketHeader(buf, static_cast<size_t>(n), &peek, true) && peek.isLong
            && peek.longType == QuicLongInitial) {
            if (!clientInitialKeys.valid()) {
                odcid = peek.dcid;
                remoteCid = peek.scid;
                clientInitialKeys = quicDeriveTrafficKeys(quicDeriveInitialSecret(odcid, true));
                serverInitialKeys = quicDeriveTrafficKeys(quicDeriveInitialSecret(odcid, false));
                if (!isClient && !tls) {
                    setupTls(false);
                }
            }
        }

        // UDP datagrams may coalesce multiple QUIC packets.
        size_t offset = 0;
        const size_t datagramLen = static_cast<size_t>(n);
        while (offset < datagramLen) {
            const char *pkt = buf + offset;
            const size_t remain = datagramLen - offset;
            // Trailing padding zeros after the last packet — stop.
            if (static_cast<unsigned char>(pkt[0]) == 0) {
                break;
            }

            QuicPacketHeader peekHdr;
            if (!quicParsePacketHeader(pkt, remain, &peekHdr, true)) {
                break;
            }

            QuicPacketNumberSpace space = QuicPnApplication;
            if (peekHdr.isLong) {
                if (peekHdr.longType == QuicLongInitial) {
                    space = QuicPnInitial;
                } else if (peekHdr.longType == QuicLongHandshake) {
                    space = QuicPnHandshake;
                } else if (peekHdr.longType == QuicLongZeroRtt) {
                    // 0-RTT not supported in MVP
                    break;
                } else {
                    break;
                }
            }

            QuicTrafficKeys *keys = keysForRecv(space);
            if (!keys || !keys->valid()) {
                // Missing keys for this packet: stop walking the datagram.
                // (Caller processes packets in order; HS keys appear only after Initial CRYPTO.)
                break;
            }

            QuicPacketHeader hdr;
            string payload;
            if (space == QuicPnApplication) {
                hdr.dcid = localCid;
            }
            size_t consumed = 0;
            if (!quicUnprotectPacket(*keys, pkt, remain, &hdr, &payload, largestReceivedPn(space), &consumed)
                || consumed == 0) {
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
        Coroutine::sleep(0.2f);
        for (QuicPacketNumberSpace space : {QuicPnInitial, QuicPnHandshake, QuicPnApplication}) {
            if (!recovery.hasInFlight(space)) {
                continue;
            }
            vector<string> pkts = recovery.packetsToRetransmitOnPto(space);
            for (const string &pkt : pkts) {
                link->sendto(pkt.data(), static_cast<int32_t>(pkt.size()), peerPath);
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
    if (d->reader) {
        d->reader->kill();
        d->reader->join();
        d->reader.reset();
    }
    if (d->ptoTimer) {
        d->ptoTimer->kill();
        d->ptoTimer->join();
        d->ptoTimer.reset();
    }
    delete d_ptr;
}

std::shared_ptr<Event> QuicConnection::handshakeDone() const
{
    NG_D(const QuicConnection);
    return d->handshakeDoneEvent;
}

void QuicConnection::setConfiguration(const QuicConfiguration &config)
{
    NG_D(QuicConnection);
    d->config = config;
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
    // wait for handshake
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
    if (d->state != ConnectedState) {
        return shared_ptr<QuicStream>();
    }
    uint64_t id = d->isClient ? d->nextClientBidi : d->nextServerBidi;
    if (d->isClient) {
        d->nextClientBidi += 4;
    } else {
        d->nextServerBidi += 4;
    }
    return d->getOrCreateStream(id);
}

shared_ptr<QuicStream> QuicConnection::acceptStream()
{
    NG_D(QuicConnection);
    // Wait for a peer-initiated stream to appear with data
    float waited = 0;
    while (waited < 5.0f && d->error == NoError) {
        for (auto &kv : d->streams) {
            const bool peerInitiated = d->isClient ? ((kv.first & 1) == 1) : ((kv.first & 1) == 0);
            if (peerInitiated) {
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
        f.errorCode = 0;
        f.frameType = 0;
        f.reasonPhrase = "close";
        string payload;
        quicEncodeFrame(f, &payload);
        d->sendPacket(QuicPnApplication, payload, true);
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
    if (!d->connection->hasAppKeys || d->finSent || d->reset) {
        return -1;
    }
    QuicFrame f;
    f.type = QuicFrame::Stream;
    f.streamId = d->streamId;
    f.offset = d->sendOffset;
    f.data.assign(data, static_cast<size_t>(size));
    f.hasLength = true;
    f.fin = false;
    string payload;
    quicEncodeFrame(f, &payload);
    if (!d->connection->sendPacket(QuicPnApplication, payload, true)) {
        return -1;
    }
    d->sendOffset += static_cast<uint64_t>(size);
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
    if (d->finSent || !d->connection->hasAppKeys) {
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
    d->connection->sendPacket(QuicPnApplication, payload, true);
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
        d->connection->sendPacket(QuicPnApplication, payload, true);
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
