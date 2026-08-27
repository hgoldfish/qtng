#include "qtng/private/stun_p.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <zlib.h>

#include "qtng/md.h"
#include "qtng/random.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

namespace {

const uint32_t kMagicCookie = 0x2112A442;
const char kMagicCookieBytes[4] = { '\x21', '\x12', '\xa4', '\x42' };

uint16_t be16(const char *p)
{
    return static_cast<uint16_t>((static_cast<unsigned char>(p[0]) << 8)
                                 | static_cast<unsigned char>(p[1]));
}

uint32_t be32(const char *p)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24)
            | (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16)
            | (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8)
            | static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

void appendU16be(string *out, uint16_t v)
{
    char b[2];
    ngToBigEndian(v, b);
    out->append(b, 2);
}

void appendU32be(string *out, uint32_t v)
{
    char b[4];
    ngToBigEndian(v, b);
    out->append(b, 4);
}

size_t paddedLen(size_t len)
{
    return (len + 3) & ~size_t(3);
}

double nowSeconds()
{
    return chrono::duration_cast<chrono::duration<double>>(
            chrono::steady_clock::now().time_since_epoch()).count();
}

// RFC 8489 §15.6: reason phrase should be a short ASCII string.
const char *errorReason(int code)
{
    switch (code) {
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 420:
        return "Unknown Attribute";
    case 437:
        return "Allocation Mismatch";
    case 438:
        return "Stale Nonce";
    case 440:
        return "Address Family not Supported";
    case 441:
        return "Wrong Credentials";
    case 442:
        return "Unsupported Transport Protocol";
    case 443:
        return "Peer Address Family Mismatch";
    case 486:
        return "Allocation Quota Reached";
    case 500:
        return "Server Error";
    case 508:
        return "Insufficient Capacity";
    default:
        return "Error";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// StunClientInfo
// ---------------------------------------------------------------------------

StunClientInfo::StunClientInfo()
    : m_ok(false)
    , m_mappedPort(0)
    , m_rtt(0.0f)
    , m_errorCode(0)
{
}

// ---------------------------------------------------------------------------
// StunMessage
// ---------------------------------------------------------------------------

StunMessage::StunMessage()
    : m_method(0)
    , m_class(StunRequestClass)
{
}

string StunMessage::newTransactionId()
{
    return randomBytes(12);
}

uint16_t StunMessage::messageType() const
{
    uint16_t type = static_cast<uint16_t>(((m_method & 0x0f80) << 2) | ((m_method & 0x0070) << 1)
                                          | (m_method & 0x000f));
    if (m_class & 0x1) {
        type |= 0x0010;  // C0
    }
    if (m_class & 0x2) {
        type |= 0x0100;  // C1
    }
    return type;
}

bool StunMessage::parse(const string &packet)
{
    m_attributes.clear();
    if (packet.size() < 20) {
        return false;
    }
    const uint16_t type = be16(packet.data());
    const uint16_t length = be16(packet.data() + 2);
    if (be32(packet.data() + 4) != kMagicCookie) {
        return false;
    }
    if (static_cast<size_t>(length) + 20 != packet.size()) {
        return false;
    }
    m_method = static_cast<uint16_t>((type & 0x000f) | ((type >> 1) & 0x0070) | ((type >> 2) & 0x0f80));
    m_class = static_cast<int>((((type >> 8) & 0x1) << 1) | ((type >> 4) & 0x1));
    m_transactionId.assign(packet.data() + 8, 12);

    size_t offset = 20;
    while (offset < packet.size()) {
        if (offset + 4 > packet.size()) {
            return false;
        }
        const uint16_t atype = be16(packet.data() + offset);
        const uint16_t alen = be16(packet.data() + offset + 2);
        if (offset + 4u + alen > packet.size()) {
            return false;
        }
        StunAttribute a;
        a.type = atype;
        a.value.assign(packet.data() + offset + 4, alen);
        a.offset = offset;
        m_attributes.push_back(a);
        offset += 4u + alen;
        offset += paddedLen(alen) - alen;
    }
    m_raw = packet;
    return true;
}

string StunMessage::encodePrefix(const vector<StunAttribute> &attrs, size_t endIndex,
                                 size_t totalLen) const
{
    string out;
    out.reserve(20 + totalLen);
    appendU16be(&out, messageType());
    appendU16be(&out, static_cast<uint16_t>(totalLen));
    out.append(kMagicCookieBytes, 4);
    out += m_transactionId;
    for (size_t i = 0; i < endIndex && i < attrs.size(); ++i) {
        const StunAttribute &a = attrs[i];
        appendU16be(&out, a.type);
        appendU16be(&out, static_cast<uint16_t>(a.value.size()));
        out += a.value;
        out.append(paddedLen(a.value.size()) - a.value.size(), '\0');
    }
    return out;
}

string StunMessage::encode() const
{
    // Parsed messages keep their exact wire bytes (including padding), so a
    // round-trip is byte-identical. Freshly built messages are serialized here.
    if (!m_raw.empty()) {
        return m_raw;
    }

    vector<StunAttribute> attrs = m_attributes;

    size_t totalLen = 0;
    for (size_t i = 0; i < attrs.size(); ++i) {
        size_t vlen = attrs[i].value.size();
        if (vlen == 0) {
            if (attrs[i].type == StunAttrMessageIntegrity) {
                vlen = 20;
            } else if (attrs[i].type == StunAttrFingerprint) {
                vlen = 4;
            }
        }
        totalLen += 4 + paddedLen(vlen);
    }
    if (totalLen > 0xffff) {
        return string();
    }

    // Placeholder attributes (20 / 4 zero bytes) get filled in place.
    // MESSAGE-INTEGRITY covers the header + attributes before MI with the
    // length field set to the size up to and including the MI attribute
    // (RFC 8489 §14.5 / RFC 5389 §15.4). FINGERPRINT covers everything before
    // it with the length field set to the full message size (RFC 8489 §14.6).
    for (size_t i = 0; i < attrs.size(); ++i) {
        if (attrs[i].type == StunAttrMessageIntegrity && !m_integrityKey.empty()
            && attrs[i].value.size() == 20) {
            size_t miLen = 0;
            for (size_t j = 0; j <= i; ++j) {
                miLen += 4 + paddedLen(attrs[j].value.size());
            }
            attrs[i].value = stunHmacSha1(m_integrityKey, encodePrefix(attrs, i, miLen));
        }
    }
    for (size_t i = 0; i < attrs.size(); ++i) {
        if (attrs[i].type == StunAttrFingerprint && attrs[i].value.size() == 4) {
            attrs[i].value = stunFingerprint(encodePrefix(attrs, i, totalLen));
        }
    }
    return encodePrefix(attrs, attrs.size(), totalLen);
}

const StunAttribute *StunMessage::attribute(uint16_t type) const
{
    for (size_t i = 0; i < m_attributes.size(); ++i) {
        if (m_attributes[i].type == type) {
            return &m_attributes[i];
        }
    }
    return nullptr;
}

void StunMessage::addAttribute(uint16_t type, const string &value)
{
    StunAttribute a;
    a.type = type;
    a.value = value;
    m_attributes.push_back(a);
    m_raw.clear();
}

void StunMessage::removeAttribute(uint16_t type)
{
    for (size_t i = 0; i < m_attributes.size();) {
        if (m_attributes[i].type == type) {
            m_attributes.erase(m_attributes.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    m_raw.clear();
}

bool StunMessage::hasAttribute(uint16_t type) const
{
    return attribute(type) != nullptr;
}

bool StunMessage::verifyIntegrity(const string &key) const
{
    const StunAttribute *mi = attribute(StunAttrMessageIntegrity);
    if (!mi || mi->value.size() != 20 || m_raw.size() < mi->offset + 24) {
        return false;
    }
    // Hash the exact wire bytes up to the MESSAGE-INTEGRITY attribute header.
    // The length field is adjusted to cover the end of the MI attribute
    // (RFC 8489 §14.5 / RFC 5389 §15.4). Padding bytes are preserved.
    string input = m_raw.substr(0, mi->offset);
    if (input.size() < 4) {
        return false;
    }
    // Length field = size of all attributes through the end of MESSAGE-INTEGRITY.
    const size_t miEndLength = mi->offset + 4;
    char lenBytes[2];
    ngToBigEndian(static_cast<uint16_t>(miEndLength), lenBytes);
    input[2] = lenBytes[0];
    input[3] = lenBytes[1];
    return stunHmacSha1(key, input) == mi->value;
}

bool StunMessage::verifyFingerprint() const
{
    const StunAttribute *fp = attribute(StunAttrFingerprint);
    if (!fp || fp->value.size() != 4 || m_raw.size() < fp->offset + 8) {
        return false;
    }
    // CRC over the exact wire bytes up to the FINGERPRINT attribute header
    // (padding preserved); the original length field already covers the whole
    // message including FINGERPRINT (RFC 8489 §14.6).
    return stunFingerprint(m_raw.substr(0, fp->offset)) == fp->value;
}

bool StunMessage::xorAddress(uint16_t attrType, HostAddress *addr, uint16_t *port) const
{
    const StunAttribute *a = attribute(attrType);
    if (!a || a->value.size() < 4) {
        return false;
    }
    const uint8_t family = static_cast<uint8_t>(a->value[1]);
    const uint16_t xport = be16(a->value.data() + 2);
    const uint16_t decodedPort = static_cast<uint16_t>(xport ^ (kMagicCookie >> 16));
    if (family == 1) {  // IPv4
        if (a->value.size() < 8) {
            return false;
        }
        const uint32_t ip = be32(a->value.data() + 4) ^ kMagicCookie;
        if (addr) {
            addr->setAddress(ip);
        }
        if (port) {
            *port = decodedPort;
        }
        return true;
    } else if (family == 2) {  // IPv6
        if (a->value.size() < 20 || m_transactionId.size() != 12) {
            return false;
        }
        IPv6Address ipv6;
        for (int i = 0; i < 4; ++i) {
            ipv6[i] = static_cast<uint8_t>(a->value[4 + i]
                                           ^ static_cast<uint8_t>((kMagicCookie >> (24 - i * 8)) & 0xff));
        }
        for (int i = 0; i < 12; ++i) {
            ipv6[4 + i] = static_cast<uint8_t>(a->value[8 + i]
                                               ^ static_cast<uint8_t>(m_transactionId[static_cast<size_t>(i)]));
        }
        if (addr) {
            addr->setAddress(ipv6);
        }
        if (port) {
            *port = decodedPort;
        }
        return true;
    }
    return false;
}

void StunMessage::setXorAddress(uint16_t attrType, const HostAddress &addr, uint16_t port)
{
    string value;
    value.append(1, '\0');
    if (addr.isIPv4()) {
        value.append(1, '\x01');
        appendU16be(&value, static_cast<uint16_t>(port ^ (kMagicCookie >> 16)));
        bool ok = false;
        const uint32_t ip = addr.toIPv4Address(&ok);
        appendU32be(&value, ok ? (ip ^ kMagicCookie) : 0u);
    } else {
        value.append(1, '\x02');
        appendU16be(&value, static_cast<uint16_t>(port ^ (kMagicCookie >> 16)));
        const IPv6Address ipv6 = addr.toIPv6Address();
        for (int i = 0; i < 4; ++i) {
            value.append(1, static_cast<char>(ipv6[i]
                                              ^ static_cast<uint8_t>((kMagicCookie >> (24 - i * 8)) & 0xff)));
        }
        for (int i = 0; i < 12; ++i) {
            if (m_transactionId.size() == 12) {
                value.append(1, static_cast<char>(ipv6[4 + i]
                                                  ^ static_cast<uint8_t>(m_transactionId[static_cast<size_t>(i)])));
            } else {
                value.append(1, '\0');
            }
        }
    }
    removeAttribute(attrType);
    addAttribute(attrType, value);
}

bool StunMessage::xorMappedAddress(HostAddress *addr, uint16_t *port) const
{
    return xorAddress(StunAttrXorMappedAddress, addr, port);
}

void StunMessage::setXorMappedAddress(const HostAddress &addr, uint16_t port)
{
    setXorAddress(StunAttrXorMappedAddress, addr, port);
}

bool StunMessage::channelNumber(uint16_t *channel) const
{
    const StunAttribute *a = attribute(StunAttrChannelNumber);
    if (!a || a->value.size() < 2) {
        return false;
    }
    if (channel) {
        *channel = be16(a->value.data());
    }
    return true;
}

void StunMessage::setChannelNumber(uint16_t channel)
{
    string value(4, '\0');
    value[0] = static_cast<char>((channel >> 8) & 0xff);
    value[1] = static_cast<char>(channel & 0xff);
    removeAttribute(StunAttrChannelNumber);
    addAttribute(StunAttrChannelNumber, value);
}

bool StunMessage::lifetime(uint32_t *secs) const
{
    const StunAttribute *a = attribute(StunAttrLifetime);
    if (!a || a->value.size() < 4) {
        return false;
    }
    if (secs) {
        *secs = be32(a->value.data());
    }
    return true;
}

void StunMessage::setLifetime(uint32_t secs)
{
    string value(4, '\0');
    ngToBigEndian(secs, &value[0]);
    removeAttribute(StunAttrLifetime);
    addAttribute(StunAttrLifetime, value);
}

bool StunMessage::requestedTransport(uint8_t *proto) const
{
    const StunAttribute *a = attribute(StunAttrRequestedTransport);
    if (!a || a->value.size() < 1) {
        return false;
    }
    if (proto) {
        *proto = static_cast<uint8_t>(a->value[0]);
    }
    return true;
}

void StunMessage::setRequestedTransport(uint8_t proto)
{
    string value(4, '\0');
    value[0] = static_cast<char>(proto);
    removeAttribute(StunAttrRequestedTransport);
    addAttribute(StunAttrRequestedTransport, value);
}

bool StunMessage::mappedAddress(HostAddress *addr, uint16_t *port) const
{
    const StunAttribute *a = attribute(StunAttrMappedAddress);
    if (!a || a->value.size() < 4) {
        return false;
    }
    const uint8_t family = static_cast<uint8_t>(a->value[1]);
    const uint16_t decodedPort = be16(a->value.data() + 2);
    if (family == 1) {
        if (a->value.size() < 8) {
            return false;
        }
        if (addr) {
            addr->setAddress(be32(a->value.data() + 4));
        }
        if (port) {
            *port = decodedPort;
        }
        return true;
    } else if (family == 2) {
        if (a->value.size() < 20) {
            return false;
        }
        IPv6Address ipv6;
        memcpy(ipv6.data(), a->value.data() + 4, 16);
        if (addr) {
            addr->setAddress(ipv6);
        }
        if (port) {
            *port = decodedPort;
        }
        return true;
    }
    return false;
}

void StunMessage::setMappedAddress(const HostAddress &addr, uint16_t port)
{
    string value;
    value.append(1, '\0');
    if (addr.isIPv4()) {
        value.append(1, '\x01');
        appendU16be(&value, port);
        bool ok = false;
        appendU32be(&value, addr.toIPv4Address(&ok));
    } else {
        value.append(1, '\x02');
        appendU16be(&value, port);
        const IPv6Address ipv6 = addr.toIPv6Address();
        value.append(reinterpret_cast<const char *>(ipv6.data()), 16);
    }
    removeAttribute(StunAttrMappedAddress);
    addAttribute(StunAttrMappedAddress, value);
}

bool StunMessage::errorCode(int *code, string *reason) const
{
    const StunAttribute *a = attribute(StunAttrErrorCode);
    if (!a || a->value.size() < 4) {
        return false;
    }
    const int c = static_cast<int>(static_cast<unsigned char>(a->value[2])) * 100
            + static_cast<int>(static_cast<unsigned char>(a->value[3]));
    if (c < 300 || c > 699) {
        return false;
    }
    if (code) {
        *code = c;
    }
    if (reason) {
        reason->assign(a->value.data() + 4, a->value.size() - 4);
    }
    return true;
}

void StunMessage::setErrorCode(int code, const string &reason)
{
    string value(4, '\0');
    value[2] = static_cast<char>((code / 100) & 0x7);
    value[3] = static_cast<char>(code % 100);
    value += reason.empty() ? errorReason(code) : reason;
    removeAttribute(StunAttrErrorCode);
    addAttribute(StunAttrErrorCode, value);
}

string StunMessage::toString() const
{
    ostringstream oss;
    oss << "StunMessage{method=0x" << hex << m_method << dec << " class=" << m_class
        << " txn=" << utils::bytesToHex(m_transactionId);
    for (size_t i = 0; i < m_attributes.size(); ++i) {
        oss << " attr=0x" << hex << m_attributes[i].type << dec << "(len=" << m_attributes[i].value.size()
            << ")";
    }
    oss << "}";
    return oss.str();
}

bool StunMessage::isChannelData(const string &packet)
{
    if (packet.size() < 4) {
        return false;
    }
    const uint16_t first = be16(packet.data());
    return first >= 0x4000 && first <= 0x7fff;
}

bool StunMessage::parseChannelData(const string &packet, ChannelDataFrame *frame)
{
    if (packet.size() < 4) {
        return false;
    }
    const uint16_t channel = be16(packet.data());
    if (channel < 0x4000 || channel > 0x7fff) {
        return false;
    }
    const uint16_t len = be16(packet.data() + 2);
    if (packet.size() != 4u + len) {
        return false;
    }
    frame->channel = channel;
    frame->data.assign(packet.data() + 4, len);
    return true;
}

string StunMessage::encodeChannelData(uint16_t channel, const string &data)
{
    string out;
    appendU16be(&out, channel);
    appendU16be(&out, static_cast<uint16_t>(data.size()));
    out += data;
    return out;
}

// ---------------------------------------------------------------------------
// Cryptographic helpers
// ---------------------------------------------------------------------------

string stunHmacSha1(const string &key, const string &data)
{
    const size_t blockSize = 64;
    string k(key);
    if (k.size() > blockSize) {
        k = MessageDigest::digest(k, MessageDigest::Sha1);
    }
    if (k.size() < blockSize) {
        k.append(blockSize - k.size(), '\0');
    }
    string innerPad(blockSize, '\x36');
    string outerPad(blockSize, '\x5c');
    for (size_t i = 0; i < blockSize; ++i) {
        innerPad[i] = static_cast<char>(static_cast<unsigned char>(innerPad[i])
                                        ^ static_cast<unsigned char>(k[i]));
        outerPad[i] = static_cast<char>(static_cast<unsigned char>(outerPad[i])
                                        ^ static_cast<unsigned char>(k[i]));
    }
    const string inner = MessageDigest::digest(innerPad + data, MessageDigest::Sha1);
    return MessageDigest::digest(outerPad + inner, MessageDigest::Sha1);
}

string stunLongTermKey(const string &username, const string &realm, const string &password)
{
    return MessageDigest::digest(username + ":" + realm + ":" + password, MessageDigest::Md5);
}

string stunFingerprint(const string &data)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(data.data()), static_cast<uInt>(data.size()));
    const uint32_t value = static_cast<uint32_t>(crc) ^ 0x5354554e;
    string out(4, '\0');
    ngToBigEndian(value, &out[0]);
    return out;
}

// ---------------------------------------------------------------------------
// StunClient
// ---------------------------------------------------------------------------

StunClient::StunClient(HostAddress::NetworkLayerProtocol proto)
    : d_ptr(new StunClientPrivate(this, proto))
{
}

StunClient::~StunClient()
{
    close();
    delete d_ptr;
}

bool StunClient::open()
{
    NG_D(StunClient);
    return d->open();
}

void StunClient::close()
{
    NG_D(StunClient);
    d->close();
}

bool StunClient::isOpen() const
{
    NG_D(const StunClient);
    return d->opened;
}

HostAddress StunClient::localAddress() const
{
    NG_D(const StunClient);
    return d->socket ? d->socket->localAddress() : HostAddress();
}

uint16_t StunClient::localPort() const
{
    NG_D(const StunClient);
    return d->socket ? d->socket->localPort() : 0;
}

StunClientInfo StunClient::query(const HostAddress &server, uint16_t port, float timeoutSecs)
{
    NG_D(StunClient);
    return d->query(server, port, timeoutSecs);
}

StunClientInfo StunClient::query(const string &server, uint16_t port, float timeoutSecs)
{
    NG_D(StunClient);
    return d->query(server, port, timeoutSecs);
}

string StunClient::errorString() const
{
    NG_D(const StunClient);
    return d->error;
}

StunClientPrivate::StunClientPrivate(StunClient *q, HostAddress::NetworkLayerProtocol p)
    : q_ptr(q)
    , proto(p)
    , opened(false)
    , softwareName("qtng STUN client")
{
}

StunClientPrivate::~StunClientPrivate() { }

bool StunClientPrivate::open()
{
    if (opened) {
        return true;
    }
    socket = make_shared<Socket>(proto, Socket::UdpSocket);
    const HostAddress bindAddr = (proto == HostAddress::IPv6Protocol) ? HostAddress::AnyIPv6
                                                                      : HostAddress::AnyIPv4;
    if (!socket->bind(bindAddr, 0)) {
        error = "UDP bind failed: " + socket->errorString();
        socket.reset();
        return false;
    }
    workers = make_unique<CoroutineGroup>();
    workers->spawnWithName("stun-client-recv", [this] { recvLoop(); });
    opened = true;
    return true;
}

void StunClientPrivate::close()
{
    if (!opened && !socket) {
        return;
    }
    opened = false;
    if (socket) {
        socket->abort();
    }
    if (workers) {
        workers->killall(true);
        workers.reset();
    }
    {
        waitersLock.tryAcquire();
        waiters.clear();
        waitersLock.release();
    }
    socket.reset();
}

void StunClientPrivate::recvLoop()
{
    while (opened && socket) {
        HostAddress addr;
        uint16_t port = 0;
        string data = socket->recvfrom(65535, &addr, &port);
        if (data.empty()) {
            if (!opened) {
                break;
            }
            Coroutine::msleep(10);
            continue;
        }
        StunMessage msg;
        if (!msg.parse(data)) {
            continue;
        }
        if (msg.messageClass() != StunSuccessClass && msg.messageClass() != StunErrorClass) {
            continue;
        }
        StunReply reply;
        reply.ok = true;
        reply.success = (msg.messageClass() == StunSuccessClass);
        reply.message = msg;
        reply.fromAddress = addr;
        reply.fromPort = port;
        waitersLock.tryAcquire();
        map<string, shared_ptr<ValueEvent<StunReply>>>::iterator it = waiters.find(msg.transactionId());
        if (it != waiters.end()) {
            it->second->send(reply);
        }
        waitersLock.release();
    }
}

StunClientInfo StunClientPrivate::query(const HostAddress &server, uint16_t port, float timeoutSecs)
{
    StunClientInfo info;
    if (!opened || !socket) {
        info.setOk(false);
        info.setErrorString("client is not open");
        return info;
    }
    if (server.isNull() || port == 0) {
        info.setOk(false);
        info.setErrorString("invalid server address");
        return info;
    }

    StunMessage msg;
    msg.setMethod(StunBindingMethod);
    msg.setMessageClass(StunRequestClass);
    msg.setTransactionId(StunMessage::newTransactionId());
    msg.addAttribute(StunAttrSoftware, softwareName);

    shared_ptr<ValueEvent<StunReply>> waiter = make_shared<ValueEvent<StunReply>>();
    {
        waitersLock.tryAcquire();
        waiters[msg.transactionId()] = waiter;
        waitersLock.release();
    }

    const double t0 = nowSeconds();
    const string packet = msg.encode();
    if (socket->sendto(packet, server, port) <= 0) {
        waitersLock.tryAcquire();
        waiters.erase(msg.transactionId());
        waitersLock.release();
        info.setOk(false);
        info.setErrorString("send failed: " + socket->errorString());
        return info;
    }

    StunReply reply;
    try {
        Timeout timeout(timeoutSecs);
        (void) timeout;
        reply = waiter->tryWait(static_cast<uint32_t>(timeoutSecs * 1000));
    } catch (TimeoutException &) {
        reply.localError = "timeout";
    }

    waitersLock.tryAcquire();
    waiters.erase(msg.transactionId());
    waitersLock.release();

    if (!reply.ok) {
        info.setOk(false);
        info.setErrorString(reply.localError.empty() ? "timeout" : reply.localError);
        return info;
    }

    info.setRtt(static_cast<float>(nowSeconds() - t0));
    if (reply.success) {
        HostAddress mapped;
        uint16_t mappedPort = 0;
        if (reply.message.xorMappedAddress(&mapped, &mappedPort)) {
            info.setMappedAddress(mapped);
            info.setMappedPort(mappedPort);
        } else if (reply.message.mappedAddress(&mapped, &mappedPort)) {
            info.setMappedAddress(mapped);
            info.setMappedPort(mappedPort);
        }
        const StunAttribute *sw = reply.message.attribute(StunAttrSoftware);
        if (sw) {
            string swv = sw->value;
            while (!swv.empty() && swv.back() == '\0') {
                swv.pop_back();
            }
            info.setSoftware(swv);
        }
        info.setOk(true);
    } else {
        int code = 0;
        string reason;
        if (reply.message.errorCode(&code, &reason)) {
            info.setErrorCode(code);
            info.setErrorString(reason);
        } else {
            info.setErrorString("STUN error response");
        }
        info.setOk(false);
    }
    return info;
}

StunClientInfo StunClientPrivate::query(const string &server, uint16_t port, float timeoutSecs)
{
    HostAddress addr;
    if (addr.setAddress(server)) {
        return query(addr, port, timeoutSecs);
    }
    vector<HostAddress> resolved = Socket::resolve(server);
    for (size_t i = 0; i < resolved.size(); ++i) {
        StunClientInfo info = query(resolved[i], port, timeoutSecs);
        if (info.ok()) {
            return info;
        }
    }
    StunClientInfo info;
    info.setOk(false);
    info.setErrorString("cannot resolve server: " + server);
    return info;
}

// ---------------------------------------------------------------------------
// StunServer
// ---------------------------------------------------------------------------

StunServer::StunServer(HostAddress::NetworkLayerProtocol proto)
    : d_ptr(new StunServerPrivate(this, proto))
{
}

StunServer::~StunServer()
{
    close();
    delete d_ptr;
}

bool StunServer::open(const HostAddress &addr, uint16_t port)
{
    NG_D(StunServer);
    return d->open(addr, port);
}

bool StunServer::open(uint16_t port)
{
    NG_D(StunServer);
    const HostAddress addr =
            (d->proto == HostAddress::IPv6Protocol) ? HostAddress::AnyIPv6 : HostAddress::AnyIPv4;
    return d->open(addr, port);
}

void StunServer::close()
{
    NG_D(StunServer);
    d->close();
}

bool StunServer::isOpen() const
{
    NG_D(const StunServer);
    return d->opened;
}

HostAddress StunServer::localAddress() const
{
    NG_D(const StunServer);
    return d->socket ? d->socket->localAddress() : HostAddress();
}

uint16_t StunServer::localPort() const
{
    NG_D(const StunServer);
    return d->socket ? d->socket->localPort() : 0;
}

string StunServer::errorString() const
{
    NG_D(const StunServer);
    return d->error;
}

StunServerPrivate::StunServerPrivate(StunServer *q, HostAddress::NetworkLayerProtocol p)
    : q_ptr(q)
    , proto(p)
    , opened(false)
    , softwareName("qtng STUN server")
{
}

StunServerPrivate::~StunServerPrivate() { }

bool StunServerPrivate::open(const HostAddress &addr, uint16_t port)
{
    if (opened) {
        return true;
    }
    socket = make_shared<Socket>(proto, Socket::UdpSocket);
    socket->setOption(Socket::AddressReusable, true);
    if (!socket->bind(addr, port)) {
        error = "UDP bind failed: " + socket->errorString();
        socket.reset();
        return false;
    }
    workers = make_unique<CoroutineGroup>();
    workers->spawnWithName("stun-server-recv", [this] { recvLoop(); });
    opened = true;
    return true;
}

void StunServerPrivate::close()
{
    if (!opened && !socket) {
        return;
    }
    opened = false;
    if (socket) {
        socket->abort();
    }
    if (workers) {
        workers->killall(true);
        workers.reset();
    }
    socket.reset();
}

void StunServerPrivate::recvLoop()
{
    while (opened && socket) {
        HostAddress addr;
        uint16_t port = 0;
        string data = socket->recvfrom(65535, &addr, &port);
        if (data.empty()) {
            if (!opened) {
                break;
            }
            Coroutine::msleep(10);
            continue;
        }
        handlePacket(data, addr, port);
    }
}

void StunServerPrivate::handlePacket(const string &data, const HostAddress &addr, uint16_t port)
{
    StunMessage msg;
    if (!msg.parse(data)) {
        return;
    }
    if (msg.messageClass() != StunRequestClass) {
        return;
    }
    handleRequest(msg, addr, port);
}

void StunServerPrivate::handleRequest(const StunMessage &msg, const HostAddress &addr, uint16_t port)
{
    StunMessage resp;
    resp.setTransactionId(msg.transactionId());
    if (msg.method() != StunBindingMethod) {
        resp.setMethod(msg.method());
        resp.setMessageClass(StunErrorClass);
        resp.setErrorCode(400, "Bad Request");
    } else {
        resp.setMethod(StunBindingMethod);
        resp.setMessageClass(StunSuccessClass);
        resp.addAttribute(StunAttrSoftware, softwareName);
        resp.setXorMappedAddress(addr, port);
    }
    const string packet = resp.encode();
    if (!packet.empty()) {
        socket->sendto(packet, addr, port);
    }
}

}  // namespace qtng
