#ifndef QTNG_NOISE_H
#define QTNG_NOISE_H

#include <cstdint>
#include <memory>
#include <string>

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

// ChaCha20-Poly1305 AEAD with a monotonically increasing 64-bit nonce
// (encoded as 12-byte little-endian nonce with 4 zero leading bytes, Noise style).
class NoiseCipherState
{
public:
    NoiseCipherState();
    ~NoiseCipherState();

    void initializeKey(const std::string &key32);
    bool hasKey() const { return m_hasKey; }
    std::uint64_t nonce() const { return m_nonce; }
    void setNonce(std::uint64_t n) { m_nonce = n; }

    // Noise CipherState.Rekey: replace k, leave n unchanged.
    bool rekey();

    // Encrypt: ciphertext || 16-byte tag. Uses and increments m_nonce.
    // Returns empty on failure. outNonce receives the nonce used (for wire header).
    std::string encryptWithAd(const std::string &ad, const std::string &plaintext);
    std::string encryptWithAd(const std::string &ad, const std::string &plaintext, std::uint64_t *outNonce);

    // Decrypt with explicit nonce (required for multipath / reordering).
    std::string decryptWithAd(const std::string &ad, const std::string &ciphertextAndTag);
    std::string decryptWithAd(const std::string &ad, const std::string &ciphertextAndTag, std::uint64_t nonce);
    bool lastDecryptOk() const { return m_lastDecryptOk; }

    // Anti-replay: accept nonce if within sliding window and not seen.
    bool acceptIncomingNonce(std::uint64_t remoteNonce);
private:
    bool m_hasKey;
    bool m_lastDecryptOk;
    std::string m_key;
    std::uint64_t m_nonce;
    std::uint64_t m_highestRemoteNonce;
    std::uint64_t m_replayWindow;  // bitset of last 64 nonces
};

enum class NoisePattern {
    XX,       // Noise_XX_25519_ChaChaPoly_SHA256 — mutual static key auth (3 messages)
    PSK_XX,   // NoisePSK_XX — XX with a pre-shared key mixed as psk0
    IK,       // Noise_IK_25519_ChaChaPoly_SHA256 — initiator knows responder static (2 messages)
};

enum class NoiseRole {
    Initiator,
    Responder,
};

// Minimal Noise handshake state machine for XX / PSK_XX / IK.
// After handshake finishes, take transport ciphers via split().
class NoiseHandshakeState
{
public:
    NoiseHandshakeState();
    ~NoiseHandshakeState();

    // prologue is MixHash()'d after the protocol name (Noise Initialize).
    // For IK, initiator must supply remoteStaticPublic (32 bytes).
    bool initialize(NoisePattern pattern, NoiseRole role,
                    const NoiseKey &localStatic,
                    const std::string &remoteStaticPublic = std::string(),
                    const std::string &psk = std::string(),
                    const std::string &prologue = std::string());

    bool isComplete() const { return m_complete; }
    bool writeMessage(const std::string &payload, std::string *outMessage);
    bool readMessage(const std::string &message, std::string *outPayload);

    // After handshake: initiator gets (send=c1, recv=c2), responder reverse.
    bool split(NoiseCipherState *send, NoiseCipherState *recv);

    std::string remoteStaticPublic() const { return m_rs; }
    std::string handshakeHash() const { return m_h; }
    std::string errorString() const { return m_error; }
private:
    void mixHash(const std::string &data);
    void mixKey(const std::string &material);
    void mixKeyAndHash(const std::string &material);
    std::string encryptAndHash(const std::string &plaintext);
    std::string decryptAndHash(const std::string &ciphertextAndTag);
    std::string hkdf(const std::string &chainingKey, const std::string &inputKeyMaterial, int numOutputs);
    bool checkRemoteStatic(const std::string &expectedRs);

    NoisePattern m_pattern;
    NoiseRole m_role;
    bool m_complete;
    int m_msgIndex;
    std::string m_error;

    NoiseKey m_s;   // local static
    NoiseKey m_e;   // local ephemeral
    std::string m_rs;  // remote static public
    std::string m_re;  // remote ephemeral public
    std::string m_psk;

    std::string m_ck;   // chaining key
    std::string m_h;    // handshake hash
    NoiseCipherState m_cs;
};

// HKDF-SHA256 helper exposed for cookie MAC etc.
std::string noiseHkdf(const std::string &secret, const std::string &salt,
                      const std::string &info, std::size_t outLen);
std::string noiseHmacSha256(const std::string &key, const std::string &data);

// SocketLike wrapper: Noise handshake + length-prefixed AEAD transport frames.
// Each sendall() encrypts one application message; recv()/recvall() return decrypted
// bytes from the current frame (partial reads buffered until the frame is consumed).
class NoiseStreamPrivate;
class NoiseStream : public SocketLike
{
public:
    explicit NoiseStream(std::shared_ptr<SocketLike> backend);
    ~NoiseStream() override;

    bool initialize(NoisePattern pattern, NoiseRole role,
                    const NoiseKey &localStatic,
                    const std::string &remoteStaticPublic = std::string(),
                    const std::string &psk = std::string(),
                    const std::string &prologue = std::string());

    // Exchange handshake messages on the backend; optional payloads are available
    // via peerHandshakePayload() after success.
    bool handshake(const std::string &payload = std::string());
    bool isHandshakeComplete() const;
    std::string peerHandshakePayload() const;
    std::string remoteStaticPublic() const;
    std::string handshakeHash() const;

    // Explicit message API (one Noise transport message each).
    bool sendMessage(const std::string &plaintext);
    std::string recvMessage();

    std::shared_ptr<SocketLike> backend() const;
    std::string errorString() const override;

    Socket::SocketError error() const override;
    bool isValid() const override;
    HostAddress localAddress() const override;
    std::uint16_t localPort() const override;
    HostAddress peerAddress() const override;
    std::string peerName() const override;
    std::uint16_t peerPort() const override;
    std::intptr_t fileno() const override;
    Socket::SocketType type() const override;
    Socket::SocketState state() const override;
    HostAddress::NetworkLayerProtocol protocol() const override;
    std::string localAddressURI() const override;
    std::string peerAddressURI() const override;

    std::shared_ptr<SocketLike> accept() override;
    Socket *acceptRaw() override;
    bool bind(const HostAddress &address, std::uint16_t port = 0,
              Socket::BindMode mode = Socket::DefaultForPlatform) override;
    bool bind(std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform) override;
    bool connect(const HostAddress &addr, std::uint16_t port) override;
    bool connect(const std::string &hostName, std::uint16_t port,
                 std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>()) override;
    void close() override;
    void abort() override;
    bool listen(int backlog) override;
    bool setOption(Socket::SocketOption option, int value) override;
    int option(Socket::SocketOption option) const override;

    std::int32_t peek(char *data, std::int32_t size) override;
    std::int32_t peekRaw(char *data, std::int32_t size) override;
    std::int32_t recv(char *data, std::int32_t size) override;
    std::int32_t recvall(char *data, std::int32_t size) override;
    std::int32_t send(const char *data, std::int32_t size) override;
    std::int32_t sendall(const char *data, std::int32_t size) override;
    std::string recv(std::int32_t size) override;
    std::string recvall(std::int32_t size) override;
    std::int32_t send(const std::string &data) override;
    std::int32_t sendall(const std::string &data) override;
private:
    NoiseStreamPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(NoiseStream)
    NG_DISABLE_COPY(NoiseStream)
};

std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<NoiseStream> s);

inline std::shared_ptr<SocketLike> asSocketLike(NoiseStream *s)
{
    return asSocketLike(std::shared_ptr<NoiseStream>(s));
}

}  // namespace qtng

#endif  // QTNG_NOISE_H
