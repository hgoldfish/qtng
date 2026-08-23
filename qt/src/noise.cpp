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
    explicit NoiseCipherStatePrivate(qtng_core::Aead::Algorithm algo = qtng_core::Aead::ChaCha20Poly1305)
        : core(algo)
    {
    }

    qtng_core::NoiseCipherState core;
};

NoiseCipherState::NoiseCipherState(AeadAlgorithm algo)
    : d_ptr(new NoiseCipherStatePrivate(static_cast<qtng_core::Aead::Algorithm>(algo)))
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
    const qtng_core::NoiseKey key = qtng_core::NoiseKey::generate();
    NoiseKey result;
    result.privateKey = toQByteArray(key.privateKey);
    result.publicKey = toQByteArray(key.publicKey);
    return result;
}

NoiseKey NoiseKey::fromPrivateKey(const QByteArray &privateKey32)
{
    const qtng_core::NoiseKey key = qtng_core::NoiseKey::fromPrivateKey(toStdString(privateKey32));
    NoiseKey result;
    result.privateKey = toQByteArray(key.privateKey);
    result.publicKey = toQByteArray(key.publicKey);
    return result;
}

QByteArray NoiseKey::dh(const QByteArray &privateKey32, const QByteArray &peerPublicKey32)
{
    return toQByteArray(qtng_core::NoiseKey::dh(toStdString(privateKey32), toStdString(peerPublicKey32)));
}

NoiseConfig::NoiseConfig(const QByteArray &localPrivateKey)
    : localStatic(localPrivateKey.isEmpty() ? NoiseKey::generate() : NoiseKey::fromPrivateKey(localPrivateKey))
{
}

NoiseConfig::NoiseConfig(const NoiseKey &key)
    : localStatic(key)
{
}

namespace {

qtng_core::NoiseConfig toCoreConfig(const NoiseConfig &cfg)
{
    qtng_core::NoiseKey key;
    key.privateKey = toStdString(cfg.localStatic.privateKey);
    key.publicKey = toStdString(cfg.localStatic.publicKey);
    qtng_core::NoiseConfig core(key);
    core.pattern = static_cast<qtng_core::NoisePattern>(cfg.pattern);
    core.role = static_cast<qtng_core::NoiseRole>(cfg.role);
    core.remoteStaticPublic = toStdString(cfg.remoteStaticPublic);
    core.psk = toStdString(cfg.psk);
    core.pskModifier = static_cast<qtng_core::NoisePskModifier>(cfg.pskModifier);
    core.prologue = toStdString(cfg.prologue);
    core.cipher = static_cast<qtng_core::Aead::Algorithm>(cfg.cipher);
    core.hash = static_cast<qtng_core::NoiseHash>(cfg.hash);
    return core;
}

}  // namespace

QString noiseProtocolName(NoisePattern pattern, NoisePskModifier pskModifier, AeadAlgorithm cipher, NoiseHash hash)
{
    return QString::fromStdString(qtng_core::noiseProtocolName(
            static_cast<qtng_core::NoisePattern>(pattern),
            static_cast<qtng_core::NoisePskModifier>(pskModifier),
            static_cast<qtng_core::Aead::Algorithm>(cipher),
            static_cast<qtng_core::NoiseHash>(hash)));
}

bool parseNoiseProtocolName(const QString &name, NoisePattern *pattern, NoisePskModifier *pskModifier,
                            AeadAlgorithm *cipher, NoiseHash *hash, QString *errorMessage)
{
    qtng_core::NoisePattern corePattern = qtng_core::NoisePattern::XX;
    qtng_core::NoisePskModifier coreMod = qtng_core::NoisePskModifier::None;
    qtng_core::Aead::Algorithm coreCipher = qtng_core::Aead::ChaCha20Poly1305;
    qtng_core::NoiseHash coreHash = qtng_core::NoiseHash::Sha256;
    string err;
    string *errPtr = errorMessage ? &err : nullptr;
    if (!qtng_core::parseNoiseProtocolName(toStdString(name), &corePattern, &coreMod, &coreCipher,
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
    config->pattern = pattern;
    config->pskModifier = pskModifier;
    config->cipher = cipher;
    config->hash = hash;
    return true;
}

class NoiseHandshakeStatePrivate
{
public:
    qtng_core::NoiseHandshakeState core;
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
    shared_ptr<qtng_core::NoiseSocket> core;

    static shared_ptr<qtng_core::NoiseSocket> coreOf(NoiseSocket *s)
    {
        return s ? s->d_func()->core : shared_ptr<qtng_core::NoiseSocket>();
    }
};

NoiseSocket::NoiseSocket(QSharedPointer<SocketLike> backend)
    : d_ptr(new NoiseSocketPrivate)
{
    d_ptr->core = make_shared<qtng_core::NoiseSocket>(toCoreSocketLike(backend));
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
    return d->core->bind(toCoreAddress(address), port, static_cast<qtng_core::Socket::BindMode>(mode));
}

bool NoiseSocket::bind(quint16 port, Socket::BindMode mode)
{
    Q_D(NoiseSocket);
    return d->core->bind(port, static_cast<qtng_core::Socket::BindMode>(mode));
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
    return d->core->connect(toStdString(hostName), port, shared_ptr<qtng_core::SocketDnsCache>());
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
    return d->core->setOption(static_cast<qtng_core::Socket::SocketOption>(option), value.toInt());
}

QVariant NoiseSocket::option(Socket::SocketOption option) const
{
    Q_D(const NoiseSocket);
    return d->core->option(static_cast<qtng_core::Socket::SocketOption>(option));
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
    return ::qtng_bridge::toQtSocketLike(qtng_core::asSocketLike(NoiseSocketPrivate::coreOf(s.data())));
}

class NoiseDatagramPrivate
{
public:
    qtng_core::NoiseDatagram core;
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
