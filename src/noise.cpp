#include <cstring>
#include <string>

#include "qtng/noise.h"
#include "qtng/md.h"
#include "qtng/utils/logging.h"

extern "C" {
#include <openssl/curve25519.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <openssl/hmac.h>
}

using namespace std;

NG_LOGGER("qtng.noise");

namespace qtng {

namespace {

const size_t kHashLen = 32;
const size_t kDhLen = 32;
const size_t kTagLen = 16;
const size_t kNonceLen = 12;

const char *kProtocolXX = "Noise_XX_25519_ChaChaPoly_SHA256";
const char *kProtocolPskXX = "NoisePSK_XX_25519_ChaChaPoly_SHA256";

void writeNonce(uint8_t nonce[kNonceLen], uint64_t n)
{
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<uint8_t>((n >> (8 * i)) & 0xff);
    }
}

string sha256(const string &data)
{
    return MessageDigest::digest(data, MessageDigest::Sha256);
}

}  // namespace

NoiseKey NoiseKey::generate()
{
    NoiseKey key;
    key.privateKey.resize(kDhLen);
    key.publicKey.resize(kDhLen);
    X25519_keypair(reinterpret_cast<uint8_t *>(&key.publicKey[0]),
                   reinterpret_cast<uint8_t *>(&key.privateKey[0]));
    return key;
}

NoiseKey NoiseKey::fromPrivateKey(const string &privateKey32)
{
    NoiseKey key;
    if (privateKey32.size() != kDhLen) {
        return key;
    }
    key.privateKey = privateKey32;
    key.publicKey.resize(kDhLen);
    static const uint8_t kBasePoint[X25519_KEY_LENGTH] = {9};
    if (!X25519(reinterpret_cast<uint8_t *>(&key.publicKey[0]),
                reinterpret_cast<const uint8_t *>(privateKey32.data()),
                kBasePoint)) {
        key.privateKey.clear();
        key.publicKey.clear();
    }
    return key;
}

string NoiseKey::dh(const string &privateKey32, const string &peerPublicKey32)
{
    if (privateKey32.size() != kDhLen || peerPublicKey32.size() != kDhLen) {
        return string();
    }
    string shared(kDhLen, '\0');
    if (!X25519(reinterpret_cast<uint8_t *>(&shared[0]),
                reinterpret_cast<const uint8_t *>(privateKey32.data()),
                reinterpret_cast<const uint8_t *>(peerPublicKey32.data()))) {
        return string();
    }
    return shared;
}

NoiseCipherState::NoiseCipherState()
    : m_hasKey(false)
    , m_lastDecryptOk(false)
    , m_nonce(0)
    , m_highestRemoteNonce(0)
    , m_replayWindow(0)
{
}

NoiseCipherState::~NoiseCipherState() {}

void NoiseCipherState::initializeKey(const string &key32)
{
    m_hasKey = (key32.size() == kDhLen);
    m_key = m_hasKey ? key32 : string();
    m_nonce = 0;
    // mixKey() reinitializes the cipher during handshake; the anti-replay
    // window must reset together with the nonce or the next decrypt(0) is
    // falsely rejected as a replay of the previous key epoch.
    m_highestRemoteNonce = 0;
    m_replayWindow = 0;
}

string NoiseCipherState::encryptWithAd(const string &ad, const string &plaintext)
{
    uint64_t used = 0;
    return encryptWithAd(ad, plaintext, &used);
}

string NoiseCipherState::encryptWithAd(const string &ad, const string &plaintext, uint64_t *outNonce)
{
    if (!m_hasKey) {
        return string();
    }
    EVP_AEAD_CTX *ctx = EVP_AEAD_CTX_new();
    if (!ctx) {
        return string();
    }
    string out;
    do {
        if (!EVP_AEAD_CTX_init(ctx, EVP_aead_chacha20_poly1305(),
                               reinterpret_cast<const uint8_t *>(m_key.data()), m_key.size(),
                               EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
            break;
        }
        const uint64_t n = m_nonce;
        uint8_t nonce[kNonceLen];
        writeNonce(nonce, n);
        out.resize(plaintext.size() + kTagLen);
        size_t outLen = 0;
        if (!EVP_AEAD_CTX_seal(ctx, reinterpret_cast<uint8_t *>(&out[0]), &outLen, out.size(), nonce, kNonceLen,
                               reinterpret_cast<const uint8_t *>(plaintext.data()), plaintext.size(),
                               reinterpret_cast<const uint8_t *>(ad.data()), ad.size())) {
            out.clear();
            break;
        }
        out.resize(outLen);
        ++m_nonce;
        if (outNonce) {
            *outNonce = n;
        }
    } while (false);
    EVP_AEAD_CTX_free(ctx);
    return out;
}

string NoiseCipherState::decryptWithAd(const string &ad, const string &ciphertextAndTag)
{
    m_lastDecryptOk = false;
    // Sequential nonce path used by the handshake state machine.
    // Must treat empty-plaintext success as success (AEAD tag-only ciphertext).
    if (!m_hasKey || ciphertextAndTag.size() < kTagLen) {
        return string();
    }
    EVP_AEAD_CTX *ctx = EVP_AEAD_CTX_new();
    if (!ctx) {
        return string();
    }
    string out;
    bool ok = false;
    do {
        if (!EVP_AEAD_CTX_init(ctx, EVP_aead_chacha20_poly1305(),
                               reinterpret_cast<const uint8_t *>(m_key.data()), m_key.size(),
                               EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
            break;
        }
        uint8_t nonceBuf[kNonceLen];
        writeNonce(nonceBuf, m_nonce);
        out.resize(ciphertextAndTag.size() - kTagLen);
        size_t outLen = 0;
        uint8_t *outPtr = out.empty() ? nullptr : reinterpret_cast<uint8_t *>(&out[0]);
        if (!EVP_AEAD_CTX_open(ctx, outPtr, &outLen, out.size(), nonceBuf, kNonceLen,
                               reinterpret_cast<const uint8_t *>(ciphertextAndTag.data()), ciphertextAndTag.size(),
                               reinterpret_cast<const uint8_t *>(ad.data()), ad.size())) {
            out.clear();
            break;
        }
        out.resize(outLen);
        ++m_nonce;
        m_lastDecryptOk = true;
        ok = true;
    } while (false);
    EVP_AEAD_CTX_free(ctx);
    return ok ? out : string();
}

string NoiseCipherState::decryptWithAd(const string &ad, const string &ciphertextAndTag, uint64_t nonce)
{
    m_lastDecryptOk = false;
    if (!m_hasKey || ciphertextAndTag.size() < kTagLen) {
        return string();
    }
    // Tentatively mark the nonce; roll back if AEAD authentication fails.
    // Otherwise a forged / handshake-retransmit blob can poison the window
    // (e.g. huge bogus nonce) and permanently reject legitimate nonce 0.
    const uint64_t savedHighest = m_highestRemoteNonce;
    const uint64_t savedWindow = m_replayWindow;
    if (!acceptIncomingNonce(nonce)) {
        return string();
    }
    EVP_AEAD_CTX *ctx = EVP_AEAD_CTX_new();
    if (!ctx) {
        m_highestRemoteNonce = savedHighest;
        m_replayWindow = savedWindow;
        return string();
    }
    string out;
    bool ok = false;
    do {
        if (!EVP_AEAD_CTX_init(ctx, EVP_aead_chacha20_poly1305(),
                               reinterpret_cast<const uint8_t *>(m_key.data()), m_key.size(),
                               EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
            break;
        }
        uint8_t nonceBuf[kNonceLen];
        writeNonce(nonceBuf, nonce);
        out.resize(ciphertextAndTag.size() - kTagLen);
        size_t outLen = 0;
        uint8_t *outPtr = out.empty() ? nullptr : reinterpret_cast<uint8_t *>(&out[0]);
        if (!EVP_AEAD_CTX_open(ctx, outPtr, &outLen, out.size(), nonceBuf, kNonceLen,
                               reinterpret_cast<const uint8_t *>(ciphertextAndTag.data()), ciphertextAndTag.size(),
                               reinterpret_cast<const uint8_t *>(ad.data()), ad.size())) {
            out.clear();
            break;
        }
        out.resize(outLen);
        m_lastDecryptOk = true;
        ok = true;
    } while (false);
    EVP_AEAD_CTX_free(ctx);
    if (!ok) {
        m_highestRemoteNonce = savedHighest;
        m_replayWindow = savedWindow;
    }
    return ok ? out : string();
}

bool NoiseCipherState::acceptIncomingNonce(uint64_t remoteNonce)
{
    if (remoteNonce > m_highestRemoteNonce) {
        const uint64_t shift = remoteNonce - m_highestRemoteNonce;
        if (shift >= 64) {
            m_replayWindow = 1;
        } else {
            m_replayWindow = (m_replayWindow << shift) | 1;
        }
        m_highestRemoteNonce = remoteNonce;
        return true;
    }
    const uint64_t bit = m_highestRemoteNonce - remoteNonce;
    if (bit >= 64) {
        return false;
    }
    const uint64_t mask = uint64_t(1) << bit;
    if (m_replayWindow & mask) {
        return false;
    }
    m_replayWindow |= mask;
    return true;
}

NoiseHandshakeState::NoiseHandshakeState()
    : m_pattern(NoisePattern::XX)
    , m_role(NoiseRole::Initiator)
    , m_complete(false)
    , m_msgIndex(0)
{
}

NoiseHandshakeState::~NoiseHandshakeState() {}

bool NoiseHandshakeState::initialize(NoisePattern pattern, NoiseRole role, const NoiseKey &localStatic,
                                     const string &remoteStaticPublic, const string &psk)
{
    m_error.clear();
    m_complete = false;
    m_msgIndex = 0;
    m_pattern = pattern;
    m_role = role;
    m_s = localStatic;
    m_e = NoiseKey();
    m_rs = remoteStaticPublic;
    m_re.clear();
    m_psk = psk;
    m_cs = NoiseCipherState();

    if (!m_s.isValid()) {
        m_error = "local static key is invalid";
        return false;
    }
    if (!m_rs.empty() && m_rs.size() != kDhLen) {
        m_error = "remote static public key must be 32 bytes";
        return false;
    }
    if (pattern == NoisePattern::PSK_XX && psk.empty()) {
        m_error = "PSK_XX requires a non-empty PSK";
        return false;
    }
    if (pattern == NoisePattern::XX && !psk.empty()) {
        m_error = "XX pattern does not take a PSK";
        return false;
    }

    const char *name = (pattern == NoisePattern::PSK_XX) ? kProtocolPskXX : kProtocolXX;
    const string protocolName(name);
    if (protocolName.size() <= kHashLen) {
        m_h.assign(kHashLen, '\0');
        memcpy(&m_h[0], protocolName.data(), protocolName.size());
    } else {
        m_h = sha256(protocolName);
    }
    m_ck = m_h;

    if (pattern == NoisePattern::PSK_XX) {
        mixKey(psk);
    }
    return true;
}

void NoiseHandshakeState::mixHash(const string &data)
{
    m_h = sha256(m_h + data);
}

void NoiseHandshakeState::mixKey(const string &material)
{
    const string outputs = hkdf(m_ck, material, 2);
    if (outputs.size() != kHashLen * 2) {
        m_error = "mixKey HKDF failed";
        return;
    }
    m_ck = outputs.substr(0, kHashLen);
    m_cs.initializeKey(outputs.substr(kHashLen, kHashLen));
}

string NoiseHandshakeState::encryptAndHash(const string &plaintext)
{
    string ciphertext;
    if (m_cs.hasKey()) {
        ciphertext = m_cs.encryptWithAd(m_h, plaintext);
        // Keyed encrypt always appends a 16-byte tag; empty means failure.
        if (ciphertext.empty()) {
            m_error = "encryptAndHash failed";
            return string();
        }
    } else {
        ciphertext = plaintext;
    }
    mixHash(ciphertext);
    return ciphertext;
}

string NoiseHandshakeState::decryptAndHash(const string &ciphertextAndTag)
{
    string plaintext;
    if (m_cs.hasKey()) {
        if (ciphertextAndTag.size() < kTagLen) {
            m_error = "decryptAndHash truncated";
            return string();
        }
        plaintext = m_cs.decryptWithAd(m_h, ciphertextAndTag);
        if (!m_cs.lastDecryptOk()) {
            m_error = "decryptAndHash failed";
            return string();
        }
    } else {
        plaintext = ciphertextAndTag;
    }
    mixHash(ciphertextAndTag);
    return plaintext;
}

string NoiseHandshakeState::hkdf(const string &chainingKey, const string &inputKeyMaterial, int numOutputs)
{
    if (numOutputs < 2 || numOutputs > 3) {
        return string();
    }
    uint8_t prk[EVP_MAX_MD_SIZE];
    size_t prkLen = 0;
    if (!HKDF_extract(prk, &prkLen, EVP_sha256(),
                      reinterpret_cast<const uint8_t *>(inputKeyMaterial.data()), inputKeyMaterial.size(),
                      reinterpret_cast<const uint8_t *>(chainingKey.data()), chainingKey.size())) {
        return string();
    }
    string out(static_cast<size_t>(numOutputs) * kHashLen, '\0');
    if (!HKDF_expand(reinterpret_cast<uint8_t *>(&out[0]), out.size(), EVP_sha256(), prk, prkLen, nullptr, 0)) {
        return string();
    }
    return out;
}

bool NoiseHandshakeState::writeMessage(const string &payload, string *outMessage)
{
    if (!outMessage || m_complete) {
        m_error = "handshake already complete or null output";
        return false;
    }
    string message;
    const bool initiator = (m_role == NoiseRole::Initiator);

    if (initiator && m_msgIndex == 0) {
        // -> e
        m_e = NoiseKey::generate();
        if (!m_e.isValid()) {
            m_error = "failed to generate ephemeral key";
            return false;
        }
        message += m_e.publicKey;
        mixHash(m_e.publicKey);
        const string &cipherPayload = encryptAndHash(payload);
        if (!m_error.empty()) {
            return false;
        }
        message += cipherPayload;
        *outMessage = message;
        ++m_msgIndex;
        return true;
    }

    if (!initiator && m_msgIndex == 1) {
        // -> e, ee, s, es
        m_e = NoiseKey::generate();
        if (!m_e.isValid()) {
            m_error = "failed to generate ephemeral key";
            return false;
        }
        message += m_e.publicKey;
        mixHash(m_e.publicKey);
        const string &ee = NoiseKey::dh(m_e.privateKey, m_re);
        if (ee.empty()) {
            m_error = "ee DH failed";
            return false;
        }
        mixKey(ee);
        const string &encS = encryptAndHash(m_s.publicKey);
        if (!m_error.empty() || encS.empty()) {
            if (m_error.empty()) {
                m_error = "encrypt static key failed";
            }
            return false;
        }
        message += encS;
        const string &es = NoiseKey::dh(m_s.privateKey, m_re);
        if (es.empty()) {
            m_error = "es DH failed";
            return false;
        }
        mixKey(es);
        const string &cipherPayload = encryptAndHash(payload);
        if (!m_error.empty()) {
            return false;
        }
        message += cipherPayload;
        *outMessage = message;
        ++m_msgIndex;
        return true;
    }

    if (initiator && m_msgIndex == 2) {
        // -> s, se
        const string &encS = encryptAndHash(m_s.publicKey);
        if (!m_error.empty() || encS.empty()) {
            if (m_error.empty()) {
                m_error = "encrypt static key failed";
            }
            return false;
        }
        message += encS;
        const string &se = NoiseKey::dh(m_s.privateKey, m_re);
        if (se.empty()) {
            m_error = "se DH failed";
            return false;
        }
        mixKey(se);
        const string &cipherPayload = encryptAndHash(payload);
        if (!m_error.empty()) {
            return false;
        }
        message += cipherPayload;
        *outMessage = message;
        ++m_msgIndex;
        m_complete = true;
        return true;
    }

    m_error = "writeMessage called at unexpected handshake step";
    return false;
}

bool NoiseHandshakeState::readMessage(const string &message, string *outPayload)
{
    if (!outPayload || m_complete) {
        m_error = "handshake already complete or null output";
        return false;
    }
    size_t pos = 0;
    const bool initiator = (m_role == NoiseRole::Initiator);

    auto take = [&](size_t n) -> string {
        if (pos + n > message.size()) {
            return string();
        }
        string part = message.substr(pos, n);
        pos += n;
        return part;
    };

    if (!initiator && m_msgIndex == 0) {
        // <- e
        m_re = take(kDhLen);
        if (m_re.size() != kDhLen) {
            m_error = "missing remote ephemeral";
            return false;
        }
        mixHash(m_re);
        const string &cipherPayload = message.substr(pos);
        *outPayload = decryptAndHash(cipherPayload);
        if (!m_error.empty()) {
            return false;
        }
        ++m_msgIndex;
        return true;
    }

    if (initiator && m_msgIndex == 1) {
        // <- e, ee, s, es
        m_re = take(kDhLen);
        if (m_re.size() != kDhLen) {
            m_error = "missing remote ephemeral";
            return false;
        }
        mixHash(m_re);
        const string &ee = NoiseKey::dh(m_e.privateKey, m_re);
        if (ee.empty()) {
            m_error = "ee DH failed";
            return false;
        }
        mixKey(ee);
        const string encS = take(kDhLen + kTagLen);
        if (encS.size() != kDhLen + kTagLen) {
            m_error = "missing remote static";
            return false;
        }
        {
            const string expectedRs = m_rs;
            m_rs = decryptAndHash(encS);
            if (m_rs.size() != kDhLen || !m_error.empty()) {
                if (m_error.empty()) {
                    m_error = "decrypt remote static failed";
                }
                return false;
            }
            if (!expectedRs.empty() && expectedRs != m_rs) {
                m_error = "remote static public key mismatch";
                return false;
            }
        }
        const string &es = NoiseKey::dh(m_e.privateKey, m_rs);
        if (es.empty()) {
            m_error = "es DH failed";
            return false;
        }
        mixKey(es);
        const string &cipherPayload = message.substr(pos);
        *outPayload = decryptAndHash(cipherPayload);
        if (!m_error.empty()) {
            return false;
        }
        ++m_msgIndex;
        return true;
    }

    if (!initiator && m_msgIndex == 2) {
        // <- s, se
        const string encS = take(kDhLen + kTagLen);
        if (encS.size() != kDhLen + kTagLen) {
            m_error = "missing remote static";
            return false;
        }
        {
            const string expectedRs = m_rs;
            m_rs = decryptAndHash(encS);
            if (m_rs.size() != kDhLen || !m_error.empty()) {
                if (m_error.empty()) {
                    m_error = "decrypt remote static failed";
                }
                return false;
            }
            if (!expectedRs.empty() && expectedRs != m_rs) {
                m_error = "remote static public key mismatch";
                return false;
            }
        }
        const string &se = NoiseKey::dh(m_e.privateKey, m_rs);
        if (se.empty()) {
            m_error = "se DH failed";
            return false;
        }
        mixKey(se);
        const string &cipherPayload = message.substr(pos);
        *outPayload = decryptAndHash(cipherPayload);
        if (!m_error.empty()) {
            return false;
        }
        ++m_msgIndex;
        m_complete = true;
        return true;
    }

    m_error = "readMessage called at unexpected handshake step";
    return false;
}

bool NoiseHandshakeState::split(NoiseCipherState *send, NoiseCipherState *recv)
{
    if (!m_complete || !send || !recv) {
        m_error = "handshake not complete";
        return false;
    }
    const string outputs = hkdf(m_ck, string(), 2);
    if (outputs.size() != kHashLen * 2) {
        m_error = "split HKDF failed";
        return false;
    }
    NoiseCipherState c1;
    NoiseCipherState c2;
    c1.initializeKey(outputs.substr(0, kHashLen));
    c2.initializeKey(outputs.substr(kHashLen, kHashLen));
    if (m_role == NoiseRole::Initiator) {
        *send = std::move(c1);
        *recv = std::move(c2);
    } else {
        *send = std::move(c2);
        *recv = std::move(c1);
    }
    return true;
}

string noiseHmacSha256(const string &key, const string &data)
{
    unsigned int len = 0;
    unsigned char md[EVP_MAX_MD_SIZE];
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char *>(data.data()), data.size(), md, &len)) {
        return string();
    }
    return string(reinterpret_cast<char *>(md), len);
}

string noiseHkdf(const string &secret, const string &salt, const string &info, size_t outLen)
{
    string out(outLen, '\0');
    if (!HKDF(reinterpret_cast<uint8_t *>(&out[0]), outLen, EVP_sha256(),
              reinterpret_cast<const uint8_t *>(secret.data()), secret.size(),
              reinterpret_cast<const uint8_t *>(salt.data()), salt.size(),
              reinterpret_cast<const uint8_t *>(info.data()), info.size())) {
        return string();
    }
    return out;
}

}  // namespace qtng
