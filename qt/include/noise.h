#ifndef QTNG_NOISE_H
#define QTNG_NOISE_H

#include <QtCore/qbytearray.h>
#include <QtCore/qsharedpointer.h>
#include <QtCore/qstring.h>
#include "config.h"
#include "socket_utils.h"

QTNETWORKNG_NAMESPACE_BEGIN

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
    NoiseCipherState();
    NoiseCipherState(const NoiseCipherState &other);
    NoiseCipherState &operator=(const NoiseCipherState &other);
    ~NoiseCipherState();

    void initializeKey(const QByteArray &key32);
    bool hasKey() const;
    quint64 nonce() const;
    void setNonce(quint64 n);
    bool rekey();

    QByteArray encryptWithAd(const QByteArray &ad, const QByteArray &plaintext);
    QByteArray encryptWithAd(const QByteArray &ad, const QByteArray &plaintext, quint64 *outNonce);

    QByteArray decryptWithAd(const QByteArray &ad, const QByteArray &ciphertextAndTag);
    QByteArray decryptWithAd(const QByteArray &ad, const QByteArray &ciphertextAndTag, quint64 nonce);
    bool lastDecryptOk() const;

    bool acceptIncomingNonce(quint64 remoteNonce);

    friend class NoiseHandshakeState;
private:
    NoiseCipherStatePrivate *d_ptr;
    Q_DECLARE_PRIVATE(NoiseCipherState)
};

enum class NoisePattern {
    XX,
    PSK_XX,
    IK,
};

enum class NoiseRole {
    Initiator,
    Responder,
};

class NoiseHandshakeStatePrivate;
class NoiseHandshakeState
{
public:
    NoiseHandshakeState();
    ~NoiseHandshakeState();

    bool initialize(NoisePattern pattern, NoiseRole role,
                    const NoiseKey &localStatic,
                    const QByteArray &remoteStaticPublic = QByteArray(),
                    const QByteArray &psk = QByteArray(),
                    const QByteArray &prologue = QByteArray());

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

QByteArray noiseHkdf(const QByteArray &secret, const QByteArray &salt,
                       const QByteArray &info, qsizetype outLen);
QByteArray noiseHmacSha256(const QByteArray &key, const QByteArray &data);

class NoiseStreamPrivate;
class NoiseStream : public SocketLike
{
public:
    explicit NoiseStream(QSharedPointer<SocketLike> backend);
    ~NoiseStream() override;

    bool initialize(NoisePattern pattern, NoiseRole role,
                    const NoiseKey &localStatic,
                    const QByteArray &remoteStaticPublic = QByteArray(),
                    const QByteArray &psk = QByteArray(),
                    const QByteArray &prologue = QByteArray());

    bool handshake(const QByteArray &payload = QByteArray());
    bool isHandshakeComplete() const;
    QByteArray peerHandshakePayload() const;
    QByteArray remoteStaticPublic() const;
    QByteArray handshakeHash() const;

    bool sendMessage(const QByteArray &plaintext);
    QByteArray recvMessage();

    QSharedPointer<SocketLike> backend() const;
    QString errorString() const override;

    Socket::SocketError error() const override;
    bool isValid() const override;
    HostAddress localAddress() const override;
    quint16 localPort() const override;
    HostAddress peerAddress() const override;
    QString peerName() const override;
    quint16 peerPort() const override;
    qintptr fileno() const override;
    Socket::SocketType type() const override;
    Socket::SocketState state() const override;
    HostAddress::NetworkLayerProtocol protocol() const override;
    QString localAddressURI() const override;
    QString peerAddressURI() const override;

    QSharedPointer<SocketLike> accept() override;
    Socket *acceptRaw() override;
    bool bind(const HostAddress &address, quint16 port = 0,
              Socket::BindMode mode = Socket::DefaultForPlatform) override;
    bool bind(quint16 port = 0, Socket::BindMode mode = Socket::DefaultForPlatform) override;
    bool connect(const HostAddress &addr, quint16 port) override;
    bool connect(const QString &hostName, quint16 port,
                 QSharedPointer<SocketDnsCache> dnsCache = QSharedPointer<SocketDnsCache>()) override;
    void close() override;
    void abort() override;
    bool listen(int backlog) override;
    bool setOption(Socket::SocketOption option, const QVariant &value) override;
    QVariant option(Socket::SocketOption option) const override;

    qint32 peek(char *data, qint32 size) override;
    qint32 peekRaw(char *data, qint32 size) override;
    qint32 recv(char *data, qint32 size) override;
    qint32 recvall(char *data, qint32 size) override;
    qint32 send(const char *data, qint32 size) override;
    qint32 sendall(const char *data, qint32 size) override;
    QByteArray recv(qint32 size) override;
    QByteArray recvall(qint32 size) override;
    qint32 send(const QByteArray &data) override;
    qint32 sendall(const QByteArray &data) override;
private:
    NoiseStreamPrivate * const d_ptr;
    Q_DECLARE_PRIVATE(NoiseStream)
    Q_DISABLE_COPY(NoiseStream)
};

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<NoiseStream> s);

inline QSharedPointer<SocketLike> asSocketLike(NoiseStream *s)
{
    return asSocketLike(QSharedPointer<NoiseStream>(s));
}

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_NOISE_H
