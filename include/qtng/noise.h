#ifndef QTNG_NOISE_H
#define QTNG_NOISE_H

#include <cstdint>
#include <string>
#include <vector>

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
    XX,       // Noise_XX_25519_ChaChaPoly_SHA256 — mutual static key auth
    PSK_XX,   // NoisePSK_XX — XX with a pre-shared key mixed into the handshake
};

enum class NoiseRole {
    Initiator,
    Responder,
};

// Minimal Noise handshake state machine for XX / PSK_XX.
// After handshake finishes, take transport ciphers via split().
class NoiseHandshakeState
{
public:
    NoiseHandshakeState();
    ~NoiseHandshakeState();

    bool initialize(NoisePattern pattern, NoiseRole role,
                    const NoiseKey &localStatic,
                    const std::string &remoteStaticPublic /* optional empty */,
                    const std::string &psk /* empty unless PSK_XX */);

    bool isComplete() const { return m_complete; }
    bool writeMessage(const std::string &payload, std::string *outMessage);
    bool readMessage(const std::string &message, std::string *outPayload);

    // After handshake: initiator gets (send=c1, recv=c2), responder reverse.
    bool split(NoiseCipherState *send, NoiseCipherState *recv);

    std::string remoteStaticPublic() const { return m_rs; }
    std::string errorString() const { return m_error; }
private:
    void mixHash(const std::string &data);
    void mixKey(const std::string &material);
    std::string encryptAndHash(const std::string &plaintext);
    std::string decryptAndHash(const std::string &ciphertextAndTag);
    std::string hkdf(const std::string &chainingKey, const std::string &inputKeyMaterial, int numOutputs);

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

}  // namespace qtng

#endif  // QTNG_NOISE_H
