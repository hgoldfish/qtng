#include <cstring>
#include <string>

#include "qtng/noise.h"
#include "qtng/md.h"
#include "qtng/utils/logging.h"

extern "C" {
#include <openssl/evp.h>
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
const size_t kMaxFramePayload = 65535;

const char *kProtocolXX = "Noise_XX_25519_ChaChaPoly_SHA256";
const char *kProtocolPskXX = "NoisePSK_XX_25519_ChaChaPoly_SHA256";
const char *kProtocolIK = "Noise_IK_25519_ChaChaPoly_SHA256";

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

bool aeadSeal(const string &key, const uint8_t nonce[kNonceLen], const string &ad, const string &plaintext,
              string *out)
{
    if (!out) {
        return false;
    }
    out->clear();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(kNonceLen), nullptr) != 1) {
            break;
        }
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const uint8_t *>(key.data()), nonce) != 1) {
            break;
        }
        int len = 0;
        if (!ad.empty()) {
            if (EVP_EncryptUpdate(ctx, nullptr, &len, reinterpret_cast<const uint8_t *>(ad.data()),
                                  static_cast<int>(ad.size()))
                != 1) {
                break;
            }
        }
        out->resize(plaintext.size() + kTagLen);
        uint8_t *outPtr = reinterpret_cast<uint8_t *>(&(*out)[0]);
        int outLen = 0;
        if (!plaintext.empty()) {
            if (EVP_EncryptUpdate(ctx, outPtr, &outLen, reinterpret_cast<const uint8_t *>(plaintext.data()),
                                  static_cast<int>(plaintext.size()))
                != 1) {
                break;
            }
        }
        int finalLen = 0;
        if (EVP_EncryptFinal_ex(ctx, outPtr + outLen, &finalLen) != 1) {
            break;
        }
        outLen += finalLen;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kTagLen), outPtr + outLen) != 1) {
            break;
        }
        out->resize(static_cast<size_t>(outLen) + kTagLen);
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        out->clear();
    }
    return ok;
}

bool aeadOpen(const string &key, const uint8_t nonce[kNonceLen], const string &ad, const string &ciphertextAndTag,
              string *out)
{
    if (!out || ciphertextAndTag.size() < kTagLen) {
        return false;
    }
    out->clear();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(kNonceLen), nullptr) != 1) {
            break;
        }
        const size_t ctLen = ciphertextAndTag.size() - kTagLen;
        uint8_t tag[kTagLen];
        memcpy(tag, ciphertextAndTag.data() + ctLen, kTagLen);
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kTagLen), tag) != 1) {
            break;
        }
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const uint8_t *>(key.data()), nonce) != 1) {
            break;
        }
        int len = 0;
        if (!ad.empty()) {
            if (EVP_DecryptUpdate(ctx, nullptr, &len, reinterpret_cast<const uint8_t *>(ad.data()),
                                  static_cast<int>(ad.size()))
                != 1) {
                break;
            }
        }
        out->resize(ctLen);
        uint8_t *outPtr = out->empty() ? nullptr : reinterpret_cast<uint8_t *>(&(*out)[0]);
        int outLen = 0;
        if (ctLen > 0) {
            if (EVP_DecryptUpdate(ctx, outPtr, &outLen, reinterpret_cast<const uint8_t *>(ciphertextAndTag.data()),
                                  static_cast<int>(ctLen))
                != 1) {
                break;
            }
        }
        int finalLen = 0;
        if (EVP_DecryptFinal_ex(ctx, outPtr ? outPtr + outLen : nullptr, &finalLen) != 1) {
            break;
        }
        out->resize(static_cast<size_t>(outLen + finalLen));
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        out->clear();
    }
    return ok;
}

// RFC 5869 HKDF-Expand (and Extract via HMAC) — works with empty IKM/info on all backends.
string hkdfExpand(const string &prk, const string &info, size_t outLen)
{
    if (prk.empty() || outLen == 0) {
        return string();
    }
    string out;
    out.reserve(outLen);
    string t;
    uint8_t counter = 1;
    while (out.size() < outLen) {
        string blockIn = t;
        blockIn.append(info);
        blockIn.push_back(static_cast<char>(counter++));
        unsigned int mdLen = 0;
        unsigned char md[EVP_MAX_MD_SIZE];
        if (!HMAC(EVP_sha256(), prk.data(), static_cast<int>(prk.size()),
                  reinterpret_cast<const unsigned char *>(blockIn.data()), blockIn.size(), md, &mdLen)) {
            return string();
        }
        t.assign(reinterpret_cast<char *>(md), mdLen);
        out.append(t);
    }
    out.resize(outLen);
    return out;
}

string hkdfSha256(const string &ikm, const string &salt, const string &info, size_t outLen)
{
    string realSalt = salt;
    if (realSalt.empty()) {
        realSalt.assign(kHashLen, '\0');
    }
    unsigned int prkLen = 0;
    unsigned char prk[EVP_MAX_MD_SIZE];
    if (!HMAC(EVP_sha256(), realSalt.data(), static_cast<int>(realSalt.size()),
              reinterpret_cast<const unsigned char *>(ikm.data()), ikm.size(), prk, &prkLen)) {
        return string();
    }
    return hkdfExpand(string(reinterpret_cast<char *>(prk), prkLen), info, outLen);
}

bool writeFrame(shared_ptr<SocketLike> sock, const string &payload, string *error)
{
    if (!sock || payload.size() > kMaxFramePayload) {
        if (error) {
            *error = "frame too large or null socket";
        }
        return false;
    }
    const uint16_t len = static_cast<uint16_t>(payload.size());
    char hdr[2] = {static_cast<char>((len >> 8) & 0xff), static_cast<char>(len & 0xff)};
    if (sock->sendall(hdr, 2) != 2) {
        if (error) {
            *error = "failed to send frame header";
        }
        return false;
    }
    if (!payload.empty() && sock->sendall(payload) != static_cast<int32_t>(payload.size())) {
        if (error) {
            *error = "failed to send frame payload";
        }
        return false;
    }
    return true;
}

bool readFrame(shared_ptr<SocketLike> sock, string *payload, string *error)
{
    if (!sock || !payload) {
        if (error) {
            *error = "null socket or payload";
        }
        return false;
    }
    char hdr[2];
    if (sock->recvall(hdr, 2) != 2) {
        if (error) {
            *error = "failed to read frame header";
        }
        return false;
    }
    const uint16_t len = (static_cast<uint16_t>(static_cast<uint8_t>(hdr[0])) << 8)
            | static_cast<uint16_t>(static_cast<uint8_t>(hdr[1]));
    payload->assign(static_cast<size_t>(len), '\0');
    if (len > 0 && sock->recvall(&(*payload)[0], len) != static_cast<int32_t>(len)) {
        if (error) {
            *error = "failed to read frame payload";
        }
        payload->clear();
        return false;
    }
    return true;
}

}  // namespace

NoiseKey NoiseKey::generate()
{
    NoiseKey key;
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!pctx) {
        return key;
    }
    EVP_PKEY *pkey = nullptr;
    if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return key;
    }
    EVP_PKEY_CTX_free(pctx);

    key.privateKey.resize(kDhLen);
    key.publicKey.resize(kDhLen);
    size_t privLen = kDhLen;
    size_t pubLen = kDhLen;
    if (EVP_PKEY_get_raw_private_key(pkey, reinterpret_cast<uint8_t *>(&key.privateKey[0]), &privLen) != 1
        || privLen != kDhLen
        || EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<uint8_t *>(&key.publicKey[0]), &pubLen) != 1
        || pubLen != kDhLen) {
        key.privateKey.clear();
        key.publicKey.clear();
    }
    EVP_PKEY_free(pkey);
    return key;
}

NoiseKey NoiseKey::fromPrivateKey(const string &privateKey32)
{
    NoiseKey key;
    if (privateKey32.size() != kDhLen) {
        return key;
    }
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                  reinterpret_cast<const uint8_t *>(privateKey32.data()), kDhLen);
    if (!pkey) {
        return key;
    }
    key.privateKey = privateKey32;
    key.publicKey.resize(kDhLen);
    size_t pubLen = kDhLen;
    if (EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<uint8_t *>(&key.publicKey[0]), &pubLen) != 1
        || pubLen != kDhLen) {
        key.privateKey.clear();
        key.publicKey.clear();
    }
    EVP_PKEY_free(pkey);
    return key;
}

string NoiseKey::dh(const string &privateKey32, const string &peerPublicKey32)
{
    if (privateKey32.size() != kDhLen || peerPublicKey32.size() != kDhLen) {
        return string();
    }
    EVP_PKEY *priv = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                  reinterpret_cast<const uint8_t *>(privateKey32.data()), kDhLen);
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                 reinterpret_cast<const uint8_t *>(peerPublicKey32.data()), kDhLen);
    if (!priv || !peer) {
        EVP_PKEY_free(priv);
        EVP_PKEY_free(peer);
        return string();
    }
    string shared;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(priv, nullptr);
    if (ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, peer) > 0) {
        size_t len = kDhLen;
        shared.resize(kDhLen);
        if (EVP_PKEY_derive(ctx, reinterpret_cast<uint8_t *>(&shared[0]), &len) <= 0 || len != kDhLen) {
            shared.clear();
        }
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(priv);
    EVP_PKEY_free(peer);
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

bool NoiseCipherState::rekey()
{
    if (!m_hasKey) {
        return false;
    }
    uint8_t nonce[kNonceLen];
    writeNonce(nonce, ~uint64_t(0));
    const string zeros(kHashLen, '\0');
    string out;
    if (!aeadSeal(m_key, nonce, string(), zeros, &out) || out.size() < kHashLen) {
        return false;
    }
    m_key = out.substr(0, kHashLen);
    return true;
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
    const uint64_t n = m_nonce;
    uint8_t nonce[kNonceLen];
    writeNonce(nonce, n);
    string out;
    if (!aeadSeal(m_key, nonce, ad, plaintext, &out)) {
        return string();
    }
    ++m_nonce;
    if (outNonce) {
        *outNonce = n;
    }
    return out;
}

string NoiseCipherState::decryptWithAd(const string &ad, const string &ciphertextAndTag)
{
    m_lastDecryptOk = false;
    if (!m_hasKey || ciphertextAndTag.size() < kTagLen) {
        return string();
    }
    uint8_t nonceBuf[kNonceLen];
    writeNonce(nonceBuf, m_nonce);
    string out;
    if (!aeadOpen(m_key, nonceBuf, ad, ciphertextAndTag, &out)) {
        return string();
    }
    ++m_nonce;
    m_lastDecryptOk = true;
    return out;
}

string NoiseCipherState::decryptWithAd(const string &ad, const string &ciphertextAndTag, uint64_t nonce)
{
    m_lastDecryptOk = false;
    if (!m_hasKey || ciphertextAndTag.size() < kTagLen) {
        return string();
    }
    const uint64_t savedHighest = m_highestRemoteNonce;
    const uint64_t savedWindow = m_replayWindow;
    if (!acceptIncomingNonce(nonce)) {
        return string();
    }
    uint8_t nonceBuf[kNonceLen];
    writeNonce(nonceBuf, nonce);
    string out;
    if (!aeadOpen(m_key, nonceBuf, ad, ciphertextAndTag, &out)) {
        m_highestRemoteNonce = savedHighest;
        m_replayWindow = savedWindow;
        return string();
    }
    m_lastDecryptOk = true;
    return out;
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
                                     const string &remoteStaticPublic, const string &psk, const string &prologue)
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
    if (pattern != NoisePattern::PSK_XX && !psk.empty()) {
        m_error = "only PSK_XX takes a PSK";
        return false;
    }
    if (pattern == NoisePattern::IK && role == NoiseRole::Initiator && m_rs.size() != kDhLen) {
        m_error = "IK initiator requires remote static public key";
        return false;
    }

    const char *name = kProtocolXX;
    if (pattern == NoisePattern::PSK_XX) {
        name = kProtocolPskXX;
    } else if (pattern == NoisePattern::IK) {
        name = kProtocolIK;
    }
    const string protocolName(name);
    if (protocolName.size() <= kHashLen) {
        m_h.assign(kHashLen, '\0');
        memcpy(&m_h[0], protocolName.data(), protocolName.size());
    } else {
        m_h = sha256(protocolName);
    }
    m_ck = m_h;

    // Noise Initialize always MixHash(prologue), including empty prologue.
    mixHash(prologue);

    if (pattern == NoisePattern::PSK_XX) {
        mixKeyAndHash(psk);
    }

    // IK pre-message: <- s
    if (pattern == NoisePattern::IK) {
        if (role == NoiseRole::Initiator) {
            mixHash(m_rs);
        } else {
            mixHash(m_s.publicKey);
        }
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

void NoiseHandshakeState::mixKeyAndHash(const string &material)
{
    mixKey(material);
    mixHash(material);
}

string NoiseHandshakeState::encryptAndHash(const string &plaintext)
{
    string ciphertext;
    if (m_cs.hasKey()) {
        ciphertext = m_cs.encryptWithAd(m_h, plaintext);
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
    return hkdfSha256(inputKeyMaterial, chainingKey, string(), static_cast<size_t>(numOutputs) * kHashLen);
}

bool NoiseHandshakeState::checkRemoteStatic(const string &expectedRs)
{
    if (!expectedRs.empty() && expectedRs != m_rs) {
        m_error = "remote static public key mismatch";
        return false;
    }
    return true;
}

bool NoiseHandshakeState::writeMessage(const string &payload, string *outMessage)
{
    if (!outMessage || m_complete) {
        m_error = "handshake already complete or null output";
        return false;
    }
    string message;
    const bool initiator = (m_role == NoiseRole::Initiator);
    const bool isIk = (m_pattern == NoisePattern::IK);

    if (initiator && m_msgIndex == 0 && !isIk) {
        // XX / PSK_XX: -> e
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

    if (initiator && m_msgIndex == 0 && isIk) {
        // IK: -> e, es, s, ss
        m_e = NoiseKey::generate();
        if (!m_e.isValid()) {
            m_error = "failed to generate ephemeral key";
            return false;
        }
        message += m_e.publicKey;
        mixHash(m_e.publicKey);
        const string &es = NoiseKey::dh(m_e.privateKey, m_rs);
        if (es.empty()) {
            m_error = "es DH failed";
            return false;
        }
        mixKey(es);
        const string &encS = encryptAndHash(m_s.publicKey);
        if (!m_error.empty() || encS.empty()) {
            if (m_error.empty()) {
                m_error = "encrypt static key failed";
            }
            return false;
        }
        message += encS;
        const string &ss = NoiseKey::dh(m_s.privateKey, m_rs);
        if (ss.empty()) {
            m_error = "ss DH failed";
            return false;
        }
        mixKey(ss);
        const string &cipherPayload = encryptAndHash(payload);
        if (!m_error.empty()) {
            return false;
        }
        message += cipherPayload;
        *outMessage = message;
        ++m_msgIndex;
        return true;
    }

    if (!initiator && m_msgIndex == 1 && !isIk) {
        // XX / PSK_XX: -> e, ee, s, es
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

    if (!initiator && m_msgIndex == 1 && isIk) {
        // IK: <- e, ee, se
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
        const string &se = NoiseKey::dh(m_e.privateKey, m_rs);
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

    if (initiator && m_msgIndex == 2 && !isIk) {
        // XX / PSK_XX: -> s, se
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
    const bool isIk = (m_pattern == NoisePattern::IK);

    auto take = [&](size_t n) -> string {
        if (pos + n > message.size()) {
            return string();
        }
        string part = message.substr(pos, n);
        pos += n;
        return part;
    };

    if (!initiator && m_msgIndex == 0 && !isIk) {
        // XX / PSK_XX: <- e
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

    if (!initiator && m_msgIndex == 0 && isIk) {
        // IK: <- e, es, s, ss
        m_re = take(kDhLen);
        if (m_re.size() != kDhLen) {
            m_error = "missing remote ephemeral";
            return false;
        }
        mixHash(m_re);
        const string &es = NoiseKey::dh(m_s.privateKey, m_re);
        if (es.empty()) {
            m_error = "es DH failed";
            return false;
        }
        mixKey(es);
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
            if (!checkRemoteStatic(expectedRs)) {
                return false;
            }
        }
        const string &ss = NoiseKey::dh(m_s.privateKey, m_rs);
        if (ss.empty()) {
            m_error = "ss DH failed";
            return false;
        }
        mixKey(ss);
        const string &cipherPayload = message.substr(pos);
        *outPayload = decryptAndHash(cipherPayload);
        if (!m_error.empty()) {
            return false;
        }
        ++m_msgIndex;
        return true;
    }

    if (initiator && m_msgIndex == 1 && !isIk) {
        // XX / PSK_XX: <- e, ee, s, es
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
            if (!checkRemoteStatic(expectedRs)) {
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

    if (initiator && m_msgIndex == 1 && isIk) {
        // IK: <- e, ee, se
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
        const string &se = NoiseKey::dh(m_s.privateKey, m_re);
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

    if (!initiator && m_msgIndex == 2 && !isIk) {
        // XX / PSK_XX: <- s, se
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
            if (!checkRemoteStatic(expectedRs)) {
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
    return hkdfSha256(secret, salt, info, outLen);
}

class NoiseStreamPrivate
{
public:
    shared_ptr<SocketLike> backend;
    NoiseHandshakeState handshake;
    NoiseCipherState sendCipher;
    NoiseCipherState recvCipher;
    NoiseRole role;
    NoisePattern pattern;
    bool handshakeDone;
    string peerPayload;
    string error;
    string recvBuf;
    Socket::SocketError sockError;

    NoiseStreamPrivate()
        : role(NoiseRole::Initiator)
        , pattern(NoisePattern::XX)
        , handshakeDone(false)
        , sockError(Socket::NoError)
    {
    }
};

NoiseStream::NoiseStream(shared_ptr<SocketLike> backend)
    : d_ptr(new NoiseStreamPrivate)
{
    NG_D(NoiseStream);
    d->backend = backend;
}

NoiseStream::~NoiseStream()
{
    delete d_ptr;
}

bool NoiseStream::initialize(NoisePattern pattern, NoiseRole role, const NoiseKey &localStatic,
                             const string &remoteStaticPublic, const string &psk, const string &prologue)
{
    NG_D(NoiseStream);
    d->error.clear();
    d->handshakeDone = false;
    d->peerPayload.clear();
    d->recvBuf.clear();
    d->role = role;
    d->pattern = pattern;
    if (!d->handshake.initialize(pattern, role, localStatic, remoteStaticPublic, psk, prologue)) {
        d->error = d->handshake.errorString();
        return false;
    }
    return true;
}

bool NoiseStream::handshake(const string &payload)
{
    NG_D(NoiseStream);
    d->error.clear();
    if (!d->backend) {
        d->error = "no backend socket";
        return false;
    }
    if (d->handshakeDone) {
        return true;
    }

    auto fail = [&](const string &msg) -> bool {
        d->error = msg.empty() ? d->handshake.errorString() : msg;
        return false;
    };

    if (d->role == NoiseRole::Initiator) {
        string msg;
        if (!d->handshake.writeMessage(payload, &msg)) {
            return fail(string());
        }
        if (!writeFrame(d->backend, msg, &d->error)) {
            return false;
        }
        string reply;
        if (!readFrame(d->backend, &reply, &d->error)) {
            return false;
        }
        string peerPayload;
        if (!d->handshake.readMessage(reply, &peerPayload)) {
            return fail(string());
        }
        d->peerPayload = peerPayload;
        if (d->pattern != NoisePattern::IK) {
            string msg3;
            if (!d->handshake.writeMessage(string(), &msg3)) {
                return fail(string());
            }
            if (!writeFrame(d->backend, msg3, &d->error)) {
                return false;
            }
        }
    } else {
        string msg;
        if (!readFrame(d->backend, &msg, &d->error)) {
            return false;
        }
        string peerPayload;
        if (!d->handshake.readMessage(msg, &peerPayload)) {
            return fail(string());
        }
        d->peerPayload = peerPayload;
        string reply;
        if (!d->handshake.writeMessage(payload, &reply)) {
            return fail(string());
        }
        if (!writeFrame(d->backend, reply, &d->error)) {
            return false;
        }
        if (d->pattern != NoisePattern::IK) {
            string msg3;
            if (!readFrame(d->backend, &msg3, &d->error)) {
                return false;
            }
            string ignored;
            if (!d->handshake.readMessage(msg3, &ignored)) {
                return fail(string());
            }
        }
    }

    if (!d->handshake.isComplete() || !d->handshake.split(&d->sendCipher, &d->recvCipher)) {
        return fail(string());
    }
    d->handshakeDone = true;
    return true;
}

bool NoiseStream::isHandshakeComplete() const
{
    NG_D(const NoiseStream);
    return d->handshakeDone;
}

string NoiseStream::peerHandshakePayload() const
{
    NG_D(const NoiseStream);
    return d->peerPayload;
}

string NoiseStream::remoteStaticPublic() const
{
    NG_D(const NoiseStream);
    return d->handshake.remoteStaticPublic();
}

string NoiseStream::handshakeHash() const
{
    NG_D(const NoiseStream);
    return d->handshake.handshakeHash();
}

bool NoiseStream::sendMessage(const string &plaintext)
{
    NG_D(NoiseStream);
    d->error.clear();
    if (!d->handshakeDone) {
        d->error = "handshake not complete";
        return false;
    }
    const string cipher = d->sendCipher.encryptWithAd(string(), plaintext);
    if (cipher.empty() && !plaintext.empty()) {
        d->error = "encrypt failed";
        return false;
    }
    // Empty plaintext still produces a 16-byte tag.
    if (cipher.size() < kTagLen) {
        d->error = "encrypt failed";
        return false;
    }
    return writeFrame(d->backend, cipher, &d->error);
}

string NoiseStream::recvMessage()
{
    NG_D(NoiseStream);
    d->error.clear();
    if (!d->handshakeDone) {
        d->error = "handshake not complete";
        return string();
    }
    string frame;
    if (!readFrame(d->backend, &frame, &d->error)) {
        return string();
    }
    const string plain = d->recvCipher.decryptWithAd(string(), frame);
    if (!d->recvCipher.lastDecryptOk()) {
        d->error = "decrypt failed";
        return string();
    }
    return plain;
}

shared_ptr<SocketLike> NoiseStream::backend() const
{
    NG_D(const NoiseStream);
    return d->backend;
}

string NoiseStream::errorString() const
{
    NG_D(const NoiseStream);
    if (!d->error.empty()) {
        return d->error;
    }
    return d->backend ? d->backend->errorString() : string();
}

Socket::SocketError NoiseStream::error() const
{
    NG_D(const NoiseStream);
    if (!d->error.empty()) {
        return Socket::UnknownSocketError;
    }
    return d->backend ? d->backend->error() : Socket::SocketAccessError;
}

bool NoiseStream::isValid() const
{
    NG_D(const NoiseStream);
    return d->backend && d->backend->isValid();
}

HostAddress NoiseStream::localAddress() const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->localAddress() : HostAddress();
}

uint16_t NoiseStream::localPort() const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->localPort() : 0;
}

HostAddress NoiseStream::peerAddress() const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->peerAddress() : HostAddress();
}

string NoiseStream::peerName() const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->peerName() : string();
}

uint16_t NoiseStream::peerPort() const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->peerPort() : 0;
}

intptr_t NoiseStream::fileno() const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->fileno() : -1;
}

Socket::SocketType NoiseStream::type() const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->type() : Socket::UnknownSocketType;
}

Socket::SocketState NoiseStream::state() const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->state() : Socket::UnconnectedState;
}

HostAddress::NetworkLayerProtocol NoiseStream::protocol() const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->protocol() : HostAddress::UnknownNetworkLayerProtocol;
}

string NoiseStream::localAddressURI() const
{
    NG_D(const NoiseStream);
    return d->backend ? ("noise+" + d->backend->localAddressURI()) : string();
}

string NoiseStream::peerAddressURI() const
{
    NG_D(const NoiseStream);
    return d->backend ? ("noise+" + d->backend->peerAddressURI()) : string();
}

shared_ptr<SocketLike> NoiseStream::accept()
{
    NG_D(NoiseStream);
    return d->backend ? d->backend->accept() : shared_ptr<SocketLike>();
}

Socket *NoiseStream::acceptRaw()
{
    NG_D(NoiseStream);
    return d->backend ? d->backend->acceptRaw() : nullptr;
}

bool NoiseStream::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    NG_D(NoiseStream);
    return d->backend && d->backend->bind(address, port, mode);
}

bool NoiseStream::bind(uint16_t port, Socket::BindMode mode)
{
    NG_D(NoiseStream);
    return d->backend && d->backend->bind(port, mode);
}

bool NoiseStream::connect(const HostAddress &addr, uint16_t port)
{
    NG_D(NoiseStream);
    return d->backend && d->backend->connect(addr, port);
}

bool NoiseStream::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    NG_D(NoiseStream);
    return d->backend && d->backend->connect(hostName, port, dnsCache);
}

void NoiseStream::close()
{
    NG_D(NoiseStream);
    if (d->backend) {
        d->backend->close();
    }
}

void NoiseStream::abort()
{
    NG_D(NoiseStream);
    if (d->backend) {
        d->backend->abort();
    }
}

bool NoiseStream::listen(int backlog)
{
    NG_D(NoiseStream);
    return d->backend && d->backend->listen(backlog);
}

bool NoiseStream::setOption(Socket::SocketOption option, int value)
{
    NG_D(NoiseStream);
    return d->backend && d->backend->setOption(option, value);
}

int NoiseStream::option(Socket::SocketOption option) const
{
    NG_D(const NoiseStream);
    return d->backend ? d->backend->option(option) : 0;
}

int32_t NoiseStream::peek(char *data, int32_t size)
{
    NG_D(NoiseStream);
    if (!data || size <= 0) {
        return -1;
    }
    if (d->recvBuf.empty()) {
        return 0;
    }
    const int32_t n = min<int32_t>(size, static_cast<int32_t>(d->recvBuf.size()));
    memcpy(data, d->recvBuf.data(), static_cast<size_t>(n));
    return n;
}

int32_t NoiseStream::peekRaw(char *data, int32_t size)
{
    NG_D(NoiseStream);
    return d->backend ? d->backend->peekRaw(data, size) : -1;
}

int32_t NoiseStream::recv(char *data, int32_t size)
{
    NG_D(NoiseStream);
    if (!data || size <= 0) {
        return -1;
    }
    if (d->recvBuf.empty()) {
        const string msg = recvMessage();
        if (msg.empty() && !d->error.empty()) {
            return -1;
        }
        d->recvBuf = msg;
        if (d->recvBuf.empty()) {
            return 0;
        }
    }
    const int32_t n = min<int32_t>(size, static_cast<int32_t>(d->recvBuf.size()));
    memcpy(data, d->recvBuf.data(), static_cast<size_t>(n));
    d->recvBuf.erase(0, static_cast<size_t>(n));
    return n;
}

int32_t NoiseStream::recvall(char *data, int32_t size)
{
    if (!data || size <= 0) {
        return -1;
    }
    int32_t got = 0;
    while (got < size) {
        const int32_t n = recv(data + got, size - got);
        if (n < 0) {
            return got > 0 ? got : -1;
        }
        if (n == 0) {
            break;
        }
        got += n;
    }
    return got;
}

int32_t NoiseStream::send(const char *data, int32_t size)
{
    return sendall(data, size);
}

int32_t NoiseStream::sendall(const char *data, int32_t size)
{
    if (!data || size < 0) {
        return -1;
    }
    if (!sendMessage(string(data, static_cast<size_t>(size)))) {
        return -1;
    }
    return size;
}

string NoiseStream::recv(int32_t size)
{
    if (size <= 0) {
        return string();
    }
    string buf(static_cast<size_t>(size), '\0');
    const int32_t n = recv(&buf[0], size);
    if (n <= 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

string NoiseStream::recvall(int32_t size)
{
    if (size <= 0) {
        return string();
    }
    string buf(static_cast<size_t>(size), '\0');
    const int32_t n = recvall(&buf[0], size);
    if (n <= 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

int32_t NoiseStream::send(const string &data)
{
    return send(data.data(), static_cast<int32_t>(data.size()));
}

int32_t NoiseStream::sendall(const string &data)
{
    return sendall(data.data(), static_cast<int32_t>(data.size()));
}

shared_ptr<SocketLike> asSocketLike(shared_ptr<NoiseStream> s)
{
    return dynamic_pointer_cast<SocketLike>(s);
}

}  // namespace qtng
