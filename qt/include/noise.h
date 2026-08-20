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
    QByteArray privateKey;
    QByteArray publicKey;

    bool isValid() const { return privateKey.size() == 32 && publicKey.size() == 32; }
    static NoiseKey generate();
    static NoiseKey fromPrivateKey(const QByteArray &privateKey32);
    static QByteArray dh(const QByteArray &privateKey32, const QByteArray &peerPublicKey32);
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
    NoiseKey localStatic;
    NoisePattern pattern = NoisePattern::XX;
    NoiseRole role = NoiseRole::Initiator;
    QByteArray remoteStaticPublic;
    QByteArray psk;
    NoisePskModifier pskModifier = NoisePskModifier::None;
    QByteArray prologue;
    AeadAlgorithm cipher = AeadAlgorithm::ChaCha20Poly1305;
    NoiseHash hash = NoiseHash::Sha256;

    // Empty localPrivateKey generates a static key. N patterns are not supported.
    // NoiseKey is copied as-is (empty/invalid is left empty).
    explicit NoiseConfig(const QByteArray &localPrivateKey = QByteArray());
    explicit NoiseConfig(const NoiseKey &key);
};

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
