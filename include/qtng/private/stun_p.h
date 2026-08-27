#ifndef QTNG_STUN_P_H
#define QTNG_STUN_P_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/coroutine_utils.h"
#include "qtng/locks.h"
#include "qtng/socket.h"
#include "qtng/stun.h"

namespace qtng {

// STUN methods (RFC 8489 §6 / RFC 8656 §6).
enum StunMethod {
    StunBindingMethod = 0x001,
    StunAllocateMethod = 0x003,
    StunRefreshMethod = 0x004,
    StunSendMethod = 0x006,
    StunDataMethod = 0x007,
    StunCreatePermissionMethod = 0x008,
    StunChannelBindMethod = 0x009,
};

// STUN attributes used by STUN and TURN.
enum StunAttributeType {
    StunAttrMappedAddress = 0x0001,
    StunAttrUsername = 0x0006,
    StunAttrMessageIntegrity = 0x0008,
    StunAttrErrorCode = 0x0009,
    StunAttrUnknownAttributes = 0x000A,
    StunAttrChannelNumber = 0x000C,
    StunAttrLifetime = 0x000D,
    StunAttrXorPeerAddress = 0x0012,
    StunAttrData = 0x0013,
    StunAttrRealm = 0x0014,
    StunAttrNonce = 0x0015,
    StunAttrXorRelayedAddress = 0x0016,
    StunAttrRequestedAddressFamily = 0x0017,
    StunAttrEvenPort = 0x0018,
    StunAttrRequestedTransport = 0x0019,
    StunAttrXorMappedAddress = 0x0020,
    StunAttrReservationToken = 0x0022,
    StunAttrPriority = 0x0024,
    StunAttrUseCandidate = 0x0025,
    StunAttrSoftware = 0x8022,
    StunAttrFingerprint = 0x8028,
    StunAttrIceControlled = 0x8029,
    StunAttrIceControlling = 0x802A,
};

// STUN message class (RFC 8489 §6).
enum StunMessageClass {
    StunRequestClass = 0x00,
    StunIndicationClass = 0x01,
    StunSuccessClass = 0x02,
    StunErrorClass = 0x03,
};

// TURN ChannelData frame (RFC 8656 §11).
struct ChannelDataFrame
{
    ChannelDataFrame()
        : channel(0)
    {
    }
    std::uint16_t channel;
    std::string data;
};

struct StunAttribute
{
    StunAttribute()
        : type(0)
        , offset(0)
    {
    }
    std::uint16_t type;
    std::string value;  // raw attribute payload (without the 4-byte header)
    std::size_t offset;  // byte offset of the attribute header inside the message
};

// RFC 8489 STUN message. Header + TLV attributes with XOR-MAPPED-ADDRESS /
// MESSAGE-INTEGRITY / FINGERPRINT support. parse() never throws.
class StunMessage
{
public:
    StunMessage();

    bool parse(const std::string &packet);
    std::string encode() const;

    std::uint16_t method() const { return m_method; }
    void setMethod(std::uint16_t method)
    {
        m_method = method;
        m_raw.clear();
    }
    int messageClass() const { return m_class; }
    void setMessageClass(int messageClass)
    {
        m_class = messageClass;
        m_raw.clear();
    }
    std::string transactionId() const { return m_transactionId; }
    void setTransactionId(const std::string &tid)
    {
        m_transactionId = tid;
        m_raw.clear();
    }

    const std::vector<StunAttribute> &attributes() const { return m_attributes; }
    std::vector<StunAttribute> &attributes() { return m_attributes; }
    const StunAttribute *attribute(std::uint16_t type) const;
    void addAttribute(std::uint16_t type, const std::string &value);
    void removeAttribute(std::uint16_t type);
    bool hasAttribute(std::uint16_t type) const;

    // Key used to fill MESSAGE-INTEGRITY on encode(). Empty means no integrity.
    std::string integrityKey() const { return m_integrityKey; }
    void setIntegrityKey(const std::string &key) { m_integrityKey = key; }

    bool hasMessageIntegrity() const { return hasAttribute(StunAttrMessageIntegrity); }
    bool hasFingerprint() const { return hasAttribute(StunAttrFingerprint); }
    bool verifyIntegrity(const std::string &key) const;
    bool verifyFingerprint() const;

    // XOR-MAPPED-ADDRESS helpers (RFC 8489 §15.2). Returns false when absent or
    // malformed.
    bool xorMappedAddress(HostAddress *addr, std::uint16_t *port) const;
    void setXorMappedAddress(const HostAddress &addr, std::uint16_t port);
    bool mappedAddress(HostAddress *addr, std::uint16_t *port) const;
    void setMappedAddress(const HostAddress &addr, std::uint16_t port);

    // Generic XOR-address helpers shared by MAPPED/RELAYED/PEER attributes.
    bool xorAddress(std::uint16_t attrType, HostAddress *addr, std::uint16_t *port) const;
    void setXorAddress(std::uint16_t attrType, const HostAddress &addr, std::uint16_t port);

    // TURN convenience wrappers (RFC 8656).
    bool xorRelayedAddress(HostAddress *addr, std::uint16_t *port) const { return xorAddress(StunAttrXorRelayedAddress, addr, port); }
    void setXorRelayedAddress(const HostAddress &addr, std::uint16_t port) { setXorAddress(StunAttrXorRelayedAddress, addr, port); }
    bool xorPeerAddress(HostAddress *addr, std::uint16_t *port) const { return xorAddress(StunAttrXorPeerAddress, addr, port); }
    void setXorPeerAddress(const HostAddress &addr, std::uint16_t port) { setXorAddress(StunAttrXorPeerAddress, addr, port); }

    // CHANNEL-NUMBER (2-byte channel + 2-byte RFFU).
    bool channelNumber(std::uint16_t *channel) const;
    void setChannelNumber(std::uint16_t channel);
    // LIFETIME (4-byte big-endian seconds).
    bool lifetime(std::uint32_t *secs) const;
    void setLifetime(std::uint32_t secs);
    // REQUESTED-TRANSPORT (1-byte protocol + 3-byte RFFU).
    bool requestedTransport(std::uint8_t *proto) const;
    void setRequestedTransport(std::uint8_t proto);

    // ERROR-CODE helpers (RFC 8489 §15.6): code 300..699.
    bool errorCode(int *code, std::string *reason) const;
    void setErrorCode(int code, const std::string &reason);

    std::string toString() const;

    static std::string newTransactionId();

    static bool isChannelData(const std::string &packet);
    static bool parseChannelData(const std::string &packet, ChannelDataFrame *frame);
    static std::string encodeChannelData(std::uint16_t channel, const std::string &data);

private:
    std::uint16_t messageType() const;
    std::string encodePrefix(const std::vector<StunAttribute> &attrs, std::size_t endIndex,
                             std::size_t totalLen) const;

    std::uint16_t m_method;
    int m_class;
    std::string m_transactionId;
    std::vector<StunAttribute> m_attributes;
    std::string m_integrityKey;
    std::string m_raw;  // exact wire bytes from the last parse(); used for verification
};

// RFC 2104 HMAC-SHA1 built on MessageDigest::Sha1. Works with the software
// MessageDigest fallback, so QTNG_NO_CRYPTO builds keep full STUN/TURN auth.
std::string stunHmacSha1(const std::string &key, const std::string &data);
// RFC 8489 long-term credential key: MD5(username ":" realm ":" password).
std::string stunLongTermKey(const std::string &username, const std::string &realm,
                            const std::string &password);
// RFC 8489 §15.5: crc32(data) XOR 0x5354554e, as 4 big-endian bytes.
std::string stunFingerprint(const std::string &data);

struct StunReply
{
    bool ok;        // a matching response arrived
    bool success;   // response class is success (not error)
    StunMessage message;
    HostAddress fromAddress;
    std::uint16_t fromPort;
    std::string localError;  // transport errors: "timeout", "not open", ...

    StunReply()
        : ok(false)
        , success(false)
        , fromPort(0)
    {
    }
};

class StunClientPrivate
{
public:
    StunClientPrivate(StunClient *q, HostAddress::NetworkLayerProtocol proto);
    ~StunClientPrivate();

    bool open();
    void close();

    StunClientInfo query(const HostAddress &server, std::uint16_t port, float timeoutSecs);
    StunClientInfo query(const std::string &server, std::uint16_t port, float timeoutSecs);

    void recvLoop();

    StunClient *q_ptr;
    HostAddress::NetworkLayerProtocol proto;
    std::shared_ptr<Socket> socket;
    std::unique_ptr<CoroutineGroup> workers;
    std::map<std::string, std::shared_ptr<ValueEvent<StunReply>>> waiters;
    RLock waitersLock;
    bool opened;
    std::string error;
    std::string softwareName;
};

class StunServerPrivate
{
public:
    StunServerPrivate(StunServer *q, HostAddress::NetworkLayerProtocol proto);
    ~StunServerPrivate();

    bool open(const HostAddress &addr, std::uint16_t port);
    void close();

    void recvLoop();
    void handlePacket(const std::string &data, const HostAddress &addr, std::uint16_t port);
    void handleRequest(const StunMessage &msg, const HostAddress &addr, std::uint16_t port);

    StunServer *q_ptr;
    HostAddress::NetworkLayerProtocol proto;
    std::shared_ptr<Socket> socket;
    std::unique_ptr<CoroutineGroup> workers;
    bool opened;
    std::string error;
    std::string softwareName;
};

}  // namespace qtng

#endif  // QTNG_STUN_P_H
