#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "bridge/socket_access.h"
#include "ssl.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class SslCipherPrivate
{
public:
    qtng_core::SslCipher core;

    static SslCipher fromCore(const qtng_core::SslCipher &cipher)
    {
        SslCipher result;
        result.d->core = cipher;
        return result;
    }
};

SslCipher::SslCipher()
    : d(new SslCipherPrivate)
{
}

SslCipher::SslCipher(const QString &name)
    : d(new SslCipherPrivate)
{
    d->core = qtng_core::SslCipher(toStdString(name));
}

SslCipher::SslCipher(const QString &name, Ssl::SslProtocol protocol)
    : d(new SslCipherPrivate)
{
    d->core = qtng_core::SslCipher(toStdString(name), static_cast<qtng_core::Ssl::SslProtocol>(protocol));
}

SslCipher::SslCipher(const SslCipher &other)
    : d(new SslCipherPrivate)
{
    d->core = other.d->core;
}

SslCipher::~SslCipher() { }

QString SslCipher::authenticationMethod() const { return toQString(d->core.authenticationMethod()); }
QString SslCipher::encryptionMethod() const { return toQString(d->core.encryptionMethod()); }
bool SslCipher::isNull() const { return d->core.isNull(); }
QString SslCipher::keyExchangeMethod() const { return toQString(d->core.keyExchangeMethod()); }
QString SslCipher::name() const { return toQString(d->core.name()); }
Ssl::SslProtocol SslCipher::protocol() const { return static_cast<Ssl::SslProtocol>(d->core.protocol()); }
QString SslCipher::protocolString() const { return toQString(d->core.protocolString()); }
int SslCipher::supportedBits() const { return d->core.supportedBits(); }
int SslCipher::usedBits() const { return d->core.usedBits(); }
SslCipher &SslCipher::operator=(const SslCipher &other) { d->core = other.d->core; return *this; }
bool SslCipher::operator==(const SslCipher &other) const { return d->core == other.d->core; }

class SslConfigurationPrivate : public QSharedData
{
public:
    qtng_core::SslConfiguration core;

    static qtng_core::SslConfiguration &coreOf(SslConfiguration &config)
    {
        return config.d->core;
    }

    static const qtng_core::SslConfiguration &coreOf(const SslConfiguration &config)
    {
        return config.d->core;
    }
};

SslConfiguration::SslConfiguration()
    : d(new SslConfigurationPrivate)
{
}

SslConfiguration::SslConfiguration(const SslConfiguration &other)
    : d(other.d)
{
}

SslConfiguration::~SslConfiguration() { }

SslConfiguration &SslConfiguration::operator=(const SslConfiguration &other)
{
    d = other.d;
    return *this;
}

bool SslConfiguration::operator==(const SslConfiguration &other) const { return d->core == other.d->core; }

QList<SslCipher> SslConfiguration::supportedCiphers()
{
    QList<SslCipher> result;
    for (const qtng_core::SslCipher &c : qtng_core::SslConfiguration::supportedCiphers()) {
        result.append(SslCipherPrivate::fromCore(c));
    }
    return result;
}

SslConfiguration SslConfiguration::testPurpose(const QString &commonName, const QString &countryCode,
                                               const QString &organization)
{
    SslConfiguration config;
    SslConfigurationPrivate::coreOf(config) = qtng_core::SslConfiguration::testPurpose(toStdString(commonName), toStdString(countryCode),
                                                              toStdString(organization));
    return config;
}

QList<QByteArray> SslConfiguration::allowedNextProtocols() const
{
    QList<QByteArray> result;
    for (const string &p : d->core.allowedNextProtocols()) {
        result.append(QByteArray(p.data(), static_cast<int>(p.size())));
    }
    return result;
}

void SslConfiguration::setAllowedNextProtocols(const QList<QByteArray> &protocols)
{
    vector<string> core;
    for (const QByteArray &p : protocols) {
        core.push_back(string(p.constData(), static_cast<size_t>(p.size())));
    }
    d->core.setAllowedNextProtocols(core);
}

class SslSocketPrivate
{
public:
    static std::shared_ptr<qtng_core::SslSocket> &coreOf(SslSocket *socket)
    {
        return socket->d_ptr->core;
    }

    std::shared_ptr<qtng_core::SslSocket> core;
};

QByteArray SslSocket::nextNegotiatedProtocol() const
{
    Q_D(const SslSocket);
    const string &p = d->core->nextNegotiatedProtocol();
    return QByteArray(p.data(), static_cast<int>(p.size()));
}

SslSocket::NextProtocolNegotiationStatus SslSocket::nextProtocolNegotiationStatus() const
{
    Q_D(const SslSocket);
    return static_cast<NextProtocolNegotiationStatus>(d->core->nextProtocolNegotiationStatus());
}

SslSocket::SslSocket(HostAddress::NetworkLayerProtocol protocol, const SslConfiguration &config)
    : d_ptr(new SslSocketPrivate)
{
    Q_D(SslSocket);
    d->core = make_shared<qtng_core::SslSocket>(static_cast<qtng_core::HostAddress::NetworkLayerProtocol>(protocol),
                                                SslConfigurationPrivate::coreOf(config));
}

SslSocket::SslSocket(qintptr socketDescriptor, const SslConfiguration &config)
    : d_ptr(new SslSocketPrivate)
{
    Q_D(SslSocket);
    d->core = make_shared<qtng_core::SslSocket>(static_cast<intptr_t>(socketDescriptor),
                                                SslConfigurationPrivate::coreOf(config));
}

SslSocket::SslSocket(QSharedPointer<Socket> rawSocket, const SslConfiguration &config)
    : d_ptr(new SslSocketPrivate)
{
    Q_D(SslSocket);
    d->core = make_shared<qtng_core::SslSocket>(socketCoreOf(rawSocket.data()), SslConfigurationPrivate::coreOf(config));
}

SslSocket::SslSocket(QSharedPointer<SocketLike> rawSocket, const SslConfiguration &config)
    : d_ptr(new SslSocketPrivate)
{
    Q_D(SslSocket);
    d->core = make_shared<qtng_core::SslSocket>(toCoreSocketLike(rawSocket), SslConfigurationPrivate::coreOf(config));
}

SslSocket::~SslSocket()
{
    delete d_ptr;
}

qintptr SslSocket::fileno() const
{
    Q_D(const SslSocket);
    return static_cast<qintptr>(d->core->fileno());
}

bool SslSocket::handshake(bool asServer, const QString &hostName)
{
    Q_D(SslSocket);
    return d->core->handshake(asServer, toStdString(hostName));
}

void SslSocket::close()
{
    Q_D(SslSocket);
    d->core->close();
}

void SslSocket::abort()
{
    Q_D(SslSocket);
    d->core->abort();
}

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<SslSocket> s)
{
    if (!s) {
        return QSharedPointer<SocketLike>();
    }
    return ::qtng_bridge::toQtSocketLike(qtng_core::asSocketLike(SslSocketPrivate::coreOf(s.data())));
}

}  // namespace QTNETWORKNG_NAMESPACE

#include "bridge/ssl_access.h"

namespace qtng_bridge {

std::shared_ptr<qtng_core::SslSocket> sslSocketCoreOf(QTNETWORKNG_NAMESPACE::SslSocket *socket)
{
    if (!socket) {
        return std::shared_ptr<qtng_core::SslSocket>();
    }
    return QTNETWORKNG_NAMESPACE::SslSocketPrivate::coreOf(socket);
}

}  // namespace qtng_bridge
