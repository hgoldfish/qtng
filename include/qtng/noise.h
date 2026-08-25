#ifndef QTNG_NOISE_H
#define QTNG_NOISE_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/aead.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

// X25519 static / ephemeral key material (32 bytes).
struct NoiseKey
{
public:
    NoiseKey() = default;

    const std::string &privateKey() const { return privateKey_; }  // 32 bytes
    void setPrivateKey(const std::string &privateKey) { privateKey_ = privateKey; }
    const std::string &publicKey() const { return publicKey_; }  // 32 bytes
    void setPublicKey(const std::string &publicKey) { publicKey_ = publicKey; }

    bool isValid() const { return privateKey_.size() == 32 && publicKey_.size() == 32; }
    static NoiseKey generate();
    static NoiseKey fromPrivateKey(const std::string &privateKey32);
    static std::string dh(const std::string &privateKey32, const std::string &peerPublicKey32);

private:
    std::string privateKey_;
    std::string publicKey_;
};

// Noise CipherState: AEAD via ``Aead`` (ChaCha20-Poly1305 or AES-256-GCM) with a
// monotonically increasing 64-bit nonce per CipherState instance. Wire nonce is 12 bytes:
// 4 zero bytes followed by the 64-bit counter (little-endian for ChaChaPoly,
// big-endian for AESGCM). After split(), send and recv each hold a separate counter.
// encryptWithAd always allocates from the counter and increments on success; there is
// no explicit-nonce encrypt overload. outNonce returns the value used (for UDP wire
// headers; sender is monotonic like WireGuard). Two-arg decryptWithAd is sequential
// (handshake / NoiseSocket). Three-arg decryptWithAd uses a packet nonce and does not
// advance the counter (UDP receive). Transport nonces are 0 .. MaxNonce (2^64-2).
// 2^64-1 is reserved for Rekey.
class NoiseCipherStatePrivate;
class NoiseCipherState
{
public:
    static constexpr std::uint64_t MaxNonce = ~std::uint64_t(0) - 1;

    explicit NoiseCipherState(Aead::Algorithm algo = Aead::ChaCha20Poly1305);
    NoiseCipherState(const NoiseCipherState &other);
    NoiseCipherState &operator=(const NoiseCipherState &other);
    NoiseCipherState(NoiseCipherState &&other);
    NoiseCipherState &operator=(NoiseCipherState &&other) noexcept;
    ~NoiseCipherState();

    Aead::Algorithm algorithm() const;
    void initializeKey(const std::string &key32);
    bool hasKey() const;
    std::uint64_t nonce() const;
    // Transport counter only: ``0 .. MaxNonce``. Values above ``MaxNonce`` are ignored.
    void setNonce(std::uint64_t n);

    // Noise CipherState.Rekey: ENCRYPT(k, 2^64-1, zerolen, zeros); nonce resets to 0.
    bool rekey();

    // Encrypt: ciphertext || 16-byte tag. Uses and increments nonce.
    // Returns empty on failure. outNonce receives the nonce used (for wire header).
    std::string encryptWithAd(const std::string &ad, const std::string &plaintext);
    std::string encryptWithAd(const std::string &ad, const std::string &plaintext, std::uint64_t *outNonce);

    // Sequential decrypt: uses and increments nonce (handshake / in-order streams).
    std::string decryptWithAd(const std::string &ad, const std::string &ciphertextAndTag);
    // Decrypt with the packet nonce and do not advance the sequential counter.
    // Replay filtering is NoiseDatagram's job (WireGuard / RFC 6479 window).
    std::string decryptWithAd(const std::string &ad, const std::string &ciphertextAndTag, std::uint64_t nonce);
    bool lastDecryptOk() const;
private:
    NoiseCipherStatePrivate *d_ptr;
    NG_DECLARE_PRIVATE(NoiseCipherState)
};

enum class NoisePattern {
    XX,  // Noise_XX_25519_*_* — 3 messages, no pre-message keys
    IK,  // Noise_IK_25519_*_* — 2 messages, initiator knows responder static
    XK,  // Noise_XK_25519_*_* — 3 messages, initiator knows responder static
    KK,  // Noise_KK_25519_*_* — 2 messages, both sides know each other's static
};

// Noise PSK modifiers (psk0..psk3). Protocol name becomes Noise_<pattern>pskN_25519_*_<HASH>.
// None: no PSK. psk0 is MixKeyAndHash at the start of message 1; pskN (N>0) is at the
// end of message N, before payload AEAD. 2-message patterns (IK, KK) allow psk0..psk2;
// 3-message patterns (XX, XK) allow psk0..psk3. PSK must be exactly 32 bytes.
enum class NoisePskModifier {
    None,
    Psk0,
    Psk1,
    Psk2,
    Psk3,
};

// Noise HASH functions (protocol-name suffix / HASHLEN). Cipher keys stay 32 bytes
// (Truncate-32 when HASHLEN is 64). BLAKE2 requires the linked OpenSSL/LibreSSL
// build; initialize() fails immediately if the digest is unavailable.
enum class NoiseHash {
    Sha256 = 0,  // HASHLEN = 32, SHA256
    Sha512 = 1,  // HASHLEN = 64, SHA512
    Blake2s = 2,  // HASHLEN = 32, BLAKE2s
    Blake2b = 3,  // HASHLEN = 64, BLAKE2b
};

enum class NoiseRole {
    Initiator,
    Responder,
};

// Handshake options for initialize(). Empty localPrivateKey generates a static
// X25519 key: N patterns (no local static) are not supported. Passing a NoiseKey
// copies it as-is (including invalid/empty; initialize() then fails).
struct NoiseConfig
{
public:
    explicit NoiseConfig(const std::string &localPrivateKey = std::string());
    explicit NoiseConfig(const NoiseKey &key);

    const NoiseKey &localStatic() const { return localStatic_; }
    void setLocalStatic(const NoiseKey &key) { localStatic_ = key; }
    NoisePattern pattern() const { return pattern_; }
    void setPattern(NoisePattern pattern) { pattern_ = pattern; }
    NoiseRole role() const { return role_; }
    void setRole(NoiseRole role) { role_ = role; }
    const std::string &remoteStaticPublic() const { return remoteStaticPublic_; }
    void setRemoteStaticPublic(const std::string &remoteStaticPublic) { remoteStaticPublic_ = remoteStaticPublic; }
    const std::string &psk() const { return psk_; }
    void setPsk(const std::string &psk) { psk_ = psk; }
    NoisePskModifier pskModifier() const { return pskModifier_; }
    void setPskModifier(NoisePskModifier pskModifier) { pskModifier_ = pskModifier; }
    const std::string &prologue() const { return prologue_; }
    void setPrologue(const std::string &prologue) { prologue_ = prologue; }
    Aead::Algorithm cipher() const { return cipher_; }
    void setCipher(Aead::Algorithm cipher) { cipher_ = cipher; }
    NoiseHash hash() const { return hash_; }
    void setHash(NoiseHash hash) { hash_ = hash; }

private:
    NoiseKey localStatic_;
    NoisePattern pattern_ = NoisePattern::XX;
    NoiseRole role_ = NoiseRole::Initiator;
    std::string remoteStaticPublic_;
    std::string psk_;
    NoisePskModifier pskModifier_ = NoisePskModifier::None;
    std::string prologue_;
    Aead::Algorithm cipher_ = Aead::ChaCha20Poly1305;
    NoiseHash hash_ = NoiseHash::Sha256;
};

// Full protocol name: Noise_<handshake>_25519_<ChaChaPoly|AESGCM>_<SHA256|...>.
std::string noiseProtocolName(NoisePattern pattern,
                              NoisePskModifier pskModifier = NoisePskModifier::None,
                              Aead::Algorithm cipher = Aead::ChaCha20Poly1305,
                              NoiseHash hash = NoiseHash::Sha256);

// Case-insensitive. Accepts names matching noiseProtocolName() for combinations
// that initialize() allows (invalid PSK slots for a pattern are rejected).
bool parseNoiseProtocolName(const std::string &name, NoisePattern *pattern,
                            NoisePskModifier *pskModifier, Aead::Algorithm *cipher, NoiseHash *hash,
                            std::string *errorMessage = nullptr);

// Sets pattern/pskModifier/cipher/hash from a full protocol name.
bool applyNoiseProtocolName(const std::string &name, NoiseConfig *config,
                            std::string *errorMessage = nullptr);

// Minimal Noise handshake state machine for XX / IK / XK / KK, with optional PSK modifiers.
// After handshake finishes, take transport ciphers via split().
class NoiseHandshakeStatePrivate;
class NoiseHandshakeState
{
public:
    NoiseHandshakeState();
    ~NoiseHandshakeState();

    // MixHash(prologue) after the protocol name. IK/XK initiator and both KK roles
    // need remoteStaticPublic. See NoiseConfig for cipher, hash, and PSK rules.
    bool initialize(const NoiseConfig &config);

    bool isComplete() const;
    bool writeMessage(const std::string &payload, std::string *outMessage);
    bool readMessage(const std::string &message, std::string *outPayload);

    // After handshake: initiator gets (send=c1, recv=c2), responder reverse.
    bool split(NoiseCipherState *send, NoiseCipherState *recv);

    std::string remoteStaticPublic() const;
    std::string handshakeHash() const;
    std::string errorString() const;
private:
    NoiseHandshakeStatePrivate * const d_ptr;
    NG_DECLARE_PRIVATE(NoiseHandshakeState)
    NG_DISABLE_COPY(NoiseHandshakeState)
};

// Datagram-oriented Noise session. Does not send or receive; the caller owns the
// transport (UDP, a queue, ...) and passes raw packet bytes in and out.
// Handshake packets are Noise messages. Transport packets are
// [8-byte big-endian nonce][ciphertext||tag]: the receiver uses that nonce for
// AEAD so delivery may be reordered, then a WireGuard/RFC 6479 8192-bit sliding
// window rejects replays and packets that have slid out of the window.
// Unauthenticated packets do not update the window.
class NoiseDatagramPrivate;
class NoiseDatagram
{
public:
    NoiseDatagram();
    NoiseDatagram(NoiseDatagram &&other);
    NoiseDatagram &operator=(NoiseDatagram &&other) noexcept;
    ~NoiseDatagram();

    bool initialize(const NoiseConfig &config);

    bool writeHandshake(const std::string &payload, std::string *outMessage);
    bool readHandshake(const std::string &message, std::string *outPayload);
    bool isHandshakeComplete() const;
    std::string peerHandshakePayload() const;
    std::string remoteStaticPublic() const;
    std::string handshakeHash() const;
    std::string errorString() const;

    // Transport: one Noise message per call. Empty plaintext still yields a tag.
    std::string encrypt(const std::string &plaintext);
    std::string decrypt(const std::string &packet);
    bool lastDecryptOk() const;
private:
    NoiseDatagramPrivate *d_ptr;
    NG_DECLARE_PRIVATE(NoiseDatagram)
    NG_DISABLE_COPY(NoiseDatagram)
};

// Reliable-stream Noise session, analogous to SslSocket. Handshake uses
// NoiseHandshakeState; transport uses sequential CipherState (implicit nonce,
// no replay window). Each Noise message is length-prefixed on the backend
// (2-byte big-endian length + ciphertext||tag for transport; raw handshake bytes).
// Not a SocketLike — wrap with asSocketLike() when one is required.
class NoiseSocketPrivate;
class NoiseSocket
{
public:
    explicit NoiseSocket(std::shared_ptr<SocketLike> backend);
    ~NoiseSocket();

    bool initialize(const NoiseConfig &config);

    bool handshake(const std::string &payload = std::string());
    bool isHandshakeComplete() const;
    std::string peerHandshakePayload() const;
    std::string remoteStaticPublic() const;
    std::string handshakeHash() const;

    std::shared_ptr<SocketLike> backend() const;
    std::string errorString() const;
    Socket::SocketError error() const;
    bool isValid() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    HostAddress peerAddress() const;
    std::string peerName() const;
    std::uint16_t peerPort() const;
    std::intptr_t fileno() const;
    Socket::SocketType type() const;
    Socket::SocketState state() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    std::string localAddressURI() const;
    std::string peerAddressURI() const;

    std::shared_ptr<SocketLike> accept();
    Socket *acceptRaw();
    bool bind(const HostAddress &address, std::uint16_t port = 0,
              Socket::BindMode mode = Socket::DefaultForPlatform);
    bool bind(std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool connect(const HostAddress &addr, std::uint16_t port);
    bool connect(const std::string &hostName, std::uint16_t port,
                 std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>());
    void close();
    void abort();
    bool listen(int backlog);
    bool setOption(Socket::SocketOption option, int value);
    int option(Socket::SocketOption option) const;

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t peekRaw(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);
private:
    NoiseSocketPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(NoiseSocket)
    NG_DISABLE_COPY(NoiseSocket)
};

std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<NoiseSocket> s);

inline std::shared_ptr<SocketLike> asSocketLike(NoiseSocket *s)
{
    return asSocketLike(std::shared_ptr<NoiseSocket>(s));
}

}  // namespace qtng

#endif  // QTNG_NOISE_H
