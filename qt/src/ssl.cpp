#include <utility>

#include <QtCore/qdebug.h>

#include "bridge/cert_access.h"
#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "bridge/pkey_access.h"
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
    qtng_core::SslConfiguration *external = nullptr;

    qtng_core::SslConfiguration &config() { return external ? *external : core; }
    const qtng_core::SslConfiguration &config() const { return external ? *external : core; }

    static void bind(SslConfiguration *config, qtng_core::SslConfiguration *core)
    {
        if (config) {
            config->d->external = core;
        }
    }

    static qtng_core::SslConfiguration &coreOf(SslConfiguration &config)
    {
        return config.d->config();
    }

    static const qtng_core::SslConfiguration &coreOf(const SslConfiguration &config)
    {
        return config.d->config();
    }

    static SslConfiguration fromCore(const qtng_core::SslConfiguration &config)
    {
        SslConfiguration result;
        result.d->core = config;
        return result;
    }
};

namespace {

class QtChooseTlsExtNameCallbackAdapter : public qtng_core::ChooseTlsExtNameCallback
{
public:
    explicit QtChooseTlsExtNameCallbackAdapter(QSharedPointer<QTNETWORKNG_NAMESPACE::ChooseTlsExtNameCallback> callback)
        : callback(std::move(callback))
    {
    }

    std::string choose(const std::string &hostName) override
    {
        if (!callback) {
            return std::string();
        }
        return toStdString(callback->choose(toQString(hostName)));
    }

    QSharedPointer<QTNETWORKNG_NAMESPACE::ChooseTlsExtNameCallback> qtCallback() const { return callback; }

private:
    QSharedPointer<QTNETWORKNG_NAMESPACE::ChooseTlsExtNameCallback> callback;
};

}  // namespace

SslConfiguration::SslConfiguration()
    : d(new SslConfigurationPrivate)
{
}

SslConfiguration::SslConfiguration(const SslConfiguration &other)
    : d(other.d)
{
}

SslConfiguration::SslConfiguration(SslConfiguration &&other)
    : d(std::move(other.d))
{
}

SslConfiguration::~SslConfiguration() { }

SslConfiguration &SslConfiguration::operator=(const SslConfiguration &other)
{
    d = other.d;
    return *this;
}

bool SslConfiguration::operator==(const SslConfiguration &other) const
{
    return d->config() == other.d->config();
}

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
    for (const string &p : d->config().allowedNextProtocols()) {
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
    d->config().setAllowedNextProtocols(core);
}

QList<Certificate> SslConfiguration::caCertificates() const
{
    QList<Certificate> result;
    for (const qtng_core::Certificate &c : d->config().caCertificates()) {
        result.append(toQtCertificate(c));
    }
    return result;
}

QList<SslCipher> SslConfiguration::ciphers() const
{
    QList<SslCipher> result;
    for (const qtng_core::SslCipher &c : d->config().ciphers()) {
        result.append(SslCipherPrivate::fromCore(c));
    }
    return result;
}

bool SslConfiguration::isNull() const
{
    return d->config().isNull();
}

Certificate SslConfiguration::localCertificate() const
{
    return toQtCertificate(d->config().localCertificate());
}

Ssl::PeerVerifyMode SslConfiguration::peerVerifyMode() const
{
    return static_cast<Ssl::PeerVerifyMode>(d->config().peerVerifyMode());
}

int SslConfiguration::peerVerifyDepth() const
{
    return d->config().peerVerifyDepth();
}

PrivateKey SslConfiguration::privateKey() const
{
    return toQtPrivateKey(d->config().privateKey());
}

bool SslConfiguration::onlySecureProtocol() const
{
    return d->config().onlySecureProtocol();
}

bool SslConfiguration::supportCompression() const
{
    return d->config().supportCompression();
}

bool SslConfiguration::sendTlsExtHostName() const
{
    return d->config().sendTlsExtHostName();
}

QSharedPointer<ChooseTlsExtNameCallback> SslConfiguration::tlsExtHostNameCallback() const
{
    const std::shared_ptr<qtng_core::ChooseTlsExtNameCallback> core = d->config().tlsExtHostNameCallback();
    if (std::shared_ptr<QtChooseTlsExtNameCallbackAdapter> adapter =
            std::dynamic_pointer_cast<QtChooseTlsExtNameCallbackAdapter>(core)) {
        return adapter->qtCallback();
    }
    return QSharedPointer<ChooseTlsExtNameCallback>();
}

void SslConfiguration::addCaCertificate(const Certificate &certificate)
{
    d->config().addCaCertificate(certificateCoreOf(certificate));
}

void SslConfiguration::addCaCertificates(const QList<Certificate> &certificates)
{
    vector<qtng_core::Certificate> core;
    core.reserve(static_cast<size_t>(certificates.size()));
    for (const Certificate &c : certificates) {
        core.push_back(certificateCoreOf(c));
    }
    d->config().addCaCertificates(core);
}

void SslConfiguration::setLocalCertificate(const Certificate &certificate)
{
    d->config().setLocalCertificate(certificateCoreOf(certificate));
}

bool SslConfiguration::setLocalCertificate(const QString &path, Ssl::EncodingFormat format)
{
    return d->config().setLocalCertificate(toStdString(path), static_cast<qtng_core::Ssl::EncodingFormat>(format));
}

void SslConfiguration::setPeerVerifyDepth(int depth)
{
    d->config().setPeerVerifyDepth(depth);
}

void SslConfiguration::setPeerVerifyMode(Ssl::PeerVerifyMode mode)
{
    d->config().setPeerVerifyMode(static_cast<qtng_core::Ssl::PeerVerifyMode>(mode));
}

void SslConfiguration::setPrivateKey(const PrivateKey &key)
{
    d->config().setPrivateKey(privateKeyCoreOf(key));
}

bool SslConfiguration::setPrivateKey(const QString &fileName, Ssl::EncodingFormat format, const QByteArray &passPhrase)
{
    return d->config().setPrivateKey(toStdString(fileName), static_cast<qtng_core::Ssl::EncodingFormat>(format),
                                     toStdString(passPhrase));
}

void SslConfiguration::setOnlySecureProtocol(bool onlySecureProtocol)
{
    d->config().setOnlySecureProtocol(onlySecureProtocol);
}

void SslConfiguration::setSupportCompression(bool supportCompression)
{
    d->config().setSupportCompression(supportCompression);
}

void SslConfiguration::setSendTlsExtHostName(bool sendTlsExtHostName)
{
    d->config().setSendTlsExtHostName(sendTlsExtHostName);
}

void SslConfiguration::setTlsExtHostNameCallback(QSharedPointer<ChooseTlsExtNameCallback> callback)
{
    d->config().setTlsExtHostNameCallback(make_shared<QtChooseTlsExtNameCallbackAdapter>(callback));
}

class SslErrorPrivate
{
public:
    qtng_core::SslError core;
};

SslError::SslError()
    : d(new SslErrorPrivate)
{
    d->core = qtng_core::SslError(qtng_core::SslError::NoError);
}

SslError::SslError(Error error)
    : d(new SslErrorPrivate)
{
    d->core = qtng_core::SslError(static_cast<qtng_core::SslError::Error>(error));
}

SslError::SslError(Error error, const Certificate &certificate)
    : d(new SslErrorPrivate)
{
    d->core = qtng_core::SslError(static_cast<qtng_core::SslError::Error>(error), certificateCoreOf(certificate));
}

SslError::SslError(const SslError &other)
    : d(new SslErrorPrivate)
{
    d->core = other.d->core;
}

SslError::~SslError() { }

SslError &SslError::operator=(const SslError &other)
{
    d->core = other.d->core;
    return *this;
}

bool SslError::operator==(const SslError &other) const
{
    return d->core == other.d->core;
}

SslError::Error SslError::error() const
{
    return static_cast<Error>(d->core.error());
}

QString SslError::errorString() const
{
    return toQString(d->core.errorString());
}

Certificate SslError::certificate() const
{
    return toQtCertificate(d->core.certificate());
}

uint qHash(const SslError &key, uint seed)
{
    // 2x boost::hash_combine inlined:
    seed ^= static_cast<int>(key.error()) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= qHash(key.certificate()) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;
}

QDebug &operator<<(QDebug &debug, const SslError &error)
{
    debug << error.errorString();
    return debug;
}

QDebug &operator<<(QDebug &debug, const SslError::Error &error)
{
    debug << toQString(qtng_core::SslError(static_cast<qtng_core::SslError::Error>(error)).errorString());
    return debug;
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

Certificate SslSocket::localCertificate() const
{
    Q_D(const SslSocket);
    return toQtCertificate(d->core->localCertificate());
}

QList<Certificate> SslSocket::localCertificateChain() const
{
    Q_D(const SslSocket);
    QList<Certificate> result;
    for (const qtng_core::Certificate &c : d->core->localCertificateChain()) {
        result.append(toQtCertificate(c));
    }
    return result;
}

SslSocket::SslMode SslSocket::mode() const
{
    Q_D(const SslSocket);
    return static_cast<SslMode>(d->core->mode());
}

Certificate SslSocket::peerCertificate() const
{
    Q_D(const SslSocket);
    return toQtCertificate(d->core->peerCertificate());
}

QList<Certificate> SslSocket::peerCertificateChain() const
{
    Q_D(const SslSocket);
    QList<Certificate> result;
    for (const qtng_core::Certificate &c : d->core->peerCertificateChain()) {
        result.append(toQtCertificate(c));
    }
    return result;
}

int SslSocket::peerVerifyDepth() const
{
    Q_D(const SslSocket);
    return d->core->sslConfiguration().peerVerifyDepth();
}

Ssl::PeerVerifyMode SslSocket::peerVerifyMode() const
{
    Q_D(const SslSocket);
    return static_cast<Ssl::PeerVerifyMode>(d->core->peerVerifyMode());
}

QString SslSocket::peerVerifyName() const
{
    Q_D(const SslSocket);
    return toQString(d->core->peerVerifyName());
}

PrivateKey SslSocket::privateKey() const
{
    Q_D(const SslSocket);
    return toQtPrivateKey(d->core->privateKey());
}

SslCipher SslSocket::cipher() const
{
    Q_D(const SslSocket);
    return SslCipherPrivate::fromCore(d->core->cipher());
}

Ssl::SslProtocol SslSocket::sslProtocol() const
{
    Q_D(const SslSocket);
    return static_cast<Ssl::SslProtocol>(d->core->sslProtocol());
}

SslConfiguration SslSocket::sslConfiguration() const
{
    Q_D(const SslSocket);
    return SslConfigurationPrivate::fromCore(d->core->sslConfiguration());
}

QList<SslError> SslSocket::sslErrors() const
{
    Q_D(const SslSocket);
    QList<SslError> result;
    for (const qtng_core::SslError &e : d->core->sslErrors()) {
        result.append(SslError(static_cast<SslError::Error>(e.error()), toQtCertificate(e.certificate())));
    }
    return result;
}

void SslSocket::setSslConfiguration(const SslConfiguration &configuration)
{
    Q_D(SslSocket);
    d->core->setSslConfiguration(SslConfigurationPrivate::coreOf(configuration));
}

void SslSocket::setPeerVerifyName(const QString &peerVerifyName)
{
    Q_D(SslSocket);
    d->core->setPeerVerifyName(toStdString(peerVerifyName));
}

void SslSocket::setTlsExtHostName(const QString &tlsExtHostName)
{
    Q_D(SslSocket);
    d->core->setTlsExtHostName(toStdString(tlsExtHostName));
}

QSharedPointer<SocketLike> SslSocket::backend() const
{
    Q_D(const SslSocket);
    return toQtSocketLike(d->core->backend());
}

Socket::SocketError SslSocket::error() const
{
    Q_D(const SslSocket);
    return static_cast<Socket::SocketError>(d->core->error());
}

QString SslSocket::errorString() const
{
    Q_D(const SslSocket);
    return toQString(d->core->errorString());
}

bool SslSocket::isValid() const
{
    Q_D(const SslSocket);
    return d->core->isValid();
}

HostAddress SslSocket::localAddress() const
{
    Q_D(const SslSocket);
    return toQtAddress(d->core->localAddress());
}

quint16 SslSocket::localPort() const
{
    Q_D(const SslSocket);
    return d->core->localPort();
}

HostAddress SslSocket::peerAddress() const
{
    Q_D(const SslSocket);
    return toQtAddress(d->core->peerAddress());
}

QString SslSocket::peerName() const
{
    Q_D(const SslSocket);
    return toQString(d->core->peerName());
}

quint16 SslSocket::peerPort() const
{
    Q_D(const SslSocket);
    return d->core->peerPort();
}

Socket::SocketType SslSocket::type() const
{
    Q_D(const SslSocket);
    return static_cast<Socket::SocketType>(d->core->type());
}

Socket::SocketState SslSocket::state() const
{
    Q_D(const SslSocket);
    return static_cast<Socket::SocketState>(d->core->state());
}

HostAddress::NetworkLayerProtocol SslSocket::protocol() const
{
    Q_D(const SslSocket);
    return static_cast<HostAddress::NetworkLayerProtocol>(d->core->protocol());
}

QString SslSocket::localAddressURI() const
{
    Q_D(const SslSocket);
    return toQString(d->core->localAddressURI());
}

QString SslSocket::peerAddressURI() const
{
    Q_D(const SslSocket);
    return toQString(d->core->peerAddressURI());
}

SslSocket *SslSocket::accept()
{
    Q_D(SslSocket);
    qtng_core::SslSocket *core = d->core->accept();
    if (!core) {
        return nullptr;
    }
    SslSocket *result = new SslSocket(HostAddress::IPv4Protocol, SslConfiguration());
    result->d_ptr->core.reset(core);
    return result;
}

Socket *SslSocket::acceptRaw()
{
    Q_D(SslSocket);
    qtng_core::Socket *core = d->core->acceptRaw();
    if (!core) {
        return nullptr;
    }
    Socket *result = new Socket(HostAddress::IPv4Protocol, Socket::TcpSocket);
    assignSocketCore(result, std::shared_ptr<qtng_core::Socket>(core));
    return result;
}

bool SslSocket::bind(const HostAddress &address, quint16 port, Socket::BindMode mode)
{
    Q_D(SslSocket);
    return d->core->bind(toCoreAddress(address), port, static_cast<qtng_core::Socket::BindMode>(mode));
}

bool SslSocket::bind(quint16 port, Socket::BindMode mode)
{
    Q_D(SslSocket);
    return d->core->bind(port, static_cast<qtng_core::Socket::BindMode>(mode));
}

bool SslSocket::connect(const HostAddress &addr, quint16 port)
{
    Q_D(SslSocket);
    return d->core->connect(toCoreAddress(addr), port);
}

bool SslSocket::connect(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache)
{
    Q_D(SslSocket);
    return d->core->connect(toStdString(hostName), port, dnsCacheCoreOf(dnsCache.data()));
}

bool SslSocket::listen(int backlog)
{
    Q_D(SslSocket);
    return d->core->listen(backlog);
}

bool SslSocket::setOption(Socket::SocketOption option, const QVariant &value)
{
    Q_D(SslSocket);
    return d->core->setOption(static_cast<qtng_core::Socket::SocketOption>(option), value.toInt());
}

QVariant SslSocket::option(Socket::SocketOption option) const
{
    Q_D(const SslSocket);
    return d->core->option(static_cast<qtng_core::Socket::SocketOption>(option));
}

qint32 SslSocket::peek(char *data, qint32 size)
{
    Q_D(SslSocket);
    return d->core->peek(data, size);
}

qint32 SslSocket::peekRaw(char *data, qint32 size)
{
    Q_D(SslSocket);
    return d->core->peekRaw(data, size);
}

qint32 SslSocket::recv(char *data, qint32 size)
{
    Q_D(SslSocket);
    return d->core->recv(data, size);
}

qint32 SslSocket::recvall(char *data, qint32 size)
{
    Q_D(SslSocket);
    return d->core->recvall(data, size);
}

qint32 SslSocket::send(const char *data, qint32 size)
{
    Q_D(SslSocket);
    return d->core->send(data, size);
}

qint32 SslSocket::sendall(const char *data, qint32 size)
{
    Q_D(SslSocket);
    return d->core->sendall(data, size);
}

QByteArray SslSocket::recv(qint32 size)
{
    Q_D(SslSocket);
    return toQByteArray(d->core->recv(size));
}

QByteArray SslSocket::recvall(qint32 size)
{
    Q_D(SslSocket);
    return toQByteArray(d->core->recvall(size));
}

qint32 SslSocket::send(const QByteArray &data)
{
    Q_D(SslSocket);
    return d->core->send(toStdString(data));
}

qint32 SslSocket::sendall(const QByteArray &data)
{
    Q_D(SslSocket);
    return d->core->sendall(toStdString(data));
}

SslSocket *SslSocket::createConnection(const HostAddress &host, quint16 port, const SslConfiguration &config,
                                       Socket::SocketError *error, int allowProtocol)
{
    qtng_core::Socket::SocketError coreError = qtng_core::Socket::UnknownSocketError;
    qtng_core::SslSocket *core = qtng_core::SslSocket::createConnection(
            toCoreAddress(host), port, SslConfigurationPrivate::coreOf(config), error ? &coreError : nullptr,
            allowProtocol);
    if (error) {
        *error = static_cast<Socket::SocketError>(coreError);
    }
    if (!core) {
        return nullptr;
    }
    SslSocket *result = new SslSocket(HostAddress::IPv4Protocol, config);
    result->d_ptr->core.reset(core);
    return result;
}

SslSocket *SslSocket::createConnection(const QString &hostName, quint16 port, const SslConfiguration &config,
                                       Socket::SocketError *error, QSharedPointer<SocketDnsCache> dnsCache,
                                       int allowProtocol)
{
    qtng_core::Socket::SocketError coreError = qtng_core::Socket::UnknownSocketError;
    qtng_core::SslSocket *core = qtng_core::SslSocket::createConnection(
            toStdString(hostName), port, SslConfigurationPrivate::coreOf(config), error ? &coreError : nullptr,
            dnsCacheCoreOf(dnsCache.data()), allowProtocol);
    if (error) {
        *error = static_cast<Socket::SocketError>(coreError);
    }
    if (!core) {
        return nullptr;
    }
    SslSocket *result = new SslSocket(HostAddress::IPv4Protocol, config);
    result->d_ptr->core.reset(core);
    return result;
}

SslSocket *SslSocket::createServer(const HostAddress &host, quint16 port, const SslConfiguration &config, int backlog)
{
    qtng_core::SslSocket *core = qtng_core::SslSocket::createServer(
            toCoreAddress(host), port, SslConfigurationPrivate::coreOf(config), backlog);
    if (!core) {
        return nullptr;
    }
    SslSocket *result = new SslSocket(HostAddress::IPv4Protocol, config);
    result->d_ptr->core.reset(core);
    return result;
}

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<SslSocket> s)
{
    if (!s) {
        return QSharedPointer<SocketLike>();
    }
    return ::qtng_bridge::toQtSocketLike(qtng_core::asSocketLike(SslSocketPrivate::coreOf(s.data())));
}

QSharedPointer<SslSocket> convertSocketLikeToSslSocket(QSharedPointer<SocketLike> socket)
{
    if (socket.isNull()) {
        return QSharedPointer<SslSocket>();
    }
    std::shared_ptr<qtng_core::SslSocket> core = qtng_core::convertSocketLikeToSslSocket(toCoreSocketLike(socket));
    if (!core) {
        return QSharedPointer<SslSocket>();
    }
    QSharedPointer<SslSocket> result(new SslSocket(HostAddress::IPv4Protocol, SslConfiguration()));
    SslSocketPrivate::coreOf(result.data()) = core;
    return result;
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

void bindSslConfigurationToCore(QTNETWORKNG_NAMESPACE::SslConfiguration *config, qtng_core::SslConfiguration *core)
{
    QTNETWORKNG_NAMESPACE::SslConfigurationPrivate::bind(config, core);
}

}  // namespace qtng_bridge
