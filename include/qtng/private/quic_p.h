#ifndef QTNG_QUIC_P_H
#define QTNG_QUIC_P_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/aead.h"
#include "qtng/private/kcp.h"
#include "qtng/quic.h"

namespace qtng {

enum QuicPacketNumberSpace {
    QuicPnInitial = 0,
    QuicPnHandshake = 1,
    QuicPnApplication = 2,
};

enum QuicLongPacketType : std::uint8_t {
    QuicLongInitial = 0,
    QuicLongZeroRtt = 1,
    QuicLongHandshake = 2,
    QuicLongRetry = 3,
};

struct QuicConnectionId {
    std::string bytes;
    bool empty() const { return bytes.empty(); }
    bool operator==(const QuicConnectionId &o) const { return bytes == o.bytes; }
};

struct QuicPacketHeader {
    bool isLong = false;
    QuicLongPacketType longType = QuicLongInitial;
    std::uint32_t version = 0;
    QuicConnectionId dcid;
    QuicConnectionId scid;
    std::string token;  // Initial only
    std::uint64_t packetNumber = 0;
    int pnLength = 0;  // 1..4
    std::size_t headerLength = 0;  // unprotected header length including PN
    std::size_t pnOffset = 0;
    // Long-header Length field: packet_number + AEAD ciphertext (payload || tag).
    std::size_t payloadLength = 0;
};

struct QuicAckRange {
    std::uint64_t gap = 0;
    std::uint64_t ackRangeLength = 0;
};

struct QuicFrame {
    enum Type {
        Padding = 0x00,
        Ping = 0x01,
        Ack = 0x02,
        AckEcn = 0x03,
        ResetStream = 0x04,
        StopSending = 0x05,
        Crypto = 0x06,
        NewToken = 0x07,
        Stream = 0x08,  // 0x08-0x0f
        MaxData = 0x10,
        MaxStreamData = 0x11,
        MaxStreamsBidi = 0x12,
        MaxStreamsUni = 0x13,
        DataBlocked = 0x14,
        StreamDataBlocked = 0x15,
        StreamsBlockedBidi = 0x16,
        StreamsBlockedUni = 0x17,
        NewConnectionId = 0x18,
        RetireConnectionId = 0x19,
        PathChallenge = 0x1a,
        PathResponse = 0x1b,
        ConnectionClose = 0x1c,
        ConnectionCloseApp = 0x1d,
        HandshakeDone = 0x1e,
        Unknown = 0xff,
    };

    Type type = Unknown;
    std::uint8_t rawType = 0;

    // ACK
    std::uint64_t largestAcknowledged = 0;
    std::uint64_t ackDelay = 0;
    std::uint64_t firstAckRange = 0;
    std::vector<QuicAckRange> ackRanges;

    // CRYPTO / STREAM
    std::uint64_t offset = 0;
    std::string data;
    std::uint64_t streamId = 0;
    bool fin = false;
    bool hasLength = false;

    // RESET_STREAM / STOP_SENDING
    std::uint64_t applicationErrorCode = 0;
    std::uint64_t finalSize = 0;

    // MAX_DATA / MAX_STREAM_DATA / MAX_STREAMS* / *BLOCKED limit fields
    std::uint64_t maxData = 0;

    // CONNECTION_CLOSE
    std::uint64_t errorCode = 0;
    std::uint64_t frameType = 0;
    std::string reasonPhrase;

    // PATH_CHALLENGE / PATH_RESPONSE
    std::string pathData;  // 8 bytes

    // NEW_CONNECTION_ID
    std::uint64_t sequenceNumber = 0;
    std::uint64_t retirePriorTo = 0;
    QuicConnectionId connectionId;
    std::string statelessResetToken;  // 16 bytes
};

// --- varint ---
bool quicEncodeVarint(std::uint64_t value, std::string *out);
bool quicDecodeVarint(const char *data, std::size_t size, std::size_t *consumed, std::uint64_t *value);
std::size_t quicVarintSize(std::uint64_t value);

// --- frames ---
bool quicEncodeFrame(const QuicFrame &frame, std::string *out);
bool quicDecodeFrames(const char *data, std::size_t size, std::vector<QuicFrame> *frames);

// --- packets ---
bool quicParsePacketHeader(const char *data, std::size_t size, QuicPacketHeader *header, bool skipPn = false);
std::string quicBuildLongHeader(QuicLongPacketType type, std::uint32_t version, const QuicConnectionId &dcid,
                                const QuicConnectionId &scid, const std::string &token, std::uint64_t packetNumber,
                                int pnLength, std::size_t payloadAndTagLen);
std::string quicBuildShortHeader(const QuicConnectionId &dcid, std::uint64_t packetNumber, int pnLength,
                                 bool keyPhase = false);

struct QuicTrafficKeys {
    std::string key;
    std::string iv;
    std::string hp;
    bool valid() const { return key.size() == 16 && iv.size() == 12 && hp.size() == 16; }
};

QuicTrafficKeys quicDeriveTrafficKeys(const std::string &secret);
std::string quicInitialSalt();
std::string quicDeriveInitialSecret(const QuicConnectionId &dcid, bool isClient);

std::string quicNonceFromIv(const std::string &iv, std::uint64_t packetNumber);
bool quicProtectPacket(const QuicTrafficKeys &keys, const std::string &header, const std::string &payload,
                       std::uint64_t packetNumber, int pnLength, std::string *outPacket);
// expectedPn: largest received PN in this space (0 if none); used for truncated PN reconstruction.
// On success, *consumed is set to the number of bytes of this QUIC packet (for coalescing).
bool quicUnprotectPacket(const QuicTrafficKeys &keys, const char *data, std::size_t size, QuicPacketHeader *header,
                         std::string *payload, std::uint64_t expectedPn = 0, std::size_t *consumed = nullptr);
std::uint64_t quicDecodePacketNumber(std::uint64_t largestPn, std::uint64_t truncated, int pnLength);

// TLS (minimal)
class QuicTlsHandshake;
struct QuicTlsSecrets {
    std::string clientHandshakeSecret;
    std::string serverHandshakeSecret;
    std::string clientAppSecret;
    std::string serverAppSecret;
    bool handshakeReady = false;
    bool appReady = false;
};

struct QuicSentPacket {
    std::uint64_t pn = 0;
    QuicPacketNumberSpace space = QuicPnInitial;
    std::string raw;
    bool ackEliciting = true;
    bool inFlight = true;
};

class QuicLossRecovery
{
public:
    void onPacketSent(const QuicSentPacket &pkt);
    void onAckReceived(QuicPacketNumberSpace space, const QuicFrame &ack, std::vector<std::string> *lostPackets);
    std::vector<std::string> packetsToRetransmitOnPto(QuicPacketNumberSpace space) const;
    std::size_t bytesInFlight() const;
    bool hasInFlight(QuicPacketNumberSpace space) const;
private:
    std::map<QuicPacketNumberSpace, std::map<std::uint64_t, QuicSentPacket>> m_sent;
    std::uint64_t m_largestAcked[3] = {0, 0, 0};
    std::size_t m_bytesInFlight = 0;
};

}  // namespace qtng

#endif  // QTNG_QUIC_P_H
