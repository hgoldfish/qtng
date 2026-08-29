#include "qtng/private/quic_p.h"

#include <cstring>

using namespace std;

namespace qtng {

namespace {

bool encodeConnId(const QuicConnectionId &id, string *out)
{
    if (id.bytes.size() > 20) {
        return false;
    }
    out->push_back(static_cast<char>(id.bytes.size()));
    out->append(id.bytes);
    return true;
}

bool decodeConnId(const char *data, size_t size, size_t *off, QuicConnectionId *id)
{
    if (*off >= size) {
        return false;
    }
    const size_t len = static_cast<unsigned char>(data[*off]);
    ++(*off);
    if (len > 20 || *off + len > size) {
        return false;
    }
    id->bytes.assign(data + *off, len);
    *off += len;
    return true;
}

void writePn(string *out, uint64_t pn, int pnLength)
{
    for (int i = pnLength - 1; i >= 0; --i) {
        out->push_back(static_cast<char>((pn >> (8 * i)) & 0xff));
    }
}

}  // namespace

bool quicEncodeFrame(const QuicFrame &frame, string *out)
{
    if (!out) {
        return false;
    }
    switch (frame.type) {
    case QuicFrame::Padding:
        out->push_back(0x00);
        return true;
    case QuicFrame::Ping:
        out->push_back(0x01);
        return true;
    case QuicFrame::Ack: {
        out->push_back(0x02);
        if (!quicEncodeVarint(frame.largestAcknowledged, out) || !quicEncodeVarint(frame.ackDelay, out)
            || !quicEncodeVarint(frame.ackRanges.size(), out) || !quicEncodeVarint(frame.firstAckRange, out)) {
            return false;
        }
        for (const QuicAckRange &r : frame.ackRanges) {
            if (!quicEncodeVarint(r.gap, out) || !quicEncodeVarint(r.ackRangeLength, out)) {
                return false;
            }
        }
        return true;
    }
    case QuicFrame::Crypto:
        out->push_back(0x06);
        return quicEncodeVarint(frame.offset, out) && quicEncodeVarint(frame.data.size(), out)
                && (out->append(frame.data), true);
    case QuicFrame::Stream: {
        // MVP: always set LEN so frames are self-delimiting inside a packet.
        uint8_t t = 0x08 | 0x02;
        if (frame.offset != 0) {
            t |= 0x04;  // OFF
        }
        if (frame.fin) {
            t |= 0x01;
        }
        out->push_back(static_cast<char>(t));
        if (!quicEncodeVarint(frame.streamId, out)) {
            return false;
        }
        if (t & 0x04) {
            if (!quicEncodeVarint(frame.offset, out)) {
                return false;
            }
        }
        if (t & 0x02) {
            if (!quicEncodeVarint(frame.data.size(), out)) {
                return false;
            }
        }
        out->append(frame.data);
        return true;
    }
    case QuicFrame::ResetStream:
        out->push_back(0x04);
        return quicEncodeVarint(frame.streamId, out) && quicEncodeVarint(frame.applicationErrorCode, out)
                && quicEncodeVarint(frame.finalSize, out);
    case QuicFrame::StopSending:
        out->push_back(0x05);
        return quicEncodeVarint(frame.streamId, out) && quicEncodeVarint(frame.applicationErrorCode, out);
    case QuicFrame::MaxData:
        out->push_back(0x10);
        return quicEncodeVarint(frame.maxData, out);
    case QuicFrame::MaxStreamData:
        out->push_back(0x11);
        return quicEncodeVarint(frame.streamId, out) && quicEncodeVarint(frame.maxData, out);
    case QuicFrame::PathChallenge:
    case QuicFrame::PathResponse:
        out->push_back(static_cast<char>(frame.type == QuicFrame::PathChallenge ? 0x1a : 0x1b));
        if (frame.pathData.size() != 8) {
            return false;
        }
        out->append(frame.pathData);
        return true;
    case QuicFrame::ConnectionClose:
        out->push_back(0x1c);
        return quicEncodeVarint(frame.errorCode, out) && quicEncodeVarint(frame.frameType, out)
                && quicEncodeVarint(frame.reasonPhrase.size(), out) && (out->append(frame.reasonPhrase), true);
    case QuicFrame::HandshakeDone:
        out->push_back(0x1e);
        return true;
    case QuicFrame::NewConnectionId:
        out->push_back(0x18);
        if (!quicEncodeVarint(frame.sequenceNumber, out) || !quicEncodeVarint(frame.retirePriorTo, out)) {
            return false;
        }
        if (frame.connectionId.bytes.empty() || frame.connectionId.bytes.size() > 20
            || frame.statelessResetToken.size() != 16) {
            return false;
        }
        out->push_back(static_cast<char>(frame.connectionId.bytes.size()));
        out->append(frame.connectionId.bytes);
        out->append(frame.statelessResetToken);
        return true;
    case QuicFrame::RetireConnectionId:
        out->push_back(0x19);
        return quicEncodeVarint(frame.sequenceNumber, out);
    default:
        return false;
    }
}

bool quicDecodeFrames(const char *data, size_t size, vector<QuicFrame> *frames)
{
    if (!data || !frames) {
        return false;
    }
    size_t off = 0;
    while (off < size) {
        const uint8_t t = static_cast<uint8_t>(data[off++]);
        QuicFrame f;
        f.rawType = t;
        if (t == 0x00) {
            f.type = QuicFrame::Padding;
            // consume consecutive padding
            while (off < size && static_cast<uint8_t>(data[off]) == 0x00) {
                ++off;
            }
            frames->push_back(f);
            continue;
        }
        if (t == 0x01) {
            f.type = QuicFrame::Ping;
            frames->push_back(f);
            continue;
        }
        if (t == 0x02 || t == 0x03) {
            f.type = (t == 0x02) ? QuicFrame::Ack : QuicFrame::AckEcn;
            size_t c = 0;
            uint64_t ackRangeCount = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.largestAcknowledged)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.ackDelay)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &ackRangeCount)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.firstAckRange)) {
                return false;
            }
            off += c;
            for (uint64_t i = 0; i < ackRangeCount; ++i) {
                QuicAckRange r;
                if (!quicDecodeVarint(data + off, size - off, &c, &r.gap)) {
                    return false;
                }
                off += c;
                if (!quicDecodeVarint(data + off, size - off, &c, &r.ackRangeLength)) {
                    return false;
                }
                off += c;
                f.ackRanges.push_back(r);
            }
            if (t == 0x03) {
                // skip 3 ECN counts
                for (int i = 0; i < 3; ++i) {
                    uint64_t ignore = 0;
                    if (!quicDecodeVarint(data + off, size - off, &c, &ignore)) {
                        return false;
                    }
                    off += c;
                }
            }
            frames->push_back(f);
            continue;
        }
        if (t == 0x06) {
            f.type = QuicFrame::Crypto;
            size_t c = 0;
            uint64_t len = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.offset)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &len)) {
                return false;
            }
            off += c;
            if (off + len > size) {
                return false;
            }
            f.data.assign(data + off, static_cast<size_t>(len));
            off += static_cast<size_t>(len);
            frames->push_back(f);
            continue;
        }
        if (t == 0x07) {
            // NEW_TOKEN — accept and ignore for MVP.
            f.type = QuicFrame::NewToken;
            size_t c = 0;
            uint64_t len = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &len)) {
                return false;
            }
            off += c;
            if (off + len > size) {
                return false;
            }
            f.data.assign(data + off, static_cast<size_t>(len));
            off += static_cast<size_t>(len);
            frames->push_back(f);
            continue;
        }
        if ((t & 0xf8) == 0x08) {
            f.type = QuicFrame::Stream;
            f.fin = (t & 0x01) != 0;
            const bool hasLen = (t & 0x02) != 0;
            const bool hasOff = (t & 0x04) != 0;
            f.hasLength = hasLen;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.streamId)) {
                return false;
            }
            off += c;
            if (hasOff) {
                if (!quicDecodeVarint(data + off, size - off, &c, &f.offset)) {
                    return false;
                }
                off += c;
            }
            uint64_t len = 0;
            if (hasLen) {
                if (!quicDecodeVarint(data + off, size - off, &c, &len)) {
                    return false;
                }
                off += c;
                if (off + len > size) {
                    return false;
                }
                f.data.assign(data + off, static_cast<size_t>(len));
                off += static_cast<size_t>(len);
            } else {
                f.data.assign(data + off, size - off);
                off = size;
            }
            frames->push_back(f);
            continue;
        }
        if (t == 0x04) {
            f.type = QuicFrame::ResetStream;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.streamId)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.applicationErrorCode)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.finalSize)) {
                return false;
            }
            off += c;
            frames->push_back(f);
            continue;
        }
        if (t == 0x05) {
            f.type = QuicFrame::StopSending;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.streamId)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.applicationErrorCode)) {
                return false;
            }
            off += c;
            frames->push_back(f);
            continue;
        }
        if (t == 0x10) {
            f.type = QuicFrame::MaxData;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.maxData)) {
                return false;
            }
            off += c;
            frames->push_back(f);
            continue;
        }
        if (t == 0x11) {
            f.type = QuicFrame::MaxStreamData;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.streamId)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.maxData)) {
                return false;
            }
            off += c;
            frames->push_back(f);
            continue;
        }
        if (t == 0x12 || t == 0x13) {
            f.type = (t == 0x12) ? QuicFrame::MaxStreamsBidi : QuicFrame::MaxStreamsUni;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.maxData)) {
                return false;
            }
            off += c;
            frames->push_back(f);
            continue;
        }
        if (t == 0x14) {
            f.type = QuicFrame::DataBlocked;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.maxData)) {
                return false;
            }
            off += c;
            frames->push_back(f);
            continue;
        }
        if (t == 0x15) {
            f.type = QuicFrame::StreamDataBlocked;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.streamId)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.maxData)) {
                return false;
            }
            off += c;
            frames->push_back(f);
            continue;
        }
        if (t == 0x16 || t == 0x17) {
            f.type = (t == 0x16) ? QuicFrame::StreamsBlockedBidi : QuicFrame::StreamsBlockedUni;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.maxData)) {
                return false;
            }
            off += c;
            frames->push_back(f);
            continue;
        }
        if (t == 0x1a || t == 0x1b) {
            f.type = (t == 0x1a) ? QuicFrame::PathChallenge : QuicFrame::PathResponse;
            if (off + 8 > size) {
                return false;
            }
            f.pathData.assign(data + off, 8);
            off += 8;
            frames->push_back(f);
            continue;
        }
        if (t == 0x1c || t == 0x1d) {
            f.type = (t == 0x1c) ? QuicFrame::ConnectionClose : QuicFrame::ConnectionCloseApp;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.errorCode)) {
                return false;
            }
            off += c;
            if (t == 0x1c) {
                if (!quicDecodeVarint(data + off, size - off, &c, &f.frameType)) {
                    return false;
                }
                off += c;
            }
            uint64_t reasonLen = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &reasonLen)) {
                return false;
            }
            off += c;
            if (off + reasonLen > size) {
                return false;
            }
            f.reasonPhrase.assign(data + off, static_cast<size_t>(reasonLen));
            off += static_cast<size_t>(reasonLen);
            frames->push_back(f);
            continue;
        }
        if (t == 0x1e) {
            f.type = QuicFrame::HandshakeDone;
            frames->push_back(f);
            continue;
        }
        if (t == 0x18) {
            f.type = QuicFrame::NewConnectionId;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.sequenceNumber)) {
                return false;
            }
            off += c;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.retirePriorTo)) {
                return false;
            }
            off += c;
            if (off >= size) {
                return false;
            }
            const size_t cidLen = static_cast<unsigned char>(data[off++]);
            if (cidLen == 0 || cidLen > 20 || off + cidLen + 16 > size) {
                return false;
            }
            f.connectionId.bytes.assign(data + off, cidLen);
            off += cidLen;
            f.statelessResetToken.assign(data + off, 16);
            off += 16;
            frames->push_back(f);
            continue;
        }
        if (t == 0x19) {
            f.type = QuicFrame::RetireConnectionId;
            size_t c = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &f.sequenceNumber)) {
                return false;
            }
            off += c;
            frames->push_back(f);
            continue;
        }
        // Unknown frame type — cannot skip without a length; fail the packet.
        return false;
    }
    return true;
}

string quicBuildLongHeader(QuicLongPacketType type, uint32_t version, const QuicConnectionId &dcid,
                           const QuicConnectionId &scid, const string &token, uint64_t packetNumber, int pnLength,
                           size_t payloadAndTagLen)
{
    string out;
    uint8_t first = 0xc0;
    first |= static_cast<uint8_t>((static_cast<uint8_t>(type) & 0x3) << 4);
    first |= static_cast<uint8_t>((pnLength - 1) & 0x3);
    out.push_back(static_cast<char>(first));
    out.push_back(static_cast<char>((version >> 24) & 0xff));
    out.push_back(static_cast<char>((version >> 16) & 0xff));
    out.push_back(static_cast<char>((version >> 8) & 0xff));
    out.push_back(static_cast<char>(version & 0xff));
    encodeConnId(dcid, &out);
    encodeConnId(scid, &out);
    if (type == QuicLongInitial) {
        quicEncodeVarint(token.size(), &out);
        out.append(token);
    }
    const size_t lengthField = payloadAndTagLen + static_cast<size_t>(pnLength);
    quicEncodeVarint(lengthField, &out);
    writePn(&out, packetNumber, pnLength);
    return out;
}

string quicBuildShortHeader(const QuicConnectionId &dcid, uint64_t packetNumber, int pnLength, bool keyPhase)
{
    string out;
    uint8_t first = 0x40;
    if (keyPhase) {
        first |= 0x04;
    }
    first |= static_cast<uint8_t>((pnLength - 1) & 0x3);
    out.push_back(static_cast<char>(first));
    out.append(dcid.bytes);
    writePn(&out, packetNumber, pnLength);
    return out;
}

string quicBuildRetryPacket(uint32_t version, const QuicConnectionId &dcid, const QuicConnectionId &scid,
                            const string &token, const QuicConnectionId &odcid)
{
    // RFC 9000 §17.2.5: long header, type 3, no length/packet number field.
    string out;
    out.push_back(static_cast<char>(0xf0));  // header form + fixed + type 3
    out.push_back(static_cast<char>((version >> 24) & 0xff));
    out.push_back(static_cast<char>((version >> 16) & 0xff));
    out.push_back(static_cast<char>((version >> 8) & 0xff));
    out.push_back(static_cast<char>(version & 0xff));
    encodeConnId(dcid, &out);
    encodeConnId(scid, &out);
    out.append(token);
    string pseudoPacket;
    pseudoPacket.push_back(static_cast<char>(odcid.bytes.size()));
    pseudoPacket.append(odcid.bytes);
    pseudoPacket.append(out);
    const string tag = quicRetryIntegrityTag(odcid, pseudoPacket);
    if (tag.size() != 16) {
        return string();
    }
    out.append(tag);
    return out;
}

bool quicParseRetryPacket(const char *data, size_t size, const QuicConnectionId &odcid, uint32_t *version,
                          QuicConnectionId *dcid, QuicConnectionId *scid, string *token)
{
    if (!data || size < 5) {
        return false;
    }
    const uint8_t first = static_cast<uint8_t>(data[0]);
    if (!(first & 0x80) || ((first >> 4) & 0x3) != 3 || !(first & 0x40)) {
        return false;  // not a long-header Retry packet
    }
    size_t off = 1;
    *version = (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 24)
            | (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16)
            | (static_cast<uint32_t>(static_cast<unsigned char>(data[3])) << 8)
            | static_cast<uint32_t>(static_cast<unsigned char>(data[4]));
    off = 5;
    if (!decodeConnId(data, size, &off, dcid) || !decodeConnId(data, size, &off, scid)) {
        return false;
    }
    if (off + 16 > size) {
        return false;
    }
    const size_t tokenLen = size - off - 16;
    if (token) {
        token->assign(data + off, tokenLen);
    }
    const string pseudo(data, size - 16);
    string pseudoPacket;
    pseudoPacket.push_back(static_cast<char>(odcid.bytes.size()));
    pseudoPacket.append(odcid.bytes);
    pseudoPacket.append(pseudo);
    const string tag = quicRetryIntegrityTag(odcid, pseudoPacket);
    return tag.size() == 16 && memcmp(tag.data(), data + off + tokenLen, 16) == 0;
}

bool quicParsePacketHeader(const char *data, size_t size, QuicPacketHeader *header, bool skipPn)
{
    if (!data || !header || size < 1) {
        return false;
    }
    const uint8_t first = static_cast<uint8_t>(data[0]);
    header->isLong = (first & 0x80) != 0;
    size_t off = 1;
    if (header->isLong) {
        if (size < 5) {
            return false;
        }
        header->longType = static_cast<QuicLongPacketType>((first >> 4) & 0x3);
        header->version = (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 24)
                | (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16)
                | (static_cast<uint32_t>(static_cast<unsigned char>(data[3])) << 8)
                | static_cast<uint32_t>(static_cast<unsigned char>(data[4]));
        off = 5;
        if (!decodeConnId(data, size, &off, &header->dcid) || !decodeConnId(data, size, &off, &header->scid)) {
            return false;
        }
        if (header->longType == QuicLongInitial) {
            size_t c = 0;
            uint64_t tokenLen = 0;
            if (!quicDecodeVarint(data + off, size - off, &c, &tokenLen)) {
                return false;
            }
            off += c;
            if (off + tokenLen > size) {
                return false;
            }
            header->token.assign(data + off, static_cast<size_t>(tokenLen));
            off += static_cast<size_t>(tokenLen);
        }
        size_t c = 0;
        uint64_t length = 0;
        if (!quicDecodeVarint(data + off, size - off, &c, &length)) {
            return false;
        }
        off += c;
        header->payloadLength = static_cast<size_t>(length);
        header->pnOffset = off;
        header->pnLength = (first & 0x03) + 1;
        if (skipPn) {
            header->headerLength = off;
            return true;
        }
        if (off + static_cast<size_t>(header->pnLength) > size) {
            return false;
        }
        header->packetNumber = 0;
        for (int i = 0; i < header->pnLength; ++i) {
            header->packetNumber = (header->packetNumber << 8) | static_cast<unsigned char>(data[off + i]);
        }
        header->headerLength = off + static_cast<size_t>(header->pnLength);
        return true;
    }

    // Short header
    header->pnLength = (first & 0x03) + 1;
    // Destination CID length is not encoded; caller must know. For parse without CID
    // length we leave dcid empty and treat remaining as needing external CID length.
    header->pnOffset = off;
    header->headerLength = off;
    return true;
}

}  // namespace qtng
