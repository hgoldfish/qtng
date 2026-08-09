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
    qtng_core::NoiseCipherState core;
};

NoiseCipherState::NoiseCipherState()
    : d_ptr(new NoiseCipherStatePrivate)
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

bool NoiseCipherState::acceptIncomingNonce(quint64 remoteNonce)
{
    Q_D(NoiseCipherState);
    return d->core.acceptIncomingNonce(remoteNonce);
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

bool NoiseHandshakeState::initialize(NoisePattern pattern, NoiseRole role, const NoiseKey &localStatic,
                                     const QByteArray &remoteStaticPublic, const QByteArray &psk,
                                     const QByteArray &prologue)
{
    Q_D(NoiseHandshakeState);
    qtng_core::NoiseKey coreKey;
    coreKey.privateKey = toStdString(localStatic.privateKey);
    coreKey.publicKey = toStdString(localStatic.publicKey);
    return d->core.initialize(static_cast<qtng_core::NoisePattern>(pattern),
                              static_cast<qtng_core::NoiseRole>(role), coreKey, toStdString(remoteStaticPublic),
                              toStdString(psk), toStdString(prologue));
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

QByteArray noiseHkdf(const QByteArray &secret, const QByteArray &salt, const QByteArray &info, qsizetype outLen)
{
    return toQByteArray(
            qtng_core::noiseHkdf(toStdString(secret), toStdString(salt), toStdString(info), static_cast<size_t>(outLen)));
}

QByteArray noiseHmacSha256(const QByteArray &key, const QByteArray &data)
{
    return toQByteArray(qtng_core::noiseHmacSha256(toStdString(key), toStdString(data)));
}

class NoiseStreamPrivate
{
public:
    shared_ptr<qtng_core::NoiseStream> core;
};

NoiseStream::NoiseStream(QSharedPointer<SocketLike> backend)
    : d_ptr(new NoiseStreamPrivate)
{
    d_ptr->core = make_shared<qtng_core::NoiseStream>(toCoreSocketLike(backend));
}

NoiseStream::~NoiseStream()
{
    delete d_ptr;
}

bool NoiseStream::initialize(NoisePattern pattern, NoiseRole role, const NoiseKey &localStatic,
                             const QByteArray &remoteStaticPublic, const QByteArray &psk, const QByteArray &prologue)
{
    Q_D(NoiseStream);
    qtng_core::NoiseKey coreKey;
    coreKey.privateKey = toStdString(localStatic.privateKey);
    coreKey.publicKey = toStdString(localStatic.publicKey);
    return d->core->initialize(static_cast<qtng_core::NoisePattern>(pattern),
                               static_cast<qtng_core::NoiseRole>(role), coreKey, toStdString(remoteStaticPublic),
                               toStdString(psk), toStdString(prologue));
}

bool NoiseStream::handshake(const QByteArray &payload)
{
    Q_D(NoiseStream);
    return d->core->handshake(toStdString(payload));
}

bool NoiseStream::isHandshakeComplete() const
{
    Q_D(const NoiseStream);
    return d->core->isHandshakeComplete();
}

QByteArray NoiseStream::peerHandshakePayload() const
{
    Q_D(const NoiseStream);
    return toQByteArray(d->core->peerHandshakePayload());
}

QByteArray NoiseStream::remoteStaticPublic() const
{
    Q_D(const NoiseStream);
    return toQByteArray(d->core->remoteStaticPublic());
}

QByteArray NoiseStream::handshakeHash() const
{
    Q_D(const NoiseStream);
    return toQByteArray(d->core->handshakeHash());
}

bool NoiseStream::sendMessage(const QByteArray &plaintext)
{
    Q_D(NoiseStream);
    return d->core->sendMessage(toStdString(plaintext));
}

QByteArray NoiseStream::recvMessage()
{
    Q_D(NoiseStream);
    return toQByteArray(d->core->recvMessage());
}

QSharedPointer<SocketLike> NoiseStream::backend() const
{
    Q_D(const NoiseStream);
    return toQtSocketLike(d->core->backend());
}

QString NoiseStream::errorString() const
{
    Q_D(const NoiseStream);
    return toQString(d->core->errorString());
}

Socket::SocketError NoiseStream::error() const
{
    Q_D(const NoiseStream);
    return static_cast<Socket::SocketError>(d->core->error());
}

bool NoiseStream::isValid() const
{
    Q_D(const NoiseStream);
    return d->core->isValid();
}

HostAddress NoiseStream::localAddress() const
{
    Q_D(const NoiseStream);
    return toQtHostAddress(d->core->localAddress());
}

quint16 NoiseStream::localPort() const
{
    Q_D(const NoiseStream);
    return d->core->localPort();
}

HostAddress NoiseStream::peerAddress() const
{
    Q_D(const NoiseStream);
    return toQtHostAddress(d->core->peerAddress());
}

QString NoiseStream::peerName() const
{
    Q_D(const NoiseStream);
    return toQString(d->core->peerName());
}

quint16 NoiseStream::peerPort() const
{
    Q_D(const NoiseStream);
    return d->core->peerPort();
}

qintptr NoiseStream::fileno() const
{
    Q_D(const NoiseStream);
    return static_cast<qintptr>(d->core->fileno());
}

Socket::SocketType NoiseStream::type() const
{
    Q_D(const NoiseStream);
    return static_cast<Socket::SocketType>(d->core->type());
}

Socket::SocketState NoiseStream::state() const
{
    Q_D(const NoiseStream);
    return static_cast<Socket::SocketState>(d->core->state());
}

HostAddress::NetworkLayerProtocol NoiseStream::protocol() const
{
    Q_D(const NoiseStream);
    return static_cast<HostAddress::NetworkLayerProtocol>(d->core->protocol());
}

QString NoiseStream::localAddressURI() const
{
    Q_D(const NoiseStream);
    return toQString(d->core->localAddressURI());
}

QString NoiseStream::peerAddressURI() const
{
    Q_D(const NoiseStream);
    return toQString(d->core->peerAddressURI());
}

QSharedPointer<SocketLike> NoiseStream::accept()
{
    Q_D(NoiseStream);
    return toQtSocketLike(d->core->accept());
}

Socket *NoiseStream::acceptRaw()
{
    return nullptr;
}

bool NoiseStream::bind(const HostAddress &address, quint16 port, Socket::BindMode mode)
{
    Q_D(NoiseStream);
    return d->core->bind(toCoreHostAddress(address), port, static_cast<qtng_core::Socket::BindMode>(mode));
}

bool NoiseStream::bind(quint16 port, Socket::BindMode mode)
{
    Q_D(NoiseStream);
    return d->core->bind(port, static_cast<qtng_core::Socket::BindMode>(mode));
}

bool NoiseStream::connect(const HostAddress &addr, quint16 port)
{
    Q_D(NoiseStream);
    return d->core->connect(toCoreHostAddress(addr), port);
}

bool NoiseStream::connect(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache)
{
    Q_UNUSED(dnsCache);
    Q_D(NoiseStream);
    return d->core->connect(toStdString(hostName), port, shared_ptr<qtng_core::SocketDnsCache>());
}

void NoiseStream::close()
{
    Q_D(NoiseStream);
    d->core->close();
}

void NoiseStream::abort()
{
    Q_D(NoiseStream);
    d->core->abort();
}

bool NoiseStream::listen(int backlog)
{
    Q_D(NoiseStream);
    return d->core->listen(backlog);
}

bool NoiseStream::setOption(Socket::SocketOption option, const QVariant &value)
{
    Q_D(NoiseStream);
    return d->core->setOption(static_cast<qtng_core::Socket::SocketOption>(option), value.toInt());
}

QVariant NoiseStream::option(Socket::SocketOption option) const
{
    Q_D(const NoiseStream);
    return d->core->option(static_cast<qtng_core::Socket::SocketOption>(option));
}

qint32 NoiseStream::peek(char *data, qint32 size)
{
    Q_D(NoiseStream);
    return d->core->peek(data, size);
}

qint32 NoiseStream::peekRaw(char *data, qint32 size)
{
    Q_D(NoiseStream);
    return d->core->peekRaw(data, size);
}

qint32 NoiseStream::recv(char *data, qint32 size)
{
    Q_D(NoiseStream);
    return d->core->recv(data, size);
}

qint32 NoiseStream::recvall(char *data, qint32 size)
{
    Q_D(NoiseStream);
    return d->core->recvall(data, size);
}

qint32 NoiseStream::send(const char *data, qint32 size)
{
    Q_D(NoiseStream);
    return d->core->send(data, size);
}

qint32 NoiseStream::sendall(const char *data, qint32 size)
{
    Q_D(NoiseStream);
    return d->core->sendall(data, size);
}

QByteArray NoiseStream::recv(qint32 size)
{
    Q_D(NoiseStream);
    return toQByteArray(d->core->recv(size));
}

QByteArray NoiseStream::recvall(qint32 size)
{
    Q_D(NoiseStream);
    return toQByteArray(d->core->recvall(size));
}

qint32 NoiseStream::send(const QByteArray &data)
{
    Q_D(NoiseStream);
    return d->core->send(toStdString(data));
}

qint32 NoiseStream::sendall(const QByteArray &data)
{
    Q_D(NoiseStream);
    return d->core->sendall(toStdString(data));
}

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<NoiseStream> s)
{
    return qSharedPointerCast<SocketLike>(s);
}

}  // namespace QTNETWORKNG_NAMESPACE
