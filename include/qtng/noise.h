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
    std::string privateKey;  // 32 bytes
    std::string publicKey;   // 32 bytes

    bool isValid() const { return privateKey.size() == 32 && publicKey.size() == 32; }
    static NoiseKey generate();
    static NoiseKey fromPrivateKey(const std::string &privateKey32);
    static std::string dh(const std::string &privateKey32, const std::string &peerPublicKey32);
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

    // Noise CipherState.Rekey: ENCRYPT(k, 2^64-1, zerolen, zeros), leave n unchanged.
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
    XX,       // Noise_XX_25519_*_SHA256 — mutual static key auth (3 messages)
    PSK_XX,   // NoisePSK_XX — XX with a pre-shared key mixed as psk0
    IK,       // Noise_IK_25519_*_SHA256 — initiator knows responder static (2 messages)
};

enum class NoiseRole {
    Initiator,
    Responder,
};

// Minimal Noise handshake state machine for XX / PSK_XX / IK.
// After handshake finishes, take transport ciphers via split().
class NoiseHandshakeStatePrivate;
class NoiseHandshakeState
{
public:
    NoiseHandshakeState();
    ~NoiseHandshakeState();

    // prologue is MixHash()'d after the protocol name (Noise Initialize).
    // For IK, initiator must supply remoteStaticPublic (32 bytes).
    // cipher must be ChaCha20Poly1305 (default) or Aes256Gcm (Noise AESGCM).
    bool initialize(NoisePattern pattern, NoiseRole role,
                    const NoiseKey &localStatic,
                    const std::string &remoteStaticPublic = std::string(),
                    const std::string &psk = std::string(),
                    const std::string &prologue = std::string(),
                    Aead::Algorithm cipher = Aead::ChaCha20Poly1305);

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

    bool initialize(NoisePattern pattern, NoiseRole role,
                    const NoiseKey &localStatic,
                    const std::string &remoteStaticPublic = std::string(),
                    const std::string &psk = std::string(),
                    const std::string &prologue = std::string(),
                    Aead::Algorithm cipher = Aead::ChaCha20Poly1305);

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

    bool initialize(NoisePattern pattern, NoiseRole role,
                    const NoiseKey &localStatic,
                    const std::string &remoteStaticPublic = std::string(),
                    const std::string &psk = std::string(),
                    const std::string &prologue = std::string(),
                    Aead::Algorithm cipher = Aead::ChaCha20Poly1305);

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
