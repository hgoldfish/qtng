#include "qtng/private/quic_p.h"

#include <cstring>

#include "qtng/aead.h"
#include "qtng/md.h"

using namespace std;

namespace qtng {

string quicInitialSalt()
{
    // RFC 9001 §5.2 Initial Salt for version 0x00000001
    static const unsigned char salt[] = {0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
                                         0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a};
    return string(reinterpret_cast<const char *>(salt), sizeof(salt));
}

string quicDeriveInitialSecret(const QuicConnectionId &dcid, bool isClient)
{
    const string initialSecret = hkdfExtract(MessageDigest::Sha256, quicInitialSalt(), dcid.bytes);
    return hkdfExpandLabel(MessageDigest::Sha256, initialSecret, isClient ? "client in" : "server in", string(), 32);
}

QuicTrafficKeys quicDeriveTrafficKeys(const string &secret)
{
    QuicTrafficKeys keys;
    keys.key = hkdfExpandLabel(MessageDigest::Sha256, secret, "quic key", string(), 16);
    keys.iv = hkdfExpandLabel(MessageDigest::Sha256, secret, "quic iv", string(), 12);
    keys.hp = hkdfExpandLabel(MessageDigest::Sha256, secret, "quic hp", string(), 16);
    return keys;
}

string quicNonceFromIv(const string &iv, uint64_t packetNumber)
{
    string nonce = iv;
    if (nonce.size() != 12) {
        return string();
    }
    for (int i = 0; i < 8; ++i) {
        nonce[11 - i] = static_cast<char>(static_cast<unsigned char>(nonce[11 - i]) ^ ((packetNumber >> (8 * i)) & 0xff));
    }
    return nonce;
}

string quicRetryIntegrityTag(const QuicConnectionId &odcid, const string &pseudoPacket)
{
    // RFC 9001 §5.8: pseudo-packet = ODCID length byte + ODCID + RETRY (minus tag).
    const string secret = hkdfExtract(MessageDigest::Sha256, string(), odcid.bytes);
    const string key = hkdfExpandLabel(MessageDigest::Sha256, secret, "quic key", string(), 16);
    const string iv = hkdfExpandLabel(MessageDigest::Sha256, secret, "quic iv", string(), 12);
    Aead aead(Aead::Aes128Gcm);
    if (!aead.setKey(key)) {
        return string();
    }
    string sealed;
    if (!aead.seal(iv, string(), pseudoPacket, &sealed) || sealed.size() < 16) {
        return string();
    }
    return sealed.substr(sealed.size() - 16);
}

namespace {

string applyHeaderProtection(const string &hpKey, const string &sample, string header, size_t pnOffset, int pnLength)
{
    if (sample.size() < 16 || hpKey.size() != 16 || pnOffset >= header.size()) {
        return string();
    }
    const string mask = aesEcbEncryptBlock(hpKey, sample.substr(0, 16));
    if (mask.size() != 16) {
        return string();
    }
    unsigned char first = static_cast<unsigned char>(header[0]);
    if (first & 0x80) {
        // long header: mask 4 bits
        first ^= static_cast<unsigned char>(mask[0]) & 0x0f;
    } else {
        first ^= static_cast<unsigned char>(mask[0]) & 0x1f;
    }
    header[0] = static_cast<char>(first);
    for (int i = 0; i < pnLength; ++i) {
        if (pnOffset + static_cast<size_t>(i) >= header.size()) {
            return string();
        }
        header[pnOffset + i] = static_cast<char>(static_cast<unsigned char>(header[pnOffset + i])
                                                 ^ static_cast<unsigned char>(mask[1 + i]));
    }
    return header;
}

// Remove header protection (sample at pn_offset+4), decode PN, build AAD.
bool removeHeaderProtection(const QuicTrafficKeys &keys, const char *data, size_t size, QuicPacketHeader *h,
                            uint64_t expectedPn, string *aad)
{
    if (size < h->pnOffset + 4 + 16) {
        return false;
    }
    const string sample(data + h->pnOffset + 4, 16);
    string hdr(data, h->pnOffset + 4);
    string unprotected = applyHeaderProtection(keys.hp, sample, hdr, h->pnOffset, 4);
    if (unprotected.empty()) {
        return false;
    }
    const uint8_t first = static_cast<uint8_t>(unprotected[0]);
    h->pnLength = (first & 0x03) + 1;
    h->keyPhase = (first & 0x04) != 0;
    uint64_t truncated = 0;
    for (int i = 0; i < h->pnLength; ++i) {
        truncated = (truncated << 8) | static_cast<unsigned char>(unprotected[h->pnOffset + i]);
    }
    h->packetNumber = quicDecodePacketNumber(expectedPn, truncated, h->pnLength);
    h->headerLength = h->pnOffset + static_cast<size_t>(h->pnLength);
    if (h->isLong) {
        h->longType = static_cast<QuicLongPacketType>((first >> 4) & 0x3);
    }
    *aad = unprotected.substr(0, h->headerLength);
    return true;
}

bool openPacketPayload(const QuicTrafficKeys &keys, const string &aad, const char *ct, size_t ctLen,
                       uint64_t packetNumber, string *payload)
{
    const string nonce = quicNonceFromIv(keys.iv, packetNumber);
    Aead aead(Aead::Aes128Gcm);
    if (!aead.setKey(keys.key)) {
        return false;
    }
    return aead.open(nonce, aad, string(ct, ctLen), payload);
}

}  // namespace

uint64_t quicDecodePacketNumber(uint64_t largestPn, uint64_t truncated, int pnLength)
{
    // RFC 9000 §A.3
    const uint64_t pnNbits = static_cast<uint64_t>(pnLength) * 8;
    const uint64_t expectedPn = largestPn + 1;
    const uint64_t pnWin = 1ull << pnNbits;
    const uint64_t pnHwin = pnWin / 2;
    const uint64_t pnMask = pnWin - 1;
    const uint64_t candidate = (expectedPn & ~pnMask) | truncated;
    if (candidate + pnHwin <= expectedPn && candidate + pnWin < (1ull << 62)) {
        return candidate + pnWin;
    }
    if (candidate > expectedPn + pnHwin && candidate >= pnWin) {
        return candidate - pnWin;
    }
    return candidate;
}

bool quicProtectPacket(const QuicTrafficKeys &keys, const string &header, const string &payload, uint64_t packetNumber,
                       int pnLength, string *outPacket)
{
    if (!outPacket || !keys.valid() || pnLength < 1 || pnLength > 4) {
        return false;
    }
    const string nonce = quicNonceFromIv(keys.iv, packetNumber);
    Aead aead(Aead::Aes128Gcm);
    if (!aead.setKey(keys.key)) {
        return false;
    }
    string sealed;
    if (!aead.seal(nonce, header, payload, &sealed)) {
        return false;
    }
    // RFC 9001 §5.4.3: sample starts at pn_offset + 4 in the wire packet.
    size_t pnOffset = header.size() - static_cast<size_t>(pnLength);
    string packet = header + sealed;
    if (packet.size() < pnOffset + 4 + 16) {
        return false;
    }
    const string sample = packet.substr(pnOffset + 4, 16);
    string protectedHeader = applyHeaderProtection(keys.hp, sample, header, pnOffset, pnLength);
    if (protectedHeader.empty()) {
        return false;
    }
    *outPacket = protectedHeader + sealed;
    return true;
}

bool quicUnprotectPacket(const QuicTrafficKeys &keys, const char *data, size_t size, QuicPacketHeader *header,
                         string *payload, uint64_t expectedPn, size_t *consumed)
{
    if (!keys.valid() || !data || !header || !payload || size < 20) {
        return false;
    }
    if (consumed) {
        *consumed = 0;
    }
    // Parse long header without reading PN (PN is protected)
    QuicPacketHeader h;
    if (!quicParsePacketHeader(data, size, &h, true)) {
        return false;
    }
    if (!h.isLong) {
        // Short header: DCID length is not on the wire; caller sets header->dcid length.
        const size_t dcidLen = header->dcid.bytes.size();
        if (size < 1 + dcidLen + 1) {
            return false;
        }
        h.dcid.bytes.assign(data + 1, dcidLen);
        h.pnOffset = 1 + dcidLen;
        string aad;
        if (!removeHeaderProtection(keys, data, size, &h, expectedPn, &aad)) {
            return false;
        }
        // Short header: remainder of datagram is this packet (no Length field).
        const size_t ctLen = size - h.headerLength;
        string plain;
        if (!openPacketPayload(keys, aad, data + h.headerLength, ctLen, h.packetNumber, &plain)) {
            return false;
        }
        *header = h;
        *payload = std::move(plain);
        if (consumed) {
            *consumed = size;
        }
        return true;
    }

    // Long header — Length covers PN + AEAD ciphertext (payload||tag).
    if (size < h.pnOffset + h.payloadLength || h.payloadLength < 20) {
        return false;
    }
    const size_t packetLen = h.pnOffset + h.payloadLength;
    string aad;
    if (!removeHeaderProtection(keys, data, size, &h, expectedPn, &aad)) {
        return false;
    }
    if (h.payloadLength < static_cast<size_t>(h.pnLength) + 16) {
        return false;
    }
    const size_t ctLen = h.payloadLength - static_cast<size_t>(h.pnLength);
    if (h.headerLength + ctLen > packetLen) {
        return false;
    }
    string plain;
    if (!openPacketPayload(keys, aad, data + h.headerLength, ctLen, h.packetNumber, &plain)) {
        return false;
    }
    *header = h;
    *payload = std::move(plain);
    if (consumed) {
        *consumed = packetLen;
    }
    return true;
}

}  // namespace qtng
