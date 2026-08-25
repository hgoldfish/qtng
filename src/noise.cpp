#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qtng/private/noise_replay_p.h"
#include "qtng/noise.h"
#include "qtng/md.h"
#include "qtng/private/openssl_raii.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

#if __cplusplus < 201703L
// ODR-use requires an out-of-class definition for static constexpr members in C++11/14.
constexpr std::uint64_t NoiseCipherState::MaxNonce;
#endif

namespace {

string noiseHandshakeName(NoisePattern pattern, NoisePskModifier pskModifier)
{
    const char *pat = "XX";
    switch (pattern) {
    case NoisePattern::IK:
        pat = "IK";
        break;
    case NoisePattern::XK:
        pat = "XK";
        break;
    case NoisePattern::KK:
        pat = "KK";
        break;
    case NoisePattern::XX:
        pat = "XX";
        break;
    }
    const char *mod = "";
    switch (pskModifier) {
    case NoisePskModifier::Psk0:
        mod = "psk0";
        break;
    case NoisePskModifier::Psk1:
        mod = "psk1";
        break;
    case NoisePskModifier::Psk2:
        mod = "psk2";
        break;
    case NoisePskModifier::Psk3:
        mod = "psk3";
        break;
    case NoisePskModifier::None:
        break;
    }
    string name(pat);
    name += mod;
    return name;
}

NG_LOGGER("qtng.noise");

const size_t kDhLen = 32;
const size_t kCipherKeyLen = 32;
const size_t kPskLen = 32;
const size_t kTagLen = 16;
const size_t kNonceLen = 12;
const size_t kWireNonceLen = 8;
const size_t kMaxFramePayload = 65535;
const uint64_t kRekeyNonce = ~uint64_t(0);

inline bool transportNonceAllowed(uint64_t n)
{
    return n <= NoiseCipherState::MaxNonce;
}

enum class Token : uint8_t { E, S, Ee, Es, Se, Ss, Psk };

struct PatternTokens {
    bool preInitiatorS = false;
    bool preResponderS = false;
    int nMessages = 0;
    array<vector<Token>, 3> messages{};
};

PatternTokens patternTokens(NoisePattern pattern)
{
    PatternTokens p;
    switch (pattern) {
    case NoisePattern::XX:
        p.nMessages = 3;
        p.messages[0] = {Token::E};
        p.messages[1] = {Token::E, Token::Ee, Token::S, Token::Es};
        p.messages[2] = {Token::S, Token::Se};
        break;
    case NoisePattern::IK:
        p.preResponderS = true;
        p.nMessages = 2;
        p.messages[0] = {Token::E, Token::Es, Token::S, Token::Ss};
        p.messages[1] = {Token::E, Token::Ee, Token::Se};
        break;
    case NoisePattern::XK:
        p.preResponderS = true;
        p.nMessages = 3;
        p.messages[0] = {Token::E, Token::Es};
        p.messages[1] = {Token::E, Token::Ee};
        p.messages[2] = {Token::S, Token::Se};
        break;
    case NoisePattern::KK:
        p.preInitiatorS = true;
        p.preResponderS = true;
        p.nMessages = 2;
        p.messages[0] = {Token::E, Token::Es, Token::Ss};
        p.messages[1] = {Token::E, Token::Ee, Token::Se};
        break;
    }
    return p;
}

struct NoiseHashInfo {
    MessageDigest::Algorithm md;
    size_t hashLen;
    const char *name;
};

NoiseHashInfo noiseHashInfo(NoiseHash hash)
{
    switch (hash) {
    case NoiseHash::Sha256:
        return {MessageDigest::Sha256, 32, "SHA256"};
    case NoiseHash::Sha512:
        return {MessageDigest::Sha512, 64, "SHA512"};
    case NoiseHash::Blake2s:
        return {MessageDigest::Blake2s_256, 32, "BLAKE2s"};
    case NoiseHash::Blake2b:
        return {MessageDigest::Blake2b_512, 64, "BLAKE2b"};
    }
    return {};
}

bool probeNoiseHash(const NoiseHashInfo &info)
{
    return info.hashLen != 0 && MessageDigest::digest(string(), info.md).size() == info.hashLen;
}

inline bool isNoiseAead(Aead::Algorithm cipher)
{
    return cipher == Aead::ChaCha20Poly1305 || cipher == Aead::Aes256Gcm;
}

// Noise nonce: 4 zero bytes || 64-bit counter. ChaChaPoly is little-endian; AESGCM is big-endian.
string encodeNonce(Aead::Algorithm algo, uint64_t n)
{
    string nonce(kNonceLen, '\0');
    if (algo == Aead::Aes256Gcm) {
        ngToBigEndian(n, &nonce[4]);
    } else {
        ngToLittleEndian(n, &nonce[4]);
    }
    return nonce;
}

bool writeFrame(shared_ptr<SocketLike> sock, const string &payload, string *error)
{
    if (!sock || payload.size() > kMaxFramePayload) {
        ngWarning() << "frame too large or null socket";
        if (error) {
            *error = "frame too large or null socket";
        }
        return false;
    }
    const uint16_t len = static_cast<uint16_t>(payload.size());
    char hdr[2] = {static_cast<char>((len >> 8) & 0xff), static_cast<char>(len & 0xff)};
    if (sock->sendall(hdr, 2) != 2) {
        ngDebug() << "failed to send frame header";
        if (error) {
            *error = "failed to send frame header";
        }
        return false;
    }
    if (!payload.empty() && sock->sendall(payload) != static_cast<int32_t>(payload.size())) {
        ngDebug() << "failed to send frame payload";
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
        ngWarning() << "null socket or payload";
        if (error) {
            *error = "null socket or payload";
        }
        return false;
    }
    char hdr[2];
    if (sock->recvall(hdr, 2) != 2) {
        ngDebug() << "failed to read frame header";
        if (error) {
            *error = "failed to read frame header";
        }
        return false;
    }
    const uint16_t len = (static_cast<uint16_t>(static_cast<uint8_t>(hdr[0])) << 8)
            | static_cast<uint16_t>(static_cast<uint8_t>(hdr[1]));
    payload->assign(static_cast<size_t>(len), '\0');
    if (len > 0 && sock->recvall(&(*payload)[0], len) != static_cast<int32_t>(len)) {
        payload->clear();
        ngDebug() << "failed to read frame payload";
        if (error) {
            *error = "failed to read frame payload";
        }
        return false;
    }
    return true;
}

string packTransport(uint64_t n, const string &ct)
{
    string out(kWireNonceLen + ct.size(), '\0');
    ngToBigEndian(n, &out[0]);
    if (!ct.empty()) {
        memcpy(&out[kWireNonceLen], ct.data(), ct.size());
    }
    return out;
}

bool unpackTransport(const string &wire, uint64_t *n, string *ct)
{
    if (!n || !ct || wire.size() < kWireNonceLen + kTagLen) {
        return false;
    }
    *n = ngFromBigEndian<uint64_t>(wire.data());
    *ct = wire.substr(kWireNonceLen);
    return true;
}

// Split transport ciphers once hs.isComplete(). Returns true when handshake
// is still in progress or split succeeded; false only on split failure.
bool splitIfHandshakeComplete(NoiseHandshakeState &hs, NoiseCipherState *send, NoiseCipherState *recv,
                              bool &ready, string &error)
{
    if (!hs.isComplete()) {
        return true;
    }
    if (!hs.split(send, recv)) {
        error = hs.errorString();
        ready = false;
        return false;
    }
    ready = true;
    return true;
}

}  // namespace

NoiseKey NoiseKey::generate()
{
    NoiseKey key;
    EvpPkeyCtxPtr pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr));
    if (!pctx) {
        ngWarning() << "NoiseKey::generate: EVP_PKEY_CTX_new_id failed";
        return key;
    }
    EVP_PKEY *rawPkey = nullptr;
    if (EVP_PKEY_keygen_init(pctx.get()) <= 0 || EVP_PKEY_keygen(pctx.get(), &rawPkey) <= 0) {
        ngWarning() << "NoiseKey::generate: X25519 keygen failed";
        return key;
    }
    EvpPkeyPtr pkey(rawPkey);

    string priv(kDhLen, '\0');
    string pub(kDhLen, '\0');
    size_t privLen = kDhLen;
    size_t pubLen = kDhLen;
    if (EVP_PKEY_get_raw_private_key(pkey.get(), reinterpret_cast<uint8_t *>(&priv[0]), &privLen) != 1
        || privLen != kDhLen
        || EVP_PKEY_get_raw_public_key(pkey.get(), reinterpret_cast<uint8_t *>(&pub[0]), &pubLen) != 1
        || pubLen != kDhLen) {
        ngWarning() << "NoiseKey::generate: failed to export raw keys";
        return key;
    }
    key.setPrivateKey(priv);
    key.setPublicKey(pub);
    return key;
}

NoiseKey NoiseKey::fromPrivateKey(const string &privateKey32)
{
    NoiseKey key;
    if (privateKey32.size() != kDhLen) {
        ngWarning() << "NoiseKey::fromPrivateKey: key must be 32 bytes, got" << privateKey32.size();
        return key;
    }
    EvpPkeyPtr pkey(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                  reinterpret_cast<const uint8_t *>(privateKey32.data()),
                                                  kDhLen));
    if (!pkey) {
        ngWarning() << "NoiseKey::fromPrivateKey: EVP_PKEY_new_raw_private_key failed";
        return key;
    }
    string pub(kDhLen, '\0');
    size_t pubLen = kDhLen;
    if (EVP_PKEY_get_raw_public_key(pkey.get(), reinterpret_cast<uint8_t *>(&pub[0]), &pubLen) != 1
        || pubLen != kDhLen) {
        ngWarning() << "NoiseKey::fromPrivateKey: failed to export public key";
        return NoiseKey();
    }
    key.setPrivateKey(privateKey32);
    key.setPublicKey(pub);
    return key;
}

string NoiseKey::dh(const string &privateKey32, const string &peerPublicKey32)
{
    if (privateKey32.size() != kDhLen || peerPublicKey32.size() != kDhLen) {
        ngWarning() << "NoiseKey::dh: keys must be 32 bytes";
        return string();
    }
    EvpPkeyPtr priv(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                 reinterpret_cast<const uint8_t *>(privateKey32.data()),
                                                 kDhLen));
    EvpPkeyPtr peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                reinterpret_cast<const uint8_t *>(peerPublicKey32.data()),
                                                kDhLen));
    if (!priv || !peer) {
        ngWarning() << "NoiseKey::dh: failed to import X25519 keys";
        return string();
    }
    string shared;
    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new(priv.get(), nullptr));
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0 || EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) <= 0) {
        ngWarning() << "NoiseKey::dh: derive init failed";
        return string();
    }
    size_t len = kDhLen;
    shared.resize(kDhLen);
    if (EVP_PKEY_derive(ctx.get(), reinterpret_cast<uint8_t *>(&shared[0]), &len) <= 0 || len != kDhLen) {
        ngWarning() << "NoiseKey::dh: EVP_PKEY_derive failed";
        return string();
    }
    return shared;
}

NoiseConfig::NoiseConfig(const string &localPrivateKey)
    : localStatic_(localPrivateKey.empty() ? NoiseKey::generate() : NoiseKey::fromPrivateKey(localPrivateKey))
{
}

NoiseConfig::NoiseConfig(const NoiseKey &key)
    : localStatic_(key)
{
}

string noiseProtocolName(NoisePattern pattern, NoisePskModifier pskModifier, Aead::Algorithm cipher,
                         NoiseHash hash)
{
    const NoiseHashInfo hashInfo = noiseHashInfo(hash);
    const char *hashName = hashInfo.name ? hashInfo.name : "SHA256";
    const char *ci = (cipher == Aead::Aes256Gcm) ? "AESGCM" : "ChaChaPoly";
    string name("Noise_");
    name += noiseHandshakeName(pattern, pskModifier);
    name += "_25519_";
    name += ci;
    name += "_";
    name += hashName;
    return name;
}

bool parseNoiseProtocolName(const string &name, NoisePattern *pattern, NoisePskModifier *pskModifier,
                            Aead::Algorithm *cipher, NoiseHash *hash, string *errorMessage)
{
    const string m = utils::toLower(utils::trimmed(name));
    if (m.empty()) {
        if (errorMessage) {
            *errorMessage = "empty Noise protocol name";
        }
        return false;
    }

    // Match noiseProtocolName() output for combinations initialize() accepts.
    // (noiseProtocolName itself does not validate PSK slot vs pattern.)
    static const NoisePattern kPatterns[] = {
        NoisePattern::XX,
        NoisePattern::IK,
        NoisePattern::XK,
        NoisePattern::KK,
    };
    static const NoisePskModifier kModifiers[] = {
        NoisePskModifier::None,
        NoisePskModifier::Psk0,
        NoisePskModifier::Psk1,
        NoisePskModifier::Psk2,
        NoisePskModifier::Psk3,
    };
    static const Aead::Algorithm kCiphers[] = {
        Aead::ChaCha20Poly1305,
        Aead::Aes256Gcm,
    };
    static const NoiseHash kHashes[] = {
        NoiseHash::Sha256,
        NoiseHash::Sha512,
        NoiseHash::Blake2s,
        NoiseHash::Blake2b,
    };

    for (NoisePattern pat : kPatterns) {
        const int maxSlot = patternTokens(pat).nMessages;
        for (NoisePskModifier mod : kModifiers) {
            if (mod != NoisePskModifier::None) {
                const int slot = static_cast<int>(mod) - static_cast<int>(NoisePskModifier::Psk0);
                if (slot < 0 || slot > maxSlot) {
                    continue;
                }
            }
            for (Aead::Algorithm algo : kCiphers) {
                for (NoiseHash digest : kHashes) {
                    if (utils::toLower(noiseProtocolName(pat, mod, algo, digest)) == m) {
                        *pattern = pat;
                        *pskModifier = mod;
                        *cipher = algo;
                        *hash = digest;
                        return true;
                    }
                }
            }
        }
    }

    if (errorMessage) {
        *errorMessage = "unknown Noise protocol name `" + name + "`";
    }
    return false;
}

bool applyNoiseProtocolName(const string &name, NoiseConfig *config, string *errorMessage)
{
    NoisePattern pattern = NoisePattern::XX;
    NoisePskModifier pskModifier = NoisePskModifier::None;
    Aead::Algorithm cipher = Aead::ChaCha20Poly1305;
    NoiseHash hash = NoiseHash::Sha256;
    if (!parseNoiseProtocolName(name, &pattern, &pskModifier, &cipher, &hash, errorMessage)) {
        return false;
    }
    config->setPattern(pattern);
    config->setPskModifier(pskModifier);
    config->setCipher(cipher);
    config->setHash(hash);
    return true;
}

class NoiseCipherStatePrivate
{
public:
    explicit NoiseCipherStatePrivate(Aead::Algorithm algo = Aead::ChaCha20Poly1305)
        : aead(algo)
        , lastDecryptOk(false)
        , nonce(0)
    {
    }

    NoiseCipherStatePrivate(const NoiseCipherStatePrivate &other)
        : aead(other.aead.algorithm())
        , lastDecryptOk(other.lastDecryptOk)
        , key(other.key)
        , nonce(other.nonce)
    {
        syncAeadKey();
    }

    NoiseCipherStatePrivate &operator=(const NoiseCipherStatePrivate &other)
    {
        if (this != &other) {
            aead.~Aead();
            new (&aead) Aead(other.aead.algorithm());
            lastDecryptOk = other.lastDecryptOk;
            key = other.key;
            nonce = other.nonce;
            syncAeadKey();
        }
        return *this;
    }

    void syncAeadKey()
    {
        if (!key.empty() && !aead.setKey(key)) {
            ngWarning() << "NoiseCipherState: AEAD setKey failed";
            key.clear();
        }
    }

    Aead aead;
    bool lastDecryptOk;
    string key;
    // 64-bit sequential counter; full semantics on NoiseCipherState in noise.h.
    uint64_t nonce;
};

NoiseCipherState::NoiseCipherState(Aead::Algorithm algo)
    : d_ptr(new NoiseCipherStatePrivate(algo))
{
}

NoiseCipherState::NoiseCipherState(const NoiseCipherState &other)
    : d_ptr(new NoiseCipherStatePrivate(*other.d_ptr))
{
}

NoiseCipherState &NoiseCipherState::operator=(const NoiseCipherState &other)
{
    if (this != &other) {
        *d_ptr = *other.d_ptr;
    }
    return *this;
}

NoiseCipherState::NoiseCipherState(NoiseCipherState &&other)
    : d_ptr(new NoiseCipherStatePrivate)
{
    std::swap(d_ptr, other.d_ptr);
}

NoiseCipherState &NoiseCipherState::operator=(NoiseCipherState &&other) noexcept
{
    std::swap(d_ptr, other.d_ptr);
    return *this;
}

NoiseCipherState::~NoiseCipherState()
{
    delete d_ptr;
}

Aead::Algorithm NoiseCipherState::algorithm() const
{
    NG_D(const NoiseCipherState);
    return d->aead.algorithm();
}

void NoiseCipherState::initializeKey(const string &key32)
{
    NG_D(NoiseCipherState);
    const Aead::Algorithm algo = d->aead.algorithm();
    d->key = (isNoiseAead(algo) && key32.size() == kCipherKeyLen) ? key32 : string();
    d->nonce = 0;
    if (!d->key.empty()) {
        d->syncAeadKey();
    }
    if (d->key.empty()) {
        if (!isNoiseAead(algo)) {
            ngDebug() << "NoiseCipherState::initializeKey: unsupported AEAD";
        } else if (key32.size() != kCipherKeyLen) {
            ngWarning() << "NoiseCipherState::initializeKey: key must be 32 bytes, got" << key32.size();
        }
    }
}

bool NoiseCipherState::hasKey() const
{
    NG_D(const NoiseCipherState);
    return !d->key.empty();
}

uint64_t NoiseCipherState::nonce() const
{
    NG_D(const NoiseCipherState);
    return d->nonce;
}

void NoiseCipherState::setNonce(uint64_t n)
{
    NG_D(NoiseCipherState);
    if (!transportNonceAllowed(n)) {
        ngWarning() << "NoiseCipherState::setNonce: nonce " << n << " exceeds MaxNonce (" << MaxNonce << ")";
        return;
    }
    d->nonce = n;
}

bool NoiseCipherState::rekey()
{
    NG_D(NoiseCipherState);
    if (d->key.empty()) {
        ngWarning() << "NoiseCipherState::rekey: no key";
        return false;
    }
    const Aead::Algorithm algo = d->aead.algorithm();
    const string zeros(kCipherKeyLen, '\0');
    string out;
    if (!d->aead.seal(encodeNonce(algo, kRekeyNonce), string(), zeros, &out) || out.size() < kCipherKeyLen) {
        ngWarning() << "NoiseCipherState::rekey: ENCRYPT(k, 2^64-1) failed";
        return false;
    }
    d->key = out.substr(0, kCipherKeyLen);
    d->syncAeadKey();
    // Noise spec CipherState.Rekey(): the new key starts with nonce 0.
    d->nonce = 0;
    return !d->key.empty();
}

string NoiseCipherState::encryptWithAd(const string &ad, const string &plaintext)
{
    uint64_t used = 0;
    return encryptWithAd(ad, plaintext, &used);
}

string NoiseCipherState::encryptWithAd(const string &ad, const string &plaintext, uint64_t *outNonce)
{
    NG_D(NoiseCipherState);
    if (d->key.empty()) {
        ngDebug() << "NoiseCipherState::encryptWithAd: no key";
        return string();
    }
    if (!transportNonceAllowed(d->nonce)) {
        ngWarning() << "NoiseCipherState::encryptWithAd: nonce exhausted";
        return string();
    }
    const uint64_t n = d->nonce;
    const Aead::Algorithm algo = d->aead.algorithm();
    string out;
    if (!d->aead.seal(encodeNonce(algo, n), ad, plaintext, &out)) {
        ngWarning() << "NoiseCipherState::encryptWithAd: AEAD seal failed n=" << n;
        return string();
    }
    ++d->nonce;
    if (outNonce) {
        *outNonce = n;
    }
    return out;
}

string NoiseCipherState::decryptWithAd(const string &ad, const string &ciphertextAndTag)
{
    NG_D(NoiseCipherState);
    const string out = decryptWithAd(ad, ciphertextAndTag, d->nonce);
    if (d->lastDecryptOk) {
        ++d->nonce;
    }
    return out;
}

string NoiseCipherState::decryptWithAd(const string &ad, const string &ciphertextAndTag, uint64_t nonce)
{
    NG_D(NoiseCipherState);
    d->lastDecryptOk = false;
    if (d->key.empty()) {
        ngDebug() << "NoiseCipherState::decryptWithAd: no key";
        return string();
    }
    if (ciphertextAndTag.size() < kTagLen) {
        ngDebug() << "NoiseCipherState::decryptWithAd: truncated";
        return string();
    }
    if (!transportNonceAllowed(nonce)) {
        ngWarning() << "NoiseCipherState::decryptWithAd: nonce " << nonce << " reserved/exhausted";
        return string();
    }
    const Aead::Algorithm algo = d->aead.algorithm();
    string out;
    if (!d->aead.open(encodeNonce(algo, nonce), ad, ciphertextAndTag, &out)) {
        ngDebug() << "NoiseCipherState::decryptWithAd: AEAD open failed n=" << nonce;
        return string();
    }
    d->lastDecryptOk = true;
    return out;
}

bool NoiseCipherState::lastDecryptOk() const
{
    NG_D(const NoiseCipherState);
    return d->lastDecryptOk;
}

class NoiseHandshakeStatePrivate
{
public:
    NoiseHandshakeStatePrivate()
        : role(NoiseRole::Initiator)
        , complete(false)
        , usesPsk(false)
        , msgIndex(0)
        , hashAlgo(MessageDigest::Sha256)
        , hashLen(32)
    {
    }

    bool mixHash(const string &data);
    bool mixKey(const string &material);
    bool mixKeyAndHash(const string &material);
    bool dhMix(const string &priv, const string &pub);
    string encryptAndHash(const string &plaintext);
    string decryptAndHash(const string &ciphertextAndTag);
    string hkdf(const string &chainingKey, const string &inputKeyMaterial, int numOutputs);
    bool checkRemoteStatic(const string &expectedRs);
    bool applyToken(Token token, bool writing, string *outMessage, const string *inMessage, size_t *pos);

    NoiseRole role;
    PatternTokens tokens;
    bool complete;
    bool usesPsk;
    int msgIndex;
    MessageDigest::Algorithm hashAlgo;
    size_t hashLen;
    string error;
    NoiseKey s;
    NoiseKey e;
    string rs;
    string re;
    string psk;
    string ck;
    string h;
    NoiseCipherState cs;
};

NoiseHandshakeState::NoiseHandshakeState()
    : d_ptr(new NoiseHandshakeStatePrivate)
{
}

NoiseHandshakeState::~NoiseHandshakeState()
{
    delete d_ptr;
}

bool NoiseHandshakeState::initialize(const NoiseConfig &config)
{
    NG_D(NoiseHandshakeState);
    d->error.clear();
    d->complete = false;
    d->msgIndex = 0;
    d->role = config.role();
    d->s = config.localStatic();
    d->e = NoiseKey();
    d->rs = config.remoteStaticPublic();
    d->re.clear();
    d->psk = config.psk();
    d->cs = NoiseCipherState(config.cipher());
    d->tokens = patternTokens(config.pattern());
    d->usesPsk = (config.pskModifier() != NoisePskModifier::None);
    d->h.clear();
    d->ck.clear();

    const NoiseHashInfo hashInfo = noiseHashInfo(config.hash());
    if (!hashInfo.name) {
        ngWarning() << "unsupported Noise hash";
        d->error = "unsupported Noise hash";
        return false;
    }
    d->hashAlgo = hashInfo.md;
    d->hashLen = hashInfo.hashLen;

    if (d->tokens.nMessages <= 0) {
        ngWarning() << "unsupported Noise pattern";
        d->error = "unsupported Noise pattern";
        return false;
    }
    if (!isNoiseAead(config.cipher())) {
        ngWarning() << "Noise AEAD must be ChaCha20Poly1305 or Aes256Gcm";
        d->error = "Noise AEAD must be ChaCha20Poly1305 or Aes256Gcm";
        return false;
    }
    if (!d->s.isValid()) {
        ngWarning() << "local static key is invalid";
        d->error = "local static key is invalid";
        return false;
    }
    if (!d->rs.empty() && d->rs.size() != kDhLen) {
        ngWarning() << "remote static public key must be 32 bytes";
        d->error = "remote static public key must be 32 bytes";
        return false;
    }
    const bool initiator = (config.role() == NoiseRole::Initiator);
    if (d->tokens.preResponderS && initiator && d->rs.size() != kDhLen) {
        ngWarning() << "initiator requires remote static public key";
        d->error = "initiator requires remote static public key";
        return false;
    }
    if (d->tokens.preInitiatorS && !initiator && d->rs.size() != kDhLen) {
        ngWarning() << "responder requires remote static public key";
        d->error = "responder requires remote static public key";
        return false;
    }
    if (d->usesPsk) {
        if (config.psk().size() != kPskLen) {
            ngWarning() << "PSK must be 32 bytes";
            d->error = "PSK must be 32 bytes";
            return false;
        }
        const int slot = static_cast<int>(config.pskModifier()) - static_cast<int>(NoisePskModifier::Psk0);
        if (slot < 0 || slot > d->tokens.nMessages) {
            ngWarning() << "PSK modifier is not valid for this pattern";
            d->error = "PSK modifier is not valid for this pattern";
            return false;
        }
        if (slot == 0) {
            d->tokens.messages[0].insert(d->tokens.messages[0].begin(), Token::Psk);
        } else {
            d->tokens.messages[static_cast<size_t>(slot - 1)].push_back(Token::Psk);
        }
    } else if (!config.psk().empty()) {
        ngWarning() << "PSK requires a psk0/psk1/psk2/psk3 modifier";
        d->error = "PSK requires a psk0/psk1/psk2/psk3 modifier";
        return false;
    }
    if (!probeNoiseHash(hashInfo)) {
        const string msg = string("Noise hash ") + hashInfo.name + " is unavailable in this crypto backend";
        ngWarning() << msg;
        d->error = msg;
        return false;
    }

    const string protocolName(noiseProtocolName(config.pattern(), config.pskModifier(), config.cipher(), config.hash()));
    if (protocolName.size() <= d->hashLen) {
        d->h.assign(d->hashLen, '\0');
        memcpy(&d->h[0], protocolName.data(), protocolName.size());
    } else {
        d->h = MessageDigest::digest(protocolName, d->hashAlgo);
        if (d->h.size() != d->hashLen) {
            ngWarning() << "failed to hash Noise protocol name";
            d->error = "failed to hash Noise protocol name";
            return false;
        }
    }
    d->ck = d->h;

    // Noise Initialize always MixHash(prologue), including empty prologue.
    if (!d->mixHash(config.prologue())) {
        return false;
    }

    if (d->tokens.preInitiatorS && !d->mixHash(initiator ? d->s.publicKey() : d->rs)) {
        return false;
    }
    if (d->tokens.preResponderS && !d->mixHash(initiator ? d->rs : d->s.publicKey())) {
        return false;
    }
    return true;
}

bool NoiseHandshakeState::isComplete() const
{
    NG_D(const NoiseHandshakeState);
    return d->complete;
}

bool NoiseHandshakeState::writeMessage(const string &payload, string *outMessage)
{
    NG_D(NoiseHandshakeState);
    d->error.clear();
    if (!outMessage) {
        ngWarning() << "writeMessage requires non-null output";
        d->error = "writeMessage requires non-null output";
        return false;
    }
    if (d->complete) {
        ngWarning() << "handshake already complete";
        d->error = "handshake already complete";
        return false;
    }
    const bool initiator = (d->role == NoiseRole::Initiator);
    if (initiator != (d->msgIndex % 2 == 0) || d->msgIndex >= d->tokens.nMessages) {
        ngWarning() << "writeMessage called at unexpected handshake step";
        d->error = "writeMessage called at unexpected handshake step";
        return false;
    }

    string message;
    for (Token token : d->tokens.messages[static_cast<size_t>(d->msgIndex)]) {
        if (!d->applyToken(token, true, &message, nullptr, nullptr)) {
            return false;
        }
    }
    const string cipherPayload = d->encryptAndHash(payload);
    if (!d->error.empty()) {
        return false;
    }
    message += cipherPayload;
    *outMessage = message;
    ++d->msgIndex;
    if (d->msgIndex >= d->tokens.nMessages) {
        d->complete = true;
    }
    return true;
}

bool NoiseHandshakeState::readMessage(const string &message, string *outPayload)
{
    NG_D(NoiseHandshakeState);
    d->error.clear();
    if (!outPayload) {
        ngWarning() << "readMessage requires non-null output";
        d->error = "readMessage requires non-null output";
        return false;
    }
    if (d->complete) {
        ngWarning() << "handshake already complete";
        d->error = "handshake already complete";
        return false;
    }
    const bool initiator = (d->role == NoiseRole::Initiator);
    if (initiator == (d->msgIndex % 2 == 0) || d->msgIndex >= d->tokens.nMessages) {
        ngWarning() << "readMessage called at unexpected handshake step";
        d->error = "readMessage called at unexpected handshake step";
        return false;
    }

    size_t pos = 0;
    for (Token token : d->tokens.messages[static_cast<size_t>(d->msgIndex)]) {
        if (!d->applyToken(token, false, nullptr, &message, &pos)) {
            return false;
        }
    }
    *outPayload = d->decryptAndHash(message.substr(pos));
    if (!d->error.empty()) {
        return false;
    }
    ++d->msgIndex;
    if (d->msgIndex >= d->tokens.nMessages) {
        d->complete = true;
    }
    return true;
}

bool NoiseHandshakeState::split(NoiseCipherState *send, NoiseCipherState *recv)
{
    NG_D(NoiseHandshakeState);
    d->error.clear();
    if (!send || !recv) {
        ngWarning() << "split requires non-null cipher states";
        d->error = "split requires non-null cipher states";
        return false;
    }
    if (!d->complete) {
        ngWarning() << "handshake not complete";
        d->error = "handshake not complete";
        return false;
    }
    const string outputs = d->hkdf(d->ck, string(), 2);
    if (outputs.size() != d->hashLen * 2) {
        ngWarning() << "split HKDF failed";
        d->error = "split HKDF failed";
        return false;
    }
    NoiseCipherState c1(d->cs.algorithm());
    NoiseCipherState c2(d->cs.algorithm());
    c1.initializeKey(outputs.substr(0, kCipherKeyLen));
    c2.initializeKey(outputs.substr(d->hashLen, kCipherKeyLen));
    if (!c1.hasKey() || !c2.hasKey()) {
        ngWarning() << "split failed to initialize cipher keys";
        d->error = "split failed to initialize cipher keys";
        return false;
    }
    if (d->role == NoiseRole::Initiator) {
        *send = std::move(c1);
        *recv = std::move(c2);
    } else {
        *send = std::move(c2);
        *recv = std::move(c1);
    }
    return true;
}

string NoiseHandshakeState::remoteStaticPublic() const
{
    NG_D(const NoiseHandshakeState);
    return d->rs;
}

string NoiseHandshakeState::handshakeHash() const
{
    NG_D(const NoiseHandshakeState);
    return d->h;
}

string NoiseHandshakeState::errorString() const
{
    NG_D(const NoiseHandshakeState);
    return d->error;
}

bool NoiseHandshakeStatePrivate::mixHash(const string &data)
{
    const string next = MessageDigest::digest(h + data, hashAlgo);
    if (next.size() != hashLen) {
        ngWarning() << "mixHash failed";
        error = "mixHash failed";
        return false;
    }
    h = next;
    return true;
}

bool NoiseHandshakeStatePrivate::mixKey(const string &material)
{
    const string outputs = hkdf(ck, material, 2);
    if (outputs.size() != hashLen * 2) {
        ngWarning() << "mixKey HKDF failed";
        error = "mixKey HKDF failed";
        return false;
    }
    ck = outputs.substr(0, hashLen);
    cs.initializeKey(outputs.substr(hashLen, kCipherKeyLen));
    if (!cs.hasKey()) {
        ngWarning() << "mixKey failed to set cipher key";
        error = "mixKey failed to set cipher key";
        return false;
    }
    return true;
}

bool NoiseHandshakeStatePrivate::mixKeyAndHash(const string &material)
{
    const string outputs = hkdf(ck, material, 3);
    if (outputs.size() != hashLen * 3) {
        ngWarning() << "mixKeyAndHash HKDF failed";
        error = "mixKeyAndHash HKDF failed";
        return false;
    }
    ck = outputs.substr(0, hashLen);
    if (!mixHash(outputs.substr(hashLen, hashLen))) {
        return false;
    }
    cs.initializeKey(outputs.substr(hashLen * 2, kCipherKeyLen));
    if (!cs.hasKey()) {
        ngWarning() << "mixKeyAndHash failed to set cipher key";
        error = "mixKeyAndHash failed to set cipher key";
        return false;
    }
    return true;
}

bool NoiseHandshakeStatePrivate::dhMix(const string &priv, const string &pub)
{
    const string secret = NoiseKey::dh(priv, pub);
    if (secret.empty()) {
        ngWarning() << "DH failed";
        error = "DH failed";
        return false;
    }
    return mixKey(secret);
}

bool NoiseHandshakeStatePrivate::applyToken(Token token, bool writing, string *outMessage, const string *inMessage,
                                            size_t *pos)
{
    auto take = [&](size_t n) -> string {
        if (!inMessage || !pos || *pos + n > inMessage->size()) {
            return string();
        }
        string part = inMessage->substr(*pos, n);
        *pos += n;
        return part;
    };

    switch (token) {
    case Token::E:
        if (writing) {
            e = NoiseKey::generate();
            if (!e.isValid()) {
                ngWarning() << "failed to generate ephemeral key";
                error = "failed to generate ephemeral key";
                return false;
            }
            if (outMessage) {
                *outMessage += e.publicKey();
            }
            if (!mixHash(e.publicKey())) {
                return false;
            }
            if (usesPsk && !mixKey(e.publicKey())) {
                return false;
            }
        } else {
            re = take(kDhLen);
            if (re.size() != kDhLen) {
                ngDebug() << "missing remote ephemeral";
                error = "missing remote ephemeral";
                return false;
            }
            if (!mixHash(re)) {
                return false;
            }
            if (usesPsk && !mixKey(re)) {
                return false;
            }
        }
        return true;
    case Token::S:
        if (writing) {
            const string encS = encryptAndHash(s.publicKey());
            if (!error.empty()) {
                return false;
            }
            if (encS.empty()) {
                ngWarning() << "encrypt static key failed";
                error = "encrypt static key failed";
                return false;
            }
            if (outMessage) {
                *outMessage += encS;
            }
        } else {
            const size_t n = cs.hasKey() ? (kDhLen + kTagLen) : kDhLen;
            const string encS = take(n);
            if (encS.size() != n) {
                ngDebug() << "missing remote static";
                error = "missing remote static";
                return false;
            }
            const string expectedRs = rs;
            rs = decryptAndHash(encS);
            if (!error.empty()) {
                return false;
            }
            if (rs.size() != kDhLen) {
                ngDebug() << "decrypt remote static failed";
                error = "decrypt remote static failed";
                return false;
            }
            if (!checkRemoteStatic(expectedRs)) {
                return false;
            }
        }
        return true;
    case Token::Ee:
        return dhMix(e.privateKey(), re);
    case Token::Es:
        if (role == NoiseRole::Initiator) {
            return dhMix(e.privateKey(), rs);
        }
        return dhMix(s.privateKey(), re);
    case Token::Se:
        if (role == NoiseRole::Initiator) {
            return dhMix(s.privateKey(), re);
        }
        return dhMix(e.privateKey(), rs);
    case Token::Ss:
        return dhMix(s.privateKey(), rs);
    case Token::Psk:
        return mixKeyAndHash(psk);
    default:
        break;
    }
    ngWarning() << "unknown handshake token";
    error = "unknown handshake token";
    return false;
}

string NoiseHandshakeStatePrivate::encryptAndHash(const string &plaintext)
{
    string ciphertext;
    if (cs.hasKey()) {
        ciphertext = cs.encryptWithAd(h, plaintext);
        if (ciphertext.empty()) {
            ngWarning() << "encryptAndHash failed";
            error = "encryptAndHash failed";
            return string();
        }
    } else {
        ciphertext = plaintext;
    }
    if (!mixHash(ciphertext)) {
        return string();
    }
    return ciphertext;
}

string NoiseHandshakeStatePrivate::decryptAndHash(const string &ciphertextAndTag)
{
    string plaintext;
    if (cs.hasKey()) {
        if (ciphertextAndTag.size() < kTagLen) {
            ngDebug() << "decryptAndHash truncated";
            error = "decryptAndHash truncated";
            return string();
        }
        plaintext = cs.decryptWithAd(h, ciphertextAndTag);
        if (!cs.lastDecryptOk()) {
            error = "decryptAndHash failed";
            return string();
        }
    } else {
        plaintext = ciphertextAndTag;
    }
    if (!mixHash(ciphertextAndTag)) {
        return string();
    }
    return plaintext;
}

string NoiseHandshakeStatePrivate::hkdf(const string &chainingKey, const string &inputKeyMaterial, int numOutputs)
{
    if (numOutputs < 2 || numOutputs > 3) {
        ngWarning() << "NoiseHandshakeState::hkdf: invalid numOutputs=" << numOutputs;
        return string();
    }
    return qtng::hkdf(hashAlgo, inputKeyMaterial, chainingKey, string(),
                      static_cast<size_t>(numOutputs) * hashLen);
}

bool NoiseHandshakeStatePrivate::checkRemoteStatic(const string &expectedRs)
{
    if (!expectedRs.empty() && expectedRs != rs) {
        ngWarning() << "remote static public key mismatch";
        error = "remote static public key mismatch";
        return false;
    }
    return true;
}


class NoiseSocketPrivate
{
public:
    NoiseSocketPrivate()
        : role(NoiseRole::Initiator)
        , ready(false)
        , gotPeerPayload(false)
    {
    }

    shared_ptr<SocketLike> backend;
    NoiseHandshakeState hs;
    NoiseCipherState send;
    NoiseCipherState recv;
    NoiseRole role;
    bool ready;
    bool gotPeerPayload;
    string peerPayload;
    string error;
    string recvBuf;
};

NoiseSocket::NoiseSocket(shared_ptr<SocketLike> backend)
    : d_ptr(new NoiseSocketPrivate)
{
    NG_D(NoiseSocket);
    d->backend = backend;
}

NoiseSocket::~NoiseSocket()
{
    delete d_ptr;
}

bool NoiseSocket::initialize(const NoiseConfig &config)
{
    NG_D(NoiseSocket);
    d->error.clear();
    d->recvBuf.clear();
    d->peerPayload.clear();
    d->ready = false;
    d->gotPeerPayload = false;
    d->role = config.role();
    d->send = NoiseCipherState(config.cipher());
    d->recv = NoiseCipherState(config.cipher());
    if (!d->hs.initialize(config)) {
        d->error = d->hs.errorString();
        return false;
    }
    return true;
}

bool NoiseSocket::handshake(const string &payload)
{
    NG_D(NoiseSocket);
    d->error.clear();
    if (!d->backend) {
        ngWarning() << "no backend socket";
        d->error = "no backend socket";
        return false;
    }
    if (d->ready) {
        return true;
    }

    auto sendHandshake = [&](const string &hsPayload) -> bool {
        string msg;
        if (!d->hs.writeMessage(hsPayload, &msg)) {
            d->error = d->hs.errorString();
            return false;
        }
        if (!splitIfHandshakeComplete(d->hs, &d->send, &d->recv, d->ready, d->error)) {
            return false;
        }
        return writeFrame(d->backend, msg, &d->error);
    };
    auto recvHandshake = [&]() -> bool {
        string msg;
        if (!readFrame(d->backend, &msg, &d->error)) {
            return false;
        }
        string peerPayload;
        if (!d->hs.readMessage(msg, &peerPayload)) {
            d->error = d->hs.errorString();
            return false;
        }
        if (!d->gotPeerPayload) {
            d->peerPayload = peerPayload;
            d->gotPeerPayload = true;
        }
        return splitIfHandshakeComplete(d->hs, &d->send, &d->recv, d->ready, d->error);
    };

    // Length-prefix each handshake message. Initiator writes first; remaining
    // turns follow ready so 2-message (IK/KK) and 3-message (XX/XK) patterns share one loop.
    if (d->role == NoiseRole::Initiator && !sendHandshake(payload)) {
        return false;
    }
    bool sentLocalPayload = (d->role == NoiseRole::Initiator);
    while (!d->ready) {
        if (!recvHandshake()) {
            return false;
        }
        if (d->ready) {
            break;
        }
        const string outPayload = sentLocalPayload ? string() : payload;
        sentLocalPayload = true;
        if (!sendHandshake(outPayload)) {
            return false;
        }
    }
    return d->ready;
}

bool NoiseSocket::isHandshakeComplete() const
{
    NG_D(const NoiseSocket);
    return d->ready;
}

string NoiseSocket::peerHandshakePayload() const
{
    NG_D(const NoiseSocket);
    return d->peerPayload;
}

string NoiseSocket::remoteStaticPublic() const
{
    NG_D(const NoiseSocket);
    return d->hs.remoteStaticPublic();
}

string NoiseSocket::handshakeHash() const
{
    NG_D(const NoiseSocket);
    return d->hs.handshakeHash();
}

shared_ptr<SocketLike> NoiseSocket::backend() const
{
    NG_D(const NoiseSocket);
    return d->backend;
}

string NoiseSocket::errorString() const
{
    NG_D(const NoiseSocket);
    if (!d->error.empty()) {
        return d->error;
    }
    if (!d->hs.errorString().empty()) {
        return d->hs.errorString();
    }
    return d->backend ? d->backend->errorString() : string();
}

Socket::SocketError NoiseSocket::error() const
{
    NG_D(const NoiseSocket);
    if (!d->error.empty() || !d->hs.errorString().empty()) {
        return Socket::UnknownSocketError;
    }
    return d->backend ? d->backend->error() : Socket::SocketAccessError;
}

bool NoiseSocket::isValid() const
{
    NG_D(const NoiseSocket);
    return d->backend && d->backend->isValid();
}

HostAddress NoiseSocket::localAddress() const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->localAddress() : HostAddress();
}

uint16_t NoiseSocket::localPort() const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->localPort() : 0;
}

HostAddress NoiseSocket::peerAddress() const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->peerAddress() : HostAddress();
}

string NoiseSocket::peerName() const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->peerName() : string();
}

uint16_t NoiseSocket::peerPort() const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->peerPort() : 0;
}

intptr_t NoiseSocket::fileno() const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->fileno() : -1;
}

Socket::SocketType NoiseSocket::type() const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->type() : Socket::UnknownSocketType;
}

Socket::SocketState NoiseSocket::state() const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->state() : Socket::UnconnectedState;
}

HostAddress::NetworkLayerProtocol NoiseSocket::protocol() const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->protocol() : HostAddress::UnknownNetworkLayerProtocol;
}

string NoiseSocket::localAddressURI() const
{
    NG_D(const NoiseSocket);
    return d->backend ? ("noise+" + d->backend->localAddressURI()) : string();
}

string NoiseSocket::peerAddressURI() const
{
    NG_D(const NoiseSocket);
    return d->backend ? ("noise+" + d->backend->peerAddressURI()) : string();
}

shared_ptr<SocketLike> NoiseSocket::accept()
{
    NG_D(NoiseSocket);
    return d->backend ? d->backend->accept() : shared_ptr<SocketLike>();
}

Socket *NoiseSocket::acceptRaw()
{
    NG_D(NoiseSocket);
    return d->backend ? d->backend->acceptRaw() : nullptr;
}

bool NoiseSocket::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    NG_D(NoiseSocket);
    return d->backend && d->backend->bind(address, port, mode);
}

bool NoiseSocket::bind(uint16_t port, Socket::BindMode mode)
{
    NG_D(NoiseSocket);
    return d->backend && d->backend->bind(port, mode);
}

bool NoiseSocket::connect(const HostAddress &addr, uint16_t port)
{
    NG_D(NoiseSocket);
    return d->backend && d->backend->connect(addr, port);
}

bool NoiseSocket::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    NG_D(NoiseSocket);
    return d->backend && d->backend->connect(hostName, port, dnsCache);
}

void NoiseSocket::close()
{
    NG_D(NoiseSocket);
    if (d->backend) {
        d->backend->close();
    }
}

void NoiseSocket::abort()
{
    NG_D(NoiseSocket);
    if (d->backend) {
        d->backend->abort();
    }
}

bool NoiseSocket::listen(int backlog)
{
    NG_D(NoiseSocket);
    return d->backend && d->backend->listen(backlog);
}

bool NoiseSocket::setOption(Socket::SocketOption option, int value)
{
    NG_D(NoiseSocket);
    return d->backend && d->backend->setOption(option, value);
}

int NoiseSocket::option(Socket::SocketOption option) const
{
    NG_D(const NoiseSocket);
    return d->backend ? d->backend->option(option) : 0;
}

int32_t NoiseSocket::peek(char *data, int32_t size)
{
    NG_D(NoiseSocket);
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

int32_t NoiseSocket::peekRaw(char *data, int32_t size)
{
    NG_D(NoiseSocket);
    return d->backend ? d->backend->peekRaw(data, size) : -1;
}

int32_t NoiseSocket::recv(char *data, int32_t size)
{
    NG_D(NoiseSocket);
    if (!data || size <= 0) {
        return -1;
    }
    if (d->recvBuf.empty()) {
        string frame;
        if (!readFrame(d->backend, &frame, &d->error)) {
            return -1;
        }
        if (!d->ready) {
            ngWarning() << "handshake not complete";
            d->error = "handshake not complete";
            return -1;
        }
        const string msg = d->recv.decryptWithAd(string(), frame);
        if (!d->recv.lastDecryptOk()) {
            d->error = "decrypt failed";
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

int32_t NoiseSocket::recvall(char *data, int32_t size)
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

int32_t NoiseSocket::send(const char *data, int32_t size)
{
    return sendall(data, size);
}

int32_t NoiseSocket::sendall(const char *data, int32_t size)
{
    NG_D(NoiseSocket);
    if (!data || size < 0) {
        return -1;
    }
    d->error.clear();
    if (!d->ready) {
        ngWarning() << "handshake not complete";
        d->error = "handshake not complete";
        return -1;
    }
    const string wire = d->send.encryptWithAd(string(), string(data, static_cast<size_t>(size)));
    if (wire.size() < kTagLen) {
        ngWarning() << "encrypt failed";
        d->error = "encrypt failed";
        return -1;
    }
    if (!writeFrame(d->backend, wire, &d->error)) {
        return -1;
    }
    return size;
}

string NoiseSocket::recv(int32_t size)
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

string NoiseSocket::recvall(int32_t size)
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

int32_t NoiseSocket::send(const string &data)
{
    return send(data.data(), static_cast<int32_t>(data.size()));
}

int32_t NoiseSocket::sendall(const string &data)
{
    return sendall(data.data(), static_cast<int32_t>(data.size()));
}

namespace {

class NoiseSocketLikeImpl : public SocketLike
{
public:
    explicit NoiseSocketLikeImpl(shared_ptr<NoiseSocket> s)
        : s(std::move(s))
    {
    }

    Socket::SocketError error() const override { return s->error(); }
    string errorString() const override { return s->errorString(); }
    bool isValid() const override { return s->isValid(); }
    HostAddress localAddress() const override { return s->localAddress(); }
    uint16_t localPort() const override { return s->localPort(); }
    HostAddress peerAddress() const override { return s->peerAddress(); }
    string peerName() const override { return s->peerName(); }
    uint16_t peerPort() const override { return s->peerPort(); }
    intptr_t fileno() const override { return s->fileno(); }
    Socket::SocketType type() const override { return s->type(); }
    Socket::SocketState state() const override { return s->state(); }
    HostAddress::NetworkLayerProtocol protocol() const override { return s->protocol(); }
    string localAddressURI() const override { return s->localAddressURI(); }
    string peerAddressURI() const override { return s->peerAddressURI(); }
    Socket *acceptRaw() override { return s->acceptRaw(); }
    shared_ptr<SocketLike> accept() override { return s->accept(); }
    bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override
    {
        return s->bind(address, port, mode);
    }
    bool bind(uint16_t port, Socket::BindMode mode) override { return s->bind(port, mode); }
    bool connect(const HostAddress &addr, uint16_t port) override { return s->connect(addr, port); }
    bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override
    {
        return s->connect(hostName, port, dnsCache);
    }
    void close() override { s->close(); }
    void abort() override { s->abort(); }
    bool listen(int backlog) override { return s->listen(backlog); }
    bool setOption(Socket::SocketOption option, int value) override { return s->setOption(option, value); }
    int option(Socket::SocketOption option) const override { return s->option(option); }
    int32_t peek(char *data, int32_t size) override { return s->peek(data, size); }
    int32_t peekRaw(char *data, int32_t size) override { return s->peekRaw(data, size); }
    int32_t recv(char *data, int32_t size) override { return s->recv(data, size); }
    int32_t recvall(char *data, int32_t size) override { return s->recvall(data, size); }
    int32_t send(const char *data, int32_t size) override { return s->send(data, size); }
    int32_t sendall(const char *data, int32_t size) override { return s->sendall(data, size); }
    string recv(int32_t size) override { return s->recv(size); }
    string recvall(int32_t size) override { return s->recvall(size); }
    int32_t send(const string &data) override { return s->send(data); }
    int32_t sendall(const string &data) override { return s->sendall(data); }

    shared_ptr<NoiseSocket> s;
};

}  // namespace

shared_ptr<SocketLike> asSocketLike(shared_ptr<NoiseSocket> s)
{
    if (!s) {
        return shared_ptr<SocketLike>();
    }
    return make_shared<NoiseSocketLikeImpl>(std::move(s));
}

class NoiseDatagramPrivate
{
public:
    NoiseDatagramPrivate()
        : handshaking(false)
        , ready(false)
        , lastDecryptOk(false)
        , gotPeerPayload(false)
    {
    }

    bool finishIfComplete();
public:
    NoiseHandshakeState hs;
    NoiseCipherState send;
    NoiseCipherState recv;
    ReplayCounter replay;
    bool handshaking;
    bool ready;
    bool lastDecryptOk;
    bool gotPeerPayload;
    string peerPayload;
    string error;
};

NoiseDatagram::NoiseDatagram()
    : d_ptr(new NoiseDatagramPrivate)
{
}

NoiseDatagram::NoiseDatagram(NoiseDatagram &&other)
    : d_ptr(new NoiseDatagramPrivate)
{
    std::swap(d_ptr, other.d_ptr);
}

NoiseDatagram &NoiseDatagram::operator=(NoiseDatagram &&other) noexcept
{
    std::swap(d_ptr, other.d_ptr);
    return *this;
}

NoiseDatagram::~NoiseDatagram()
{
    delete d_ptr;
}

bool NoiseDatagram::initialize(const NoiseConfig &config)
{
    NG_D(NoiseDatagram);
    d->error.clear();
    d->peerPayload.clear();
    d->ready = false;
    d->handshaking = false;
    d->lastDecryptOk = false;
    d->gotPeerPayload = false;
    d->replay.reset();
    d->send = NoiseCipherState(config.cipher());
    d->recv = NoiseCipherState(config.cipher());
    if (!d->hs.initialize(config)) {
        d->error = d->hs.errorString();
        return false;
    }
    d->handshaking = true;
    return true;
}

bool NoiseDatagramPrivate::finishIfComplete()
{
    if (!splitIfHandshakeComplete(hs, &send, &recv, ready, error)) {
        handshaking = false;
        return false;
    }
    if (ready) {
        replay.reset();
        handshaking = false;
    }
    return true;
}

bool NoiseDatagram::writeHandshake(const string &payload, string *outMessage)
{
    NG_D(NoiseDatagram);
    d->error.clear();
    if (!d->handshaking) {
        ngWarning() << "not handshaking";
        d->error = "not handshaking";
        return false;
    }
    if (!d->hs.writeMessage(payload, outMessage)) {
        d->error = d->hs.errorString();
        return false;
    }
    return d->finishIfComplete();
}

bool NoiseDatagram::readHandshake(const string &message, string *outPayload)
{
    NG_D(NoiseDatagram);
    d->error.clear();
    if (!d->handshaking) {
        ngWarning() << "not handshaking";
        d->error = "not handshaking";
        return false;
    }
    string payload;
    if (!d->hs.readMessage(message, &payload)) {
        d->error = d->hs.errorString();
        return false;
    }
    if (!d->gotPeerPayload) {
        d->peerPayload = payload;
        d->gotPeerPayload = true;
    }
    if (outPayload) {
        *outPayload = payload;
    }
    return d->finishIfComplete();
}

bool NoiseDatagram::isHandshakeComplete() const
{
    NG_D(const NoiseDatagram);
    return d->ready;
}

string NoiseDatagram::peerHandshakePayload() const
{
    NG_D(const NoiseDatagram);
    return d->peerPayload;
}

string NoiseDatagram::remoteStaticPublic() const
{
    NG_D(const NoiseDatagram);
    return d->hs.remoteStaticPublic();
}

string NoiseDatagram::handshakeHash() const
{
    NG_D(const NoiseDatagram);
    return d->hs.handshakeHash();
}

string NoiseDatagram::errorString() const
{
    NG_D(const NoiseDatagram);
    return d->error;
}

string NoiseDatagram::encrypt(const string &plaintext)
{
    NG_D(NoiseDatagram);
    d->error.clear();
    if (!d->ready) {
        ngWarning() << "handshake not complete";
        d->error = "handshake not complete";
        return string();
    }
    if (d->send.nonce() >= kRejectAfterMessages) {
        ngWarning() << "encrypt: nonce exhausted";
        d->error = "nonce exhausted";
        return string();
    }
    uint64_t n = 0;
    const string cipher = d->send.encryptWithAd(string(), plaintext, &n);
    if (cipher.size() < kTagLen) {
        ngWarning() << "encrypt failed";
        d->error = "encrypt failed";
        return string();
    }
    return packTransport(n, cipher);
}

string NoiseDatagram::decrypt(const string &packet)
{
    NG_D(NoiseDatagram);
    d->error.clear();
    d->lastDecryptOk = false;
    if (!d->ready) {
        ngWarning() << "handshake not complete";
        d->error = "handshake not complete";
        return string();
    }
    uint64_t n = 0;
    string ct;
    if (!unpackTransport(packet, &n, &ct)) {
        ngDebug() << "truncated datagram";
        d->error = "truncated datagram";
        return string();
    }
    if (d->replay.exhausted()) {
        ngWarning() << "decrypt: nonce exhausted";
        d->error = "nonce exhausted";
        return string();
    }
    const string plain = d->recv.decryptWithAd(string(), ct, n);
    if (!d->recv.lastDecryptOk()) {
        d->error = "decrypt failed";
        return string();
    }
    if (!d->replay.validate(n)) {
        ngDebug() << "replay or stale nonce " << n;
        d->error = "replay or stale nonce";
        return string();
    }
    d->lastDecryptOk = true;
    return plain;
}

bool NoiseDatagram::lastDecryptOk() const
{
    NG_D(const NoiseDatagram);
    return d->lastDecryptOk;
}

}  // namespace qtng
