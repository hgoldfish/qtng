#include <utility>

#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "bridge/stream_bridge.h"
#include "noise.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class NoiseCipherStatePrivate
{
public:
    explicit NoiseCipherStatePrivate(QTNG_NAMESPACE::Aead::Algorithm algo = QTNG_NAMESPACE::Aead::ChaCha20Poly1305)
        : core(algo)
    {
    }

    QTNG_NAMESPACE::NoiseCipherState core;
};

NoiseCipherState::NoiseCipherState(AeadAlgorithm algo)
    : d_ptr(new NoiseCipherStatePrivate(static_cast<QTNG_NAMESPACE::Aead::Algorithm>(algo)))
{
}

NoiseCipherState::NoiseCipherState(const NoiseCipherState &other)
    : d_ptr(new NoiseCipherStatePrivate)
{
    d_ptr->core = other.d_ptr->core;
}

NoiseCipherState &NoiseCipherState::operator=(const NoiseCipherState &other)
{
    if (this != &other) {
        d_ptr->core = other.d_ptr->core;
    }
    return *this;
}

NoiseCipherState::~NoiseCipherState()
{
    delete d_ptr;
}

void NoiseCipherState::initializeKey(const QByteArray &key32)
{
    Q_D(NoiseCipherState);
    d->core.initializeKey(toStdString(key32));
}

bool NoiseCipherState::hasKey() const
{
    Q_D(const NoiseCipherState);
    return d->core.hasKey();
}

AeadAlgorithm NoiseCipherState::algorithm() const
{
    Q_D(const NoiseCipherState);
    return static_cast<AeadAlgorithm>(d->core.algorithm());
}

quint64 NoiseCipherState::nonce() const
{
    Q_D(const NoiseCipherState);
    return d->core.nonce();
}

void NoiseCipherState::setNonce(quint64 n)
{
    Q_D(NoiseCipherState);
    d->core.setNonce(n);
}

bool NoiseCipherState::rekey()
{
    Q_D(NoiseCipherState);
    return d->core.rekey();
}

QByteArray NoiseCipherState::encryptWithAd(const QByteArray &ad, const QByteArray &plaintext)
{
    Q_D(NoiseCipherState);
    return toQByteArray(d->core.encryptWithAd(toStdString(ad), toStdString(plaintext)));
}

QByteArray NoiseCipherState::encryptWithAd(const QByteArray &ad, const QByteArray &plaintext, quint64 *outNonce)
{
    Q_D(NoiseCipherState);
    uint64_t coreNonce = 0;
    const QByteArray result =
            toQByteArray(d->core.encryptWithAd(toStdString(ad), toStdString(plaintext), &coreNonce));
    if (outNonce) {
        *outNonce = coreNonce;
    }
    return result;
}

QByteArray NoiseCipherState::decryptWithAd(const QByteArray &ad, const QByteArray &ciphertextAndTag)
{
    Q_D(NoiseCipherState);
    return toQByteArray(d->core.decryptWithAd(toStdString(ad), toStdString(ciphertextAndTag)));
}

QByteArray NoiseCipherState::decryptWithAd(const QByteArray &ad, const QByteArray &ciphertextAndTag, quint64 nonce)
{
    Q_D(NoiseCipherState);
    return toQByteArray(d->core.decryptWithAd(toStdString(ad), toStdString(ciphertextAndTag), nonce));
}

bool NoiseCipherState::lastDecryptOk() const
{
    Q_D(const NoiseCipherState);
    return d->core.lastDecryptOk();
}

NoiseKey NoiseKey::generate()
{
    const QTNG_NAMESPACE::NoiseKey key = QTNG_NAMESPACE::NoiseKey::generate();
    NoiseKey result;
    result.setPrivateKey(toQByteArray(key.privateKey()));
    result.setPublicKey(toQByteArray(key.publicKey()));
    return result;
}

NoiseKey NoiseKey::fromPrivateKey(const QByteArray &privateKey32)
{
    const QTNG_NAMESPACE::NoiseKey key = QTNG_NAMESPACE::NoiseKey::fromPrivateKey(toStdString(privateKey32));
    NoiseKey result;
    result.setPrivateKey(toQByteArray(key.privateKey()));
    result.setPublicKey(toQByteArray(key.publicKey()));
    return result;
}

QByteArray NoiseKey::dh(const QByteArray &privateKey32, const QByteArray &peerPublicKey32)
{
    return toQByteArray(QTNG_NAMESPACE::NoiseKey::dh(toStdString(privateKey32), toStdString(peerPublicKey32)));
}

NoiseConfig::NoiseConfig(const QByteArray &localPrivateKey)
    : localStatic_(localPrivateKey.isEmpty() ? NoiseKey::generate() : NoiseKey::fromPrivateKey(localPrivateKey))
{
}

NoiseConfig::NoiseConfig(const NoiseKey &key)
    : localStatic_(key)
{
}

namespace {

QTNG_NAMESPACE::NoiseConfig toCoreConfig(const NoiseConfig &cfg)
{
    QTNG_NAMESPACE::NoiseKey key;
    key.setPrivateKey(toStdString(cfg.localStatic().privateKey()));
    key.setPublicKey(toStdString(cfg.localStatic().publicKey()));
    QTNG_NAMESPACE::NoiseConfig core(key);
    core.setPattern(static_cast<QTNG_NAMESPACE::NoisePattern>(cfg.pattern()));
    core.setRole(static_cast<QTNG_NAMESPACE::NoiseRole>(cfg.role()));
    core.setRemoteStaticPublic(toStdString(cfg.remoteStaticPublic()));
    core.setPsk(toStdString(cfg.psk()));
    core.setPskModifier(static_cast<QTNG_NAMESPACE::NoisePskModifier>(cfg.pskModifier()));
    core.setPrologue(toStdString(cfg.prologue()));
    core.setCipher(static_cast<QTNG_NAMESPACE::Aead::Algorithm>(cfg.cipher()));
    core.setHash(static_cast<QTNG_NAMESPACE::NoiseHash>(cfg.hash()));
    return core;
}

}  // namespace

QString noiseProtocolName(NoisePattern pattern, NoisePskModifier pskModifier, AeadAlgorithm cipher, NoiseHash hash)
{
    return QString::fromStdString(QTNG_NAMESPACE::noiseProtocolName(
            static_cast<QTNG_NAMESPACE::NoisePattern>(pattern),
            static_cast<QTNG_NAMESPACE::NoisePskModifier>(pskModifier),
            static_cast<QTNG_NAMESPACE::Aead::Algorithm>(cipher),
            static_cast<QTNG_NAMESPACE::NoiseHash>(hash)));
}

bool parseNoiseProtocolName(const QString &name, NoisePattern *pattern, NoisePskModifier *pskModifier,
                            AeadAlgorithm *cipher, NoiseHash *hash, QString *errorMessage)
{
    QTNG_NAMESPACE::NoisePattern corePattern = QTNG_NAMESPACE::NoisePattern::XX;
    QTNG_NAMESPACE::NoisePskModifier coreMod = QTNG_NAMESPACE::NoisePskModifier::None;
    QTNG_NAMESPACE::Aead::Algorithm coreCipher = QTNG_NAMESPACE::Aead::ChaCha20Poly1305;
    QTNG_NAMESPACE::NoiseHash coreHash = QTNG_NAMESPACE::NoiseHash::Sha256;
    string err;
    string *errPtr = errorMessage ? &err : nullptr;
    if (!QTNG_NAMESPACE::parseNoiseProtocolName(toStdString(name), &corePattern, &coreMod, &coreCipher,
                                           &coreHash, errPtr)) {
        if (errorMessage) {
            *errorMessage = QString::fromStdString(err);
        }
        return false;
    }
    *pattern = static_cast<NoisePattern>(corePattern);
    *pskModifier = static_cast<NoisePskModifier>(coreMod);
    *cipher = static_cast<AeadAlgorithm>(coreCipher);
    *hash = static_cast<NoiseHash>(coreHash);
    return true;
}

bool applyNoiseProtocolName(const QString &name, NoiseConfig *config, QString *errorMessage)
{
    NoisePattern pattern = NoisePattern::XX;
    NoisePskModifier pskModifier = NoisePskModifier::None;
    AeadAlgorithm cipher = AeadAlgorithm::ChaCha20Poly1305;
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

class NoiseHandshakeStatePrivate
{
public:
    QTNG_NAMESPACE::NoiseHandshakeState core;
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
    Q_D(NoiseHandshakeState);
    return d->core.initialize(toCoreConfig(config));
}

bool NoiseHandshakeState::isComplete() const
{
    Q_D(const NoiseHandshakeState);
    return d->core.isComplete();
}

bool NoiseHandshakeState::writeMessage(const QByteArray &payload, QByteArray *outMessage)
{
    Q_D(NoiseHandshakeState);
    string coreOut;
    const bool ok = d->core.writeMessage(toStdString(payload), &coreOut);
    if (ok && outMessage) {
        *outMessage = toQByteArray(coreOut);
    }
    return ok;
}

bool NoiseHandshakeState::readMessage(const QByteArray &message, QByteArray *outPayload)
{
    Q_D(NoiseHandshakeState);
    string coreOut;
    const bool ok = d->core.readMessage(toStdString(message), &coreOut);
    if (ok && outPayload) {
        *outPayload = toQByteArray(coreOut);
    }
    return ok;
}

bool NoiseHandshakeState::split(NoiseCipherState *send, NoiseCipherState *recv)
{
    Q_D(NoiseHandshakeState);
    if (!send || !recv) {
        return false;
    }
    return d->core.split(&send->d_ptr->core, &recv->d_ptr->core);
}

QByteArray NoiseHandshakeState::remoteStaticPublic() const
{
    Q_D(const NoiseHandshakeState);
    return toQByteArray(d->core.remoteStaticPublic());
}

QByteArray NoiseHandshakeState::handshakeHash() const
{
    Q_D(const NoiseHandshakeState);
    return toQByteArray(d->core.handshakeHash());
}

QString NoiseHandshakeState::errorString() const
{
    Q_D(const NoiseHandshakeState);
    return toQString(d->core.errorString());
}

class NoiseSocketPrivate
{
public:
    shared_ptr<QTNG_NAMESPACE::NoiseSocket> core;

    static shared_ptr<QTNG_NAMESPACE::NoiseSocket> coreOf(NoiseSocket *s)
    {
        return s ? s->d_func()->core : shared_ptr<QTNG_NAMESPACE::NoiseSocket>();
    }
};

NoiseSocket::NoiseSocket(QSharedPointer<SocketLike> backend)
    : d_ptr(new NoiseSocketPrivate)
{
    d_ptr->core = make_shared<QTNG_NAMESPACE::NoiseSocket>(toCoreSocketLike(backend));
}

NoiseSocket::~NoiseSocket()
{
    delete d_ptr;
}

bool NoiseSocket::initialize(const NoiseConfig &config)
{
    Q_D(NoiseSocket);
    return d->core->initialize(toCoreConfig(config));
}

bool NoiseSocket::handshake(const QByteArray &payload)
{
    Q_D(NoiseSocket);
    return d->core->handshake(toStdString(payload));
}

bool NoiseSocket::isHandshakeComplete() const
{
    Q_D(const NoiseSocket);
    return d->core->isHandshakeComplete();
}

QByteArray NoiseSocket::peerHandshakePayload() const
{
    Q_D(const NoiseSocket);
    return toQByteArray(d->core->peerHandshakePayload());
}

QByteArray NoiseSocket::remoteStaticPublic() const
{
    Q_D(const NoiseSocket);
    return toQByteArray(d->core->remoteStaticPublic());
}

QByteArray NoiseSocket::handshakeHash() const
{
    Q_D(const NoiseSocket);
    return toQByteArray(d->core->handshakeHash());
}

QSharedPointer<SocketLike> NoiseSocket::backend() const
{
    Q_D(const NoiseSocket);
    return toQtSocketLike(d->core->backend());
}

QString NoiseSocket::errorString() const
{
    Q_D(const NoiseSocket);
    return toQString(d->core->errorString());
}

Socket::SocketError NoiseSocket::error() const
{
    Q_D(const NoiseSocket);
    return static_cast<Socket::SocketError>(d->core->error());
}

bool NoiseSocket::isValid() const
{
    Q_D(const NoiseSocket);
    return d->core->isValid();
}

HostAddress NoiseSocket::localAddress() const
{
    Q_D(const NoiseSocket);
    return toQtAddress(d->core->localAddress());
}

quint16 NoiseSocket::localPort() const
{
    Q_D(const NoiseSocket);
    return d->core->localPort();
}

HostAddress NoiseSocket::peerAddress() const
{
    Q_D(const NoiseSocket);
    return toQtAddress(d->core->peerAddress());
}

QString NoiseSocket::peerName() const
{
    Q_D(const NoiseSocket);
    return toQString(d->core->peerName());
}

quint16 NoiseSocket::peerPort() const
{
    Q_D(const NoiseSocket);
    return d->core->peerPort();
}

qintptr NoiseSocket::fileno() const
{
    Q_D(const NoiseSocket);
    return static_cast<qintptr>(d->core->fileno());
}

Socket::SocketType NoiseSocket::type() const
{
    Q_D(const NoiseSocket);
    return static_cast<Socket::SocketType>(d->core->type());
}

Socket::SocketState NoiseSocket::state() const
{
    Q_D(const NoiseSocket);
    return static_cast<Socket::SocketState>(d->core->state());
}

HostAddress::NetworkLayerProtocol NoiseSocket::protocol() const
{
    Q_D(const NoiseSocket);
    return static_cast<HostAddress::NetworkLayerProtocol>(d->core->protocol());
}

QString NoiseSocket::localAddressURI() const
{
    Q_D(const NoiseSocket);
    return toQString(d->core->localAddressURI());
}

QString NoiseSocket::peerAddressURI() const
{
    Q_D(const NoiseSocket);
    return toQString(d->core->peerAddressURI());
}

QSharedPointer<SocketLike> NoiseSocket::accept()
{
    Q_D(NoiseSocket);
    return toQtSocketLike(d->core->accept());
}

Socket *NoiseSocket::acceptRaw()
{
    return nullptr;
}

bool NoiseSocket::bind(const HostAddress &address, quint16 port, Socket::BindMode mode)
{
    Q_D(NoiseSocket);
    return d->core->bind(toCoreAddress(address), port, static_cast<QTNG_NAMESPACE::Socket::BindMode>(mode));
}

bool NoiseSocket::bind(quint16 port, Socket::BindMode mode)
{
    Q_D(NoiseSocket);
    return d->core->bind(port, static_cast<QTNG_NAMESPACE::Socket::BindMode>(mode));
}

bool NoiseSocket::connect(const HostAddress &addr, quint16 port)
{
    Q_D(NoiseSocket);
    return d->core->connect(toCoreAddress(addr), port);
}

bool NoiseSocket::connect(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache)
{
    Q_UNUSED(dnsCache);
    Q_D(NoiseSocket);
    return d->core->connect(toStdString(hostName), port, shared_ptr<QTNG_NAMESPACE::SocketDnsCache>());
}

void NoiseSocket::close()
{
    Q_D(NoiseSocket);
    d->core->close();
}

void NoiseSocket::abort()
{
    Q_D(NoiseSocket);
    d->core->abort();
}

bool NoiseSocket::listen(int backlog)
{
    Q_D(NoiseSocket);
    return d->core->listen(backlog);
}

bool NoiseSocket::setOption(Socket::SocketOption option, const QVariant &value)
{
    Q_D(NoiseSocket);
    return d->core->setOption(static_cast<QTNG_NAMESPACE::Socket::SocketOption>(option), value.toInt());
}

QVariant NoiseSocket::option(Socket::SocketOption option) const
{
    Q_D(const NoiseSocket);
    return d->core->option(static_cast<QTNG_NAMESPACE::Socket::SocketOption>(option));
}

qint32 NoiseSocket::peek(char *data, qint32 size)
{
    Q_D(NoiseSocket);
    return d->core->peek(data, size);
}

qint32 NoiseSocket::peekRaw(char *data, qint32 size)
{
    Q_D(NoiseSocket);
    return d->core->peekRaw(data, size);
}

qint32 NoiseSocket::recv(char *data, qint32 size)
{
    Q_D(NoiseSocket);
    return d->core->recv(data, size);
}

qint32 NoiseSocket::recvall(char *data, qint32 size)
{
    Q_D(NoiseSocket);
    return d->core->recvall(data, size);
}

qint32 NoiseSocket::send(const char *data, qint32 size)
{
    Q_D(NoiseSocket);
    return d->core->send(data, size);
}

qint32 NoiseSocket::sendall(const char *data, qint32 size)
{
    Q_D(NoiseSocket);
    return d->core->sendall(data, size);
}

QByteArray NoiseSocket::recv(qint32 size)
{
    Q_D(NoiseSocket);
    return toQByteArray(d->core->recv(size));
}

QByteArray NoiseSocket::recvall(qint32 size)
{
    Q_D(NoiseSocket);
    return toQByteArray(d->core->recvall(size));
}

qint32 NoiseSocket::send(const QByteArray &data)
{
    Q_D(NoiseSocket);
    return d->core->send(toStdString(data));
}

qint32 NoiseSocket::sendall(const QByteArray &data)
{
    Q_D(NoiseSocket);
    return d->core->sendall(toStdString(data));
}

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<NoiseSocket> s)
{
    if (!s) {
        return QSharedPointer<SocketLike>();
    }
    return ::qtng_bridge::toQtSocketLike(QTNG_NAMESPACE::asSocketLike(NoiseSocketPrivate::coreOf(s.data())));
}

class NoiseDatagramPrivate
{
public:
    QTNG_NAMESPACE::NoiseDatagram core;
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
    Q_D(NoiseDatagram);
    return d->core.initialize(toCoreConfig(config));
}

bool NoiseDatagram::writeHandshake(const QByteArray &payload, QByteArray *outMessage)
{
    Q_D(NoiseDatagram);
    string coreOut;
    const bool ok = d->core.writeHandshake(toStdString(payload), &coreOut);
    if (ok && outMessage) {
        *outMessage = toQByteArray(coreOut);
    }
    return ok;
}

bool NoiseDatagram::readHandshake(const QByteArray &message, QByteArray *outPayload)
{
    Q_D(NoiseDatagram);
    string coreOut;
    const bool ok = d->core.readHandshake(toStdString(message), &coreOut);
    if (ok && outPayload) {
        *outPayload = toQByteArray(coreOut);
    }
    return ok;
}

bool NoiseDatagram::isHandshakeComplete() const
{
    Q_D(const NoiseDatagram);
    return d->core.isHandshakeComplete();
}

QByteArray NoiseDatagram::peerHandshakePayload() const
{
    Q_D(const NoiseDatagram);
    return toQByteArray(d->core.peerHandshakePayload());
}

QByteArray NoiseDatagram::remoteStaticPublic() const
{
    Q_D(const NoiseDatagram);
    return toQByteArray(d->core.remoteStaticPublic());
}

QByteArray NoiseDatagram::handshakeHash() const
{
    Q_D(const NoiseDatagram);
    return toQByteArray(d->core.handshakeHash());
}

QString NoiseDatagram::errorString() const
{
    Q_D(const NoiseDatagram);
    return toQString(d->core.errorString());
}

QByteArray NoiseDatagram::encrypt(const QByteArray &plaintext)
{
    Q_D(NoiseDatagram);
    return toQByteArray(d->core.encrypt(toStdString(plaintext)));
}

QByteArray NoiseDatagram::decrypt(const QByteArray &packet)
{
    Q_D(NoiseDatagram);
    return toQByteArray(d->core.decrypt(toStdString(packet)));
}

bool NoiseDatagram::lastDecryptOk() const
{
    Q_D(const NoiseDatagram);
    return d->core.lastDecryptOk();
}

}  // namespace QTNETWORKNG_NAMESPACE
