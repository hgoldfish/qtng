#ifndef QTNG_NOISE_H
#define QTNG_NOISE_H

#include <QtCore/qbytearray.h>
#include <QtCore/qsharedpointer.h>
#include <QtCore/qstring.h>
#include "config.h"
#include "socket_utils.h"

QTNETWORKNG_NAMESPACE_BEGIN

// Numeric values match qtng::Aead::Algorithm.
enum class AeadAlgorithm {
    Aes128Gcm = 1,
    Aes256Gcm = 2,
    ChaCha20Poly1305 = 3,
};

struct NoiseKey
{
public:
    NoiseKey() = default;

    QByteArray privateKey() const { return privateKey_; }
    void setPrivateKey(const QByteArray &privateKey) { privateKey_ = privateKey; }
    QByteArray publicKey() const { return publicKey_; }
    void setPublicKey(const QByteArray &publicKey) { publicKey_ = publicKey; }

    bool isValid() const { return privateKey_.size() == 32 && publicKey_.size() == 32; }
    static NoiseKey generate();
    static NoiseKey fromPrivateKey(const QByteArray &privateKey32);
    static QByteArray dh(const QByteArray &privateKey32, const QByteArray &peerPublicKey32);

private:
    QByteArray privateKey_;
    QByteArray publicKey_;
};

class NoiseCipherStatePrivate;
class NoiseCipherState
{
public:
    explicit NoiseCipherState(AeadAlgorithm algo = AeadAlgorithm::ChaCha20Poly1305);
    NoiseCipherState(const NoiseCipherState &other);
    NoiseCipherState &operator=(const NoiseCipherState &other);
    ~NoiseCipherState();

    AeadAlgorithm algorithm() const;
    void initializeKey(const QByteArray &key32);
    bool hasKey() const;
    quint64 nonce() const;
    void setNonce(quint64 n);
    static constexpr quint64 MaxNonce = (~quint64(0)) - 1;
    bool rekey();

    QByteArray encryptWithAd(const QByteArray &ad, const QByteArray &plaintext);
    QByteArray encryptWithAd(const QByteArray &ad, const QByteArray &plaintext, quint64 *outNonce);

    QByteArray decryptWithAd(const QByteArray &ad, const QByteArray &ciphertextAndTag);
    QByteArray decryptWithAd(const QByteArray &ad, const QByteArray &ciphertextAndTag, quint64 nonce);
    bool lastDecryptOk() const;
private:
    NoiseCipherStatePrivate *d_ptr;
    Q_DECLARE_PRIVATE(NoiseCipherState)
    // NoiseHandshakeState::split() feeds the core cipher states into the core
    // handshake's split, which is not reachable through the public Qt API.
    friend class NoiseHandshakeState;
};

enum class NoisePattern {
    XX,  // Noise_XX_25519_*_* — 3 messages, no pre-message keys
    IK,  // Noise_IK_25519_*_* — 2 messages, initiator knows responder static
    XK,  // Noise_XK_25519_*_* — 3 messages, initiator knows responder static
    KK,  // Noise_KK_25519_*_* — 2 messages, both sides know each other's static
};

enum class NoisePskModifier {
    None,
    Psk0,
    Psk1,
    Psk2,
    Psk3,
};

// Numeric values match qtng::NoiseHash.
enum class NoiseHash {
    Sha256 = 0,
    Sha512 = 1,
    Blake2s = 2,
    Blake2b = 3,
};

enum class NoiseRole {
    Initiator,
    Responder,
};

struct NoiseConfig
{
public:
    // Empty localPrivateKey generates a static key. N patterns are not supported.
    // NoiseKey is copied as-is (empty/invalid is left empty).
    explicit NoiseConfig(const QByteArray &localPrivateKey = QByteArray());
    explicit NoiseConfig(const NoiseKey &key);

    const NoiseKey &localStatic() const { return localStatic_; }
    void setLocalStatic(const NoiseKey &key) { localStatic_ = key; }
    NoisePattern pattern() const { return pattern_; }
    void setPattern(NoisePattern pattern) { pattern_ = pattern; }
    NoiseRole role() const { return role_; }
    void setRole(NoiseRole role) { role_ = role; }
    QByteArray remoteStaticPublic() const { return remoteStaticPublic_; }
    void setRemoteStaticPublic(const QByteArray &remoteStaticPublic) { remoteStaticPublic_ = remoteStaticPublic; }
    QByteArray psk() const { return psk_; }
    void setPsk(const QByteArray &psk) { psk_ = psk; }
    NoisePskModifier pskModifier() const { return pskModifier_; }
    void setPskModifier(NoisePskModifier pskModifier) { pskModifier_ = pskModifier; }
    QByteArray prologue() const { return prologue_; }
    void setPrologue(const QByteArray &prologue) { prologue_ = prologue; }
    AeadAlgorithm cipher() const { return cipher_; }
    void setCipher(AeadAlgorithm cipher) { cipher_ = cipher; }
    NoiseHash hash() const { return hash_; }
    void setHash(NoiseHash hash) { hash_ = hash; }

private:
    NoiseKey localStatic_;
    NoisePattern pattern_ = NoisePattern::XX;
    NoiseRole role_ = NoiseRole::Initiator;
    QByteArray remoteStaticPublic_;
    QByteArray psk_;
    NoisePskModifier pskModifier_ = NoisePskModifier::None;
    QByteArray prologue_;
    AeadAlgorithm cipher_ = AeadAlgorithm::ChaCha20Poly1305;
    NoiseHash hash_ = NoiseHash::Sha256;
};

// Full protocol name: Noise_<handshake>_25519_<ChaChaPoly|AESGCM>_<SHA256|...>.
QString noiseProtocolName(NoisePattern pattern, NoisePskModifier pskModifier = NoisePskModifier::None,
                          AeadAlgorithm cipher = AeadAlgorithm::ChaCha20Poly1305,
                          NoiseHash hash = NoiseHash::Sha256);

// Case-insensitive. Accepts names matching noiseProtocolName() for combinations
// that initialize() allows (invalid PSK slots for a pattern are rejected).
bool parseNoiseProtocolName(const QString &name, NoisePattern *pattern, NoisePskModifier *pskModifier,
                            AeadAlgorithm *cipher, NoiseHash *hash, QString *errorMessage = nullptr);

// Sets pattern/pskModifier/cipher/hash from a full protocol name.
bool applyNoiseProtocolName(const QString &name, NoiseConfig *config, QString *errorMessage = nullptr);

class NoiseHandshakeStatePrivate;
class NoiseHandshakeState
{
public:
    NoiseHandshakeState();
    ~NoiseHandshakeState();

    bool initialize(const NoiseConfig &config);

    bool isComplete() const;
    bool writeMessage(const QByteArray &payload, QByteArray *outMessage);
    bool readMessage(const QByteArray &message, QByteArray *outPayload);

    bool split(NoiseCipherState *send, NoiseCipherState *recv);

    QByteArray remoteStaticPublic() const;
    QByteArray handshakeHash() const;
    QString errorString() const;

private:
    NoiseHandshakeStatePrivate * const d_ptr;
    Q_DECLARE_PRIVATE(NoiseHandshakeState)
    Q_DISABLE_COPY(NoiseHandshakeState)
};

class NoiseDatagramPrivate;
class NoiseDatagram
{
public:
    NoiseDatagram();
    NoiseDatagram(NoiseDatagram &&other);
    NoiseDatagram &operator=(NoiseDatagram &&other) noexcept;
    ~NoiseDatagram();

    bool initialize(const NoiseConfig &config);

    bool writeHandshake(const QByteArray &payload, QByteArray *outMessage);
    bool readHandshake(const QByteArray &message, QByteArray *outPayload);
    bool isHandshakeComplete() const;
    QByteArray peerHandshakePayload() const;
    QByteArray remoteStaticPublic() const;
    QByteArray handshakeHash() const;
    QString errorString() const;

    QByteArray encrypt(const QByteArray &plaintext);
    QByteArray decrypt(const QByteArray &packet);
    bool lastDecryptOk() const;
private:
    NoiseDatagramPrivate *d_ptr;
    Q_DECLARE_PRIVATE(NoiseDatagram)
    Q_DISABLE_COPY(NoiseDatagram)
};

class NoiseSocketPrivate;
class NoiseSocket
{
public:
    explicit NoiseSocket(QSharedPointer<SocketLike> backend);
    ~NoiseSocket();

    bool initialize(const NoiseConfig &config);

    bool handshake(const QByteArray &payload = QByteArray());
    bool isHandshakeComplete() const;
    QByteArray peerHandshakePayload() const;
    QByteArray remoteStaticPublic() const;
    QByteArray handshakeHash() const;

    QSharedPointer<SocketLike> backend() const;
    QString errorString() const;
    Socket::SocketError error() const;
    bool isValid() const;
    HostAddress localAddress() const;
    quint16 localPort() const;
    HostAddress peerAddress() const;
    QString peerName() const;
    quint16 peerPort() const;
    qintptr fileno() const;
    Socket::SocketType type() const;
    Socket::SocketState state() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    QString localAddressURI() const;
    QString peerAddressURI() const;

    QSharedPointer<SocketLike> accept();
    Socket *acceptRaw();
    bool bind(const HostAddress &address, quint16 port = 0,
              Socket::BindMode mode = Socket::DefaultForPlatform);
    bool bind(quint16 port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool connect(const HostAddress &addr, quint16 port);
    bool connect(const QString &hostName, quint16 port,
                 QSharedPointer<SocketDnsCache> dnsCache = QSharedPointer<SocketDnsCache>());
    void close();
    void abort();
    bool listen(int backlog);
    bool setOption(Socket::SocketOption option, const QVariant &value);
    QVariant option(Socket::SocketOption option) const;

    qint32 peek(char *data, qint32 size);
    qint32 peekRaw(char *data, qint32 size);
    qint32 recv(char *data, qint32 size);
    qint32 recvall(char *data, qint32 size);
    qint32 send(const char *data, qint32 size);
    qint32 sendall(const char *data, qint32 size);
    QByteArray recv(qint32 size);
    QByteArray recvall(qint32 size);
    qint32 send(const QByteArray &data);
    qint32 sendall(const QByteArray &data);
private:
    NoiseSocketPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(NoiseSocket)
    Q_DISABLE_COPY(NoiseSocket)
};

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<NoiseSocket> s);

inline QSharedPointer<SocketLike> asSocketLike(NoiseSocket *s)
{
    return asSocketLike(QSharedPointer<NoiseSocket>(s));
}

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_NOISE_H
