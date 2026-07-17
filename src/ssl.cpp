using namespace std;

#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include "qtng/locks.h"
#include "qtng/ssl.h"
#include "qtng/socket.h"
#include "qtng/private/socket_p.h"
#include "qtng/socket_utils.h"
#include "qtng/private/crypto_p.h"
#include "qtng/io_utils.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/logging.h"

NG_LOGGER("qtng.ssl");

namespace qtng {

class SslCipherPrivate
{
public:
    SslCipherPrivate()
        : supportedBits(0)
        , bits(0)
        , protocol(Ssl::UnknownProtocol)
        , exportable(false)
        , isNull(true)
    {
    }

    static SslCipher from_SSL_CIPHER(const SSL_CIPHER *cipher);

    string name;
    string protocolString;
    string keyExchangeMethod;
    string authenticationMethod;
    string encryptionMethod;
    int supportedBits;
    int bits;
    Ssl::SslProtocol protocol;
    bool exportable;
    bool isNull;
};

// for qt before 5.4
inline vector<string> splitDescription(const string &descriptionOneLine)
{
    vector<string> parts;
    for (const string &part : utils::split(descriptionOneLine, ' ')) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

SslCipher SslCipherPrivate::from_SSL_CIPHER(const SSL_CIPHER *cipher)
{
    SslCipher ciph;
    if (!cipher) {
        return ciph;
    }

    char buf[256];
    char *description = SSL_CIPHER_description(cipher, buf, sizeof(buf));
    if (!description) {
        return ciph;
    }
    string descriptionOneLine = description;

    const vector<string> &descriptionList = splitDescription(descriptionOneLine);
    if (descriptionList.size() > 5) {
        // ### crude code.
        ciph.d->isNull = false;
        ciph.d->name = descriptionList[0];

        const string &protoString = descriptionList[1];
        ciph.d->protocolString = protoString;
        if (protoString == "SSLv3") {
            ciph.d->protocol = Ssl::SslV3;
        } else if (protoString == "SSLv2") {
            ciph.d->protocol = Ssl::SslV2;
        } else if (protoString == "TLSv1") {
            ciph.d->protocol = Ssl::TlsV1_0;
        } else if (protoString == "TLSv1.1") {
            ciph.d->protocol = Ssl::TlsV1_1;
        } else if (protoString == "TLSv1.2") {
            ciph.d->protocol = Ssl::TlsV1_2;
        } else if (protoString == "TLSv1.3") {
            ciph.d->protocol = Ssl::TlsV1_3;
        } else {
            ciph.d->protocol = Ssl::UnknownProtocol;
        }
        if (utils::startsWith(descriptionList[2], "Kx="))
            ciph.d->keyExchangeMethod = descriptionList[2].substr(3);
        if (utils::startsWith(descriptionList[3], "Au="))
            ciph.d->authenticationMethod = descriptionList[3].substr(3);
        if (utils::startsWith(descriptionList[4], "Enc="))
            ciph.d->encryptionMethod = descriptionList[4].substr(4);
        ciph.d->exportable = (descriptionList.size() > 6 && descriptionList[6] == "export");

        ciph.d->bits = SSL_CIPHER_get_bits(cipher, &ciph.d->supportedBits);
    }
    return ciph;
}

SslCipher::SslCipher()
    : d(new SslCipherPrivate)
{
}

SslCipher::SslCipher(const string &name)
    : d(new SslCipherPrivate)
{
    const vector<SslCipher> &ciphers = SslConfiguration::supportedCiphers();
    for (const SslCipher &cipher : ciphers) {
        if (cipher.name() == name) {
            *this = cipher;
            return;
        }
    }
}

SslCipher::SslCipher(const string &name, Ssl::SslProtocol protocol)
    : d(new SslCipherPrivate)
{
    const vector<SslCipher> &ciphers = SslConfiguration::supportedCiphers();
    for (const SslCipher &cipher : ciphers) {
        if (cipher.name() == name && cipher.protocol() == protocol) {
            *this = cipher;
            return;
        }
    }
}

SslCipher::SslCipher(const SslCipher &other)
    : d(new SslCipherPrivate)
{
    *d.get() = *other.d.get();
}

SslCipher::~SslCipher() { }

SslCipher &SslCipher::operator=(const SslCipher &other)
{
    *d.get() = *other.d.get();
    return *this;
}

bool SslCipher::operator==(const SslCipher &other) const
{
    return d->name == other.d->name && d->protocol == other.d->protocol;
}

bool SslCipher::isNull() const
{
    return d->isNull;
}

string SslCipher::name() const
{
    return d->name;
}

int SslCipher::supportedBits() const
{
    return d->supportedBits;
}

int SslCipher::usedBits() const
{
    return d->bits;
}

string SslCipher::keyExchangeMethod() const
{
    return d->keyExchangeMethod;
}

string SslCipher::authenticationMethod() const
{
    return d->authenticationMethod;
}

string SslCipher::encryptionMethod() const
{
    return d->encryptionMethod;
}

string SslCipher::protocolString() const
{
    return d->protocolString;
}

Ssl::SslProtocol SslCipher::protocol() const
{
    return d->protocol;
}

ostream &operator<<(ostream &debug, const SslCipher &cipher)
{
    debug << "SslCipher(name=" << cipher.name() << ", bits=" << cipher.usedBits()
          << ", proto=" << cipher.protocolString() << ')';
    return debug;
}

class SslConfigurationPrivate 
{
public:
    SslConfigurationPrivate();
    SslConfigurationPrivate(const SslConfigurationPrivate &) = default;
    bool isNull() const;
    bool operator==(const SslConfigurationPrivate &other) const;
    static shared_ptr<SSL_CTX> makeContext(const SslConfiguration &config, bool asServer);
    void setSendTlsExtHostName(bool sendTlsExtHostName);

    vector<Certificate> caCertificates;
    Certificate localCertificate;
    PrivateKey privateKey;
    vector<string> allowedNextProtocols;
    Ssl::PeerVerifyMode peerVerifyMode;
    vector<SslCipher> ciphers;
    shared_ptr<ChooseTlsExtNameCallback> chooseTlsExtNameCallback;
    int peerVerifyDepth;
    bool onlySecureProtocol;
    bool supportCompression;
};

bool SslConfigurationPrivate::operator==(const SslConfigurationPrivate &other) const
{
    return caCertificates == other.caCertificates && localCertificate == other.localCertificate
            && privateKey == other.privateKey && allowedNextProtocols == other.allowedNextProtocols
            && peerVerifyMode == other.peerVerifyMode && ciphers == other.ciphers
            && chooseTlsExtNameCallback == other.chooseTlsExtNameCallback && peerVerifyDepth == other.peerVerifyDepth
            && onlySecureProtocol == other.onlySecureProtocol && supportCompression == other.supportCompression;
}

bool SslConfigurationPrivate::isNull() const
{
    return caCertificates.empty() && localCertificate.isNull() && !privateKey.isValid()
            && allowedNextProtocols.empty() && peerVerifyMode == Ssl::AutoVerifyPeer && ciphers.empty()
            && !chooseTlsExtNameCallback && peerVerifyDepth == 4 && onlySecureProtocol == true
            && supportCompression == true;
}

SslConfigurationPrivate::SslConfigurationPrivate()
    : peerVerifyMode(Ssl::AutoVerifyPeer)
    , peerVerifyDepth(4)
    , onlySecureProtocol(true)
    , supportCompression(true)
{
    setSendTlsExtHostName(true);
}

class AlwaysTheSameChooseTlsExtNameCallback : public ChooseTlsExtNameCallback
{
    virtual string choose(const string &hostName) override { return hostName; }
};

void SslConfigurationPrivate::setSendTlsExtHostName(bool sendTlsExtHostName)
{
    static shared_ptr<ChooseTlsExtNameCallback> defaultCallback(new AlwaysTheSameChooseTlsExtNameCallback());
    if (sendTlsExtHostName) {
        chooseTlsExtNameCallback = defaultCallback;
    } else {
        chooseTlsExtNameCallback.reset();
    }
}

shared_ptr<SSL_CTX> SslConfigurationPrivate::makeContext(const SslConfiguration &config, bool asServer)
{
    shared_ptr<SSL_CTX> ctx;
    const SSL_METHOD *method = nullptr;
    if (asServer) {
        method = SSLv23_server_method();
    } else {
        method = SSLv23_client_method();
    }
    if (!method) {
        return ctx;
    }
    ctx.reset(SSL_CTX_new(method), SSL_CTX_free);
    if (!ctx) {
        return ctx;
    }
    SSL_CTX_set_verify_depth(ctx.get(), config.peerVerifyDepth());
    long flags = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1;
    if (config.onlySecureProtocol()) {
        flags |= SSL_OP_NO_TLSv1_1;
    }
    if (!config.supportCompression()) {
        flags |= SSL_OP_NO_COMPRESSION;
    }
    SSL_CTX_set_options(ctx.get(), flags);
    const PrivateKey &privateKey = config.privateKey();
    if (privateKey.isValid()) {
        int r = SSL_CTX_use_PrivateKey(ctx.get(), static_cast<EVP_PKEY *>(privateKey.handle()));
        if (!r) {
            ngDebug() << "can not set ssl private key.";
        }
    }
    const Certificate &localCertificate = config.localCertificate();
    if (localCertificate.isValid()) {
        int r = SSL_CTX_use_certificate(ctx.get(), static_cast<X509 *>(localCertificate.handle()));
        if (!r) {
            ngDebug() << "can not set ssl certificate.";
        }
    }
    return ctx;
}

SslConfiguration::SslConfiguration()
    : d(new SslConfigurationPrivate())
{
}

SslConfiguration::SslConfiguration(const SslConfiguration &other)
    : d(other.d)
{
}

#ifdef Q_COMPILER_RVALUE_REFS

SslConfiguration::SslConfiguration(SslConfiguration &&other)
    : d(nullptr)
{
    swap(d, other.d);
}

#endif

SslConfiguration::~SslConfiguration() { }

SslConfiguration &SslConfiguration::operator=(const SslConfiguration &other)
{
    if (this == &other) {
        return *this;
    }
    d = other.d;
    return *this;
}

bool SslConfiguration::operator==(const SslConfiguration &other) const
{
    if (d == other.d) {
        return true;
    }
    return d->operator==(*other.d);
}

bool SslConfiguration::isNull() const
{
    return d->isNull();
}

vector<string> SslConfiguration::allowedNextProtocols() const
{
    return d->allowedNextProtocols;
}

vector<Certificate> SslConfiguration::caCertificates() const
{
    return d->caCertificates;
}

vector<SslCipher> SslConfiguration::ciphers() const
{
    return d->ciphers;
}

Certificate SslConfiguration::localCertificate() const
{
    return d->localCertificate;
}

Ssl::PeerVerifyMode SslConfiguration::peerVerifyMode() const
{
    return d->peerVerifyMode;
}

int SslConfiguration::peerVerifyDepth() const
{
    return d->peerVerifyDepth;
}

PrivateKey SslConfiguration::privateKey() const
{
    return d->privateKey;
}

bool SslConfiguration::onlySecureProtocol() const
{
    return d->onlySecureProtocol;
}

bool SslConfiguration::supportCompression() const
{
    return d->supportCompression;
}

bool SslConfiguration::sendTlsExtHostName() const
{
    return static_cast<bool>(d->chooseTlsExtNameCallback);
}

shared_ptr<ChooseTlsExtNameCallback> SslConfiguration::tlsExtHostNameCallback() const
{
    return d->chooseTlsExtNameCallback;
}

void SslConfiguration::addCaCertificate(const Certificate &certificate)
{
    d->caCertificates.push_back(certificate);
}

void SslConfiguration::addCaCertificates(const vector<Certificate> &certificates)
{
    d->caCertificates.insert(d->caCertificates.end(), certificates.begin(), certificates.end());
}

void SslConfiguration::setAllowedNextProtocols(const vector<string> &protocols)
{
    d->allowedNextProtocols = protocols;
}

void SslConfiguration::setPeerVerifyDepth(int depth)
{
    d->peerVerifyDepth = depth;
}

void SslConfiguration::setPeerVerifyMode(Ssl::PeerVerifyMode mode)
{
    d->peerVerifyMode = mode;
}

void SslConfiguration::setLocalCertificate(const Certificate &certificate)
{
    d->localCertificate = certificate;
}

bool SslConfiguration::setLocalCertificate(const string &path, Ssl::EncodingFormat format)
{
    const string buf = PosixPath(path).readall(nullptr);
    if (buf.empty()) {
        return false;
    }

    const Certificate cert = Certificate::load(buf, format);
    if (cert.isNull() || cert.isBlacklisted()) {
        return false;
    }
    d->localCertificate = cert;
    return true;
}

void SslConfiguration::setPrivateKey(const PrivateKey &key)
{
    d->privateKey = key;
}

bool SslConfiguration::setPrivateKey(const string &fileName, Ssl::EncodingFormat format, const string &passPhrase)
{
    const string buf = PosixPath(fileName).readall(nullptr);
    if (buf.empty()) {
        return false;
    }
    const PrivateKey key = PrivateKey::load(buf, format, passPhrase);
    if (!key.isValid()) {
        return false;
    }
    d->privateKey = key;
    return true;
}

void SslConfiguration::setOnlySecureProtocol(bool onlySecureProtocol)
{
    d->onlySecureProtocol = onlySecureProtocol;
}

void SslConfiguration::setSupportCompression(bool supportCompression)
{
    d->supportCompression = supportCompression;
}

void SslConfiguration::setSendTlsExtHostName(bool sendTlsExtHostName)
{
    d->setSendTlsExtHostName(sendTlsExtHostName);
}

void SslConfiguration::setTlsExtHostNameCallback(shared_ptr<ChooseTlsExtNameCallback> callback)
{
    d->chooseTlsExtNameCallback = callback;
}

vector<SslCipher> SslConfiguration::supportedCiphers()
{
    return vector<SslCipher>();
}

SslConfiguration SslConfiguration::testPurpose(const string &commonName, const string &countryCode,
                                               const string &organization)
{
    PrivateKey key = PrivateKey::generate(PrivateKey::Rsa, 2048);
    multimap<Certificate::SubjectInfo, string> info;
    info.insert({Certificate::CommonName, commonName});
    info.insert({Certificate::CountryName, countryCode});
    info.insert({Certificate::Organization, organization});
    const utils::DateTime now = utils::DateTime::currentDateTimeUtc();
    const Certificate cert = Certificate::selfSign(key, MessageDigest::Sha256, 293424, now,
                                                   now.addSecs(10LL * 365 * 24 * 3600), info);
    SslConfiguration config;
    config.setPrivateKey(key);
    config.setLocalCertificate(cert);
    return config;
}

class SslErrorPrivate
{
public:
    Certificate certificate;
    SslError::Error error;
};

SslError::SslError()
    : d(new SslErrorPrivate)
{
    d->error = SslError::NoError;
    d->certificate = Certificate();
}

SslError::SslError(Error error)
    : d(new SslErrorPrivate)
{
    d->error = error;
    d->certificate = Certificate();
}

SslError::SslError(Error error, const Certificate &certificate)
    : d(new SslErrorPrivate)
{
    d->error = error;
    d->certificate = certificate;
}

SslError::SslError(const SslError &other)
    : d(new SslErrorPrivate)
{
    *d.get() = *other.d.get();
}

SslError::~SslError() { }

SslError &SslError::operator=(const SslError &other)
{
    *d.get() = *other.d.get();
    return *this;
}

bool SslError::operator==(const SslError &other) const
{
    return d->error == other.d->error && d->certificate == other.d->certificate;
}

SslError::Error SslError::error() const
{
    return d->error;
}

string SslError::errorString() const
{
    string errStr;
    switch (d->error) {
    case NoError:
        errStr = "No error";
        break;
    case UnableToGetIssuerCertificate:
        errStr = "The issuer certificate could not be found";
        break;
    case UnableToDecryptCertificateSignature:
        errStr = "The certificate signature could not be decrypted";
        break;
    case UnableToDecodeIssuerPublicKey:
        errStr = "The public key in the certificate could not be read";
        break;
    case CertificateSignatureFailed:
        errStr = "The signature of the certificate is invalid";
        break;
    case CertificateNotYetValid:
        errStr = "The certificate is not yet valid";
        break;
    case CertificateExpired:
        errStr = "The certificate has expired";
        break;
    case InvalidNotBeforeField:
        errStr = "The certificate's notBefore field contains an invalid time";
        break;
    case InvalidNotAfterField:
        errStr = "The certificate's notAfter field contains an invalid time";
        break;
    case SelfSignedCertificate:
        errStr = "The certificate is self-signed, and untrusted";
        break;
    case SelfSignedCertificateInChain:
        errStr = "The root certificate of the certificate chain is self-signed, and untrusted";
        break;
    case UnableToGetLocalIssuerCertificate:
        errStr = "The issuer certificate of a locally looked up certificate could not be found";
        break;
    case UnableToVerifyFirstCertificate:
        errStr = "No certificates could be verified";
        break;
    case InvalidCaCertificate:
        errStr = "One of the CA certificates is invalid";
        break;
    case PathLengthExceeded:
        errStr = "The basicConstraints path length parameter has been exceeded";
        break;
    case InvalidPurpose:
        errStr = "The supplied certificate is unsuitable for this purpose";
        break;
    case CertificateUntrusted:
        errStr = "The root CA certificate is not trusted for this purpose";
        break;
    case CertificateRejected:
        errStr = "The root CA certificate is marked to reject the specified purpose";
        break;
    case SubjectIssuerMismatch:  // hostname mismatch
        errStr = "The current candidate issuer certificate was rejected because its"
                                     " subject name did not match the issuer name of the current certificate";
        break;
    case AuthorityIssuerSerialNumberMismatch:
        errStr = "The current candidate issuer certificate was rejected because"
                                     " its issuer name and serial number was present and did not match the"
                                     " authority key identifier of the current certificate";
        break;
    case NoPeerCertificate:
        errStr = "The peer did not present any certificate";
        break;
    case HostNameMismatch:
        errStr = "The host name did not match any of the valid hosts"
                                     " for this certificate";
        break;
    case NoSslSupport:
        errStr = "SSL is not supported on this pltform.";
        break;
    case CertificateBlacklisted:
        errStr = "The peer certificate is blacklisted";
        break;
    default:
        errStr = "Unknown error";
        break;
    }

    return errStr;
}

Certificate SslError::certificate() const
{
    return d->certificate;
}

uint qHash(const SslError &key, uint seed)
{
    // 2x boost::hash_combine inlined:
    seed ^= static_cast<int>(key.error()) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= qHash(key.certificate()) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;
}

ostream &operator<<(ostream &debug, const SslError &error)
{
    debug << error.errorString();
    return debug;
}

ostream &operator<<(ostream &debug, const SslError::Error &error)
{
    debug << SslError(error).errorString();
    return debug;
}

/*
static SslError _q_OpenSSL_to_SslError(int errorCode, const Certificate &cert)
{
    SslError error;
    switch (errorCode) {
    case X509_V_OK:
        // X509_V_OK is also reported if the peer had no certificate.
        break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        error = SslError(SslError::UnableToGetIssuerCertificate, cert); break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
        error = SslError(SslError::UnableToDecryptCertificateSignature, cert); break;
    case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
        error = SslError(SslError::UnableToDecodeIssuerPublicKey, cert); break;
    case X509_V_ERR_CERT_SIGNATURE_FAILURE:
        error = SslError(SslError::CertificateSignatureFailed, cert); break;
    case X509_V_ERR_CERT_NOT_YET_VALID:
        error = SslError(SslError::CertificateNotYetValid, cert); break;
    case X509_V_ERR_CERT_HAS_EXPIRED:
        error = SslError(SslError::CertificateExpired, cert); break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
        error = SslError(SslError::InvalidNotBeforeField, cert); break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
        error = SslError(SslError::InvalidNotAfterField, cert); break;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        error = SslError(SslError::SelfSignedCertificate, cert); break;
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        error = SslError(SslError::SelfSignedCertificateInChain, cert); break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        error = SslError(SslError::UnableToGetLocalIssuerCertificate, cert); break;
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        error = SslError(SslError::UnableToVerifyFirstCertificate, cert); break;
    case X509_V_ERR_CERT_REVOKED:
        error = SslError(SslError::CertificateRevoked, cert); break;
    case X509_V_ERR_INVALID_CA:
        error = SslError(SslError::InvalidCaCertificate, cert); break;
    case X509_V_ERR_PATH_LENGTH_EXCEEDED:
        error = SslError(SslError::PathLengthExceeded, cert); break;
    case X509_V_ERR_INVALID_PURPOSE:
        error = SslError(SslError::InvalidPurpose, cert); break;
    case X509_V_ERR_CERT_UNTRUSTED:
        error = SslError(SslError::CertificateUntrusted, cert); break;
    case X509_V_ERR_CERT_REJECTED:
        error = SslError(SslError::CertificateRejected, cert); break;
    default:
        error = SslError(SslError::UnspecifiedError, cert); break;
    }
    return error;
}
*/

template<typename SocketType>
class SslConnection
{
public:
    SslConnection(const SslConfiguration &config);
    SslConnection();
    ~SslConnection();
    bool handshake(bool asServer, const string &hostName);
    bool _handshake();
    bool close();
    void abort();
    int32_t peek(char *data, int32_t size);
    int32_t peekRaw(char *data, int32_t size);
    int32_t recv(char *data, int32_t size, bool all);
    int32_t send(const char *data, int32_t size, bool all);
    bool pumpOutgoing();
    bool pumpIncoming();
    Certificate localCertificate() const;
    vector<Certificate> localCertificateChain() const;
    Certificate peerCertificate() const;
    vector<Certificate> peerCertificateChain() const;
    Ssl::PeerVerifyMode peerVerifyMode() const;
    SslCipher cipher() const;
    SslSocket::SslMode mode() const;
    Ssl::SslProtocol sslProtocol() const;

    shared_ptr<SocketType> rawSocket;
    SslConfiguration config;
    shared_ptr<SSL_CTX> ctx;
    shared_ptr<SSL> ssl;
    vector<SslError> errors;
    string peerVerifyName;
    string tlsExtHostName;
    bool asServer;
};

template<typename SocketType>
SslConnection<SocketType>::SslConnection(const SslConfiguration &config)
    : config(config)
{
    initOpenSSL();
}

template<typename SocketType>
SslConnection<SocketType>::SslConnection()
{
    initOpenSSL();
}

template<typename SocketType>
SslConnection<SocketType>::~SslConnection()
{
    rawSocket->abort();
    cleanupOpenSSL();
}

template<typename SocketType>
bool SslConnection<SocketType>::handshake(bool asServer, const string &hostName)
{
    // FIXME use verifyMode to set verifyPeerName
    if (tlsExtHostName.empty() && !hostName.empty()) {
        tlsExtHostName = hostName;
    }
    if (!rawSocket) {
        return false;
    }
    this->asServer = asServer;

    BIO *incoming = BIO_new(BIO_s_mem());
    if (!incoming) {
        return false;
    }
    BIO *outgoing = BIO_new(BIO_s_mem());
    if (!outgoing) {
        BIO_free(incoming);
        return false;
    }

    ctx = SslConfigurationPrivate::makeContext(config, asServer);
    bool freeBIOs = true;
    if (ctx) {
        ssl.reset(SSL_new(ctx.get()), SSL_free);
        if (ssl) {
            // do not free incoming & outgoing
            freeBIOs = false;
            SSL_set_bio(ssl.get(), incoming, outgoing);
            if (!asServer && !tlsExtHostName.empty()) {
                shared_ptr<ChooseTlsExtNameCallback> callback = config.tlsExtHostNameCallback();
                if (callback) {
                    const string &t = callback->choose(tlsExtHostName);
                    if (!t.empty()) {
                        SSL_set_tlsext_host_name(ssl.get(), t.data());
                    }
                }
            }
            if (_handshake()) {
                return true;
            }
            ssl.reset();
        }
        ctx.reset();
    }
    if (freeBIOs) {
        BIO_free(incoming);
        BIO_free(outgoing);
    }
    return false;
}

template<typename SocketType>
bool SslConnection<SocketType>::pumpOutgoing()
{
    if (!ssl) {
        ngWarning() << "ssl is null while pump outgoing.";
        return false;
    }
    int pendingBytes;
    array<char, 1024 * 8> buf;
    BIO *outgoing = SSL_get_wbio(ssl.get());
    assert(outgoing);
    while ((pendingBytes = BIO_pending(outgoing)) > 0) {
        int32_t encryptedBytesRead = BIO_read(outgoing, buf.data(), pendingBytes);
        int32_t actualWritten = rawSocket->sendall(buf.data(), encryptedBytesRead);
        if (actualWritten < encryptedBytesRead) {
            return false;
        }
    }
    return true;
}

template<typename SocketType>
bool SslConnection<SocketType>::pumpIncoming()
{
    if (!ssl) {
        ngWarning() << "ssl is null while pump incoming.";
        return false;
    }
    const string &buf = rawSocket->recv(1024 * 8);
    if (buf.empty()) {
        return false;
    }
    int totalWritten = 0;
    BIO *incoming = SSL_get_rbio(ssl.get());
    assert(incoming);
    while (totalWritten < buf.size()) {
        int writtenToBio = BIO_write(incoming, buf.data() + totalWritten, buf.size() - totalWritten);
        if (writtenToBio > 0) {
            totalWritten += writtenToBio;
        } else {
            ngDebug() << "Unable to decrypt data";
            return false;
        }
    };
    return true;
}

template<typename SocketType>
bool SslConnection<SocketType>::_handshake()
{
    assert(ssl);
    while (true) {
        int result = asServer ? SSL_accept(ssl.get()) : SSL_connect(ssl.get());
        if (result <= 0) {
            int err = SSL_get_error(ssl.get(), result);
            switch (err) {
            case SSL_ERROR_WANT_READ:
                if (!pumpOutgoing())
                    return false;
                if (!pumpIncoming())
                    return false;
                break;
            case SSL_ERROR_WANT_WRITE:
                if (!pumpOutgoing())
                    return false;
                break;
            case SSL_ERROR_ZERO_RETURN:
            case SSL_ERROR_WANT_CONNECT:
            case SSL_ERROR_WANT_ACCEPT:
            case SSL_ERROR_WANT_X509_LOOKUP:
                //            case SSL_ERROR_WANT_CLIENT_HELLO_CB:
                ngDebug() << "invalid ssl connection state.";
                return false;
            case SSL_ERROR_SYSCALL:
            case SSL_ERROR_SSL: {
                ERR_get_error();
                // ngDebug() << "protocol error on handshake. lib:" << ERR_GET_LIB(sslerror) << "reason:" << ERR_GET_REASON(sslerror);
                return false;
            }
            default:
                ngDebug() << "handshake error.";
                return false;
            }
        } else {
            return true;
        }
    }
}

template<typename SocketType>
int32_t SslConnection<SocketType>::peek(char *data, int32_t size)
{
    if (!ssl) {
        return -1;
    }
    int result = SSL_peek(ssl.get(), data, size);
    if (result <= 0) {
        int err = SSL_get_error(ssl.get(), result);
        switch (err) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
        case SSL_ERROR_ZERO_RETURN:
            return 0;
        case SSL_ERROR_SYSCALL:
        case SSL_ERROR_SSL: {
            unsigned long sslerror = ERR_get_error();
            ngDebug() << "ssl peek error. lib:" << ERR_GET_LIB(sslerror) << "reason:" << ERR_GET_REASON(sslerror);
            return -1;
        }
        case SSL_ERROR_WANT_CONNECT:
        case SSL_ERROR_WANT_ACCEPT:
        case SSL_ERROR_WANT_X509_LOOKUP:
            //            case SSL_ERROR_WANT_CLIENT_HELLO_CB:
        default:
            ngDebug() << "ssl recv error. error code:" << err;
            return -1;
        }
    }
    return result;
}

template<typename SocketType>
int32_t SslConnection<SocketType>::peekRaw(char *data, int32_t size)
{
    if (!ssl) {
        return -1;
    }
    return rawSocket->peek(data, size);
}

template<typename SocketType>
int32_t SslConnection<SocketType>::recv(char *data, int32_t size, bool all)
{
    if (!ssl) {
        return -1;
    }
    int32_t total = 0;
    while (true) {
        int result = SSL_read(ssl.get(), data + total, size - total);
        if (result <= 0) {
            int err = SSL_get_error(ssl.get(), result);
            switch (err) {
            case SSL_ERROR_WANT_READ:
                if (!pumpOutgoing() || !pumpIncoming()) {
                    return total == 0 ? -1 : total;
                }
                break;
            case SSL_ERROR_WANT_WRITE:
                if (!pumpOutgoing()) {
                    return total == 0 ? -1 : total;
                }
                break;
            case SSL_ERROR_ZERO_RETURN:
                ngDebug() << "error zero return." << total;
                return total;
            case SSL_ERROR_SYSCALL:
            case SSL_ERROR_SSL: {
                unsigned long sslerror = ERR_get_error();
                ngDebug() << "ssl recv error. lib:" << ERR_GET_LIB(sslerror) << "reason:" << ERR_GET_REASON(sslerror);
                return total == 0 ? -1 : total;
            }
            case SSL_ERROR_WANT_CONNECT:
            case SSL_ERROR_WANT_ACCEPT:
            case SSL_ERROR_WANT_X509_LOOKUP:
                //            case SSL_ERROR_WANT_CLIENT_HELLO_CB:
            default:
                ngDebug() << "ssl recv error. error code:" << err;
                return total == 0 ? -1 : total;
            }
        } else {
            total += result;
            if (all && total < size) {
                continue;
            } else {
                return total;
            }
        }
    }
}

template<typename SocketType>
int32_t SslConnection<SocketType>::send(const char *data, int32_t size, bool all)
{
    if (!ssl || size <= 0) {
        return -1;
    }
    int32_t total = 0;
    // be careful for dead lock
    while (true) {
        int result = SSL_write(ssl.get(), data + total, size - total);
        if (result <= 0) {
            int err = SSL_get_error(ssl.get(), result);
            switch (err) {
            case SSL_ERROR_WANT_READ:
                if (!pumpOutgoing() || !pumpIncoming()) {
                    return -1;
                }
                break;
            case SSL_ERROR_WANT_WRITE:
                if (!pumpOutgoing()) {
                    return -1;
                }
                break;
            case SSL_ERROR_NONE:
                break;
            case SSL_ERROR_ZERO_RETURN:
                // may the remote peer close the connection.
                return -1;
            case SSL_ERROR_SYSCALL:
            case SSL_ERROR_SSL: {
                unsigned long sslerror = ERR_get_error();
                char buf[256];
                ERR_error_string_n(sslerror, buf, 256);
                ngDebug() << "ssl send error. lib:" << ERR_GET_LIB(sslerror) << "reason:" << buf;
                return -1;
            }
            case SSL_ERROR_WANT_CONNECT:
            case SSL_ERROR_WANT_ACCEPT:
            case SSL_ERROR_WANT_X509_LOOKUP:
                //            case SSL_ERROR_WANT_CLIENT_HELLO_CB:
            default:
                ngDebug() << "ssl send error. error_code:" << err;
                return -1;
            }
        } else {
            total += result;
            if ((total > size)) {
                ngDebug() << "send too many data.";
                if (!pumpOutgoing())
                    return -1;
                return size;
            } else if ((total == size)) {
                if (!pumpOutgoing())
                    return -1;
                return total;
            } else {
                if (all) {
                    continue;
                } else {
                    if (!pumpOutgoing())
                        return -1;
                    return total;
                }
            }
        }
    }
}

template<typename SocketType>
bool SslConnection<SocketType>::close()
{
    if (!ssl || !rawSocket->isValid()) {
        return false;
    }
    int tried = 0;
    while (true) {
        int result = SSL_shutdown(ssl.get());
        if (result < 0) {
            int err = SSL_get_error(ssl.get(), result);
            switch (err) {
            case SSL_ERROR_WANT_READ:
                if (!pumpOutgoing())
                    return false;
                if (!pumpIncoming())
                    return false;
                break;
            case SSL_ERROR_WANT_WRITE:
                if (!pumpOutgoing())
                    return false;
                break;
            case SSL_ERROR_NONE:
            case SSL_ERROR_ZERO_RETURN:
                return true;
            case SSL_ERROR_WANT_CONNECT:
            case SSL_ERROR_WANT_ACCEPT:
            case SSL_ERROR_WANT_X509_LOOKUP:
                //            case SSL_ERROR_WANT_CLIENT_HELLO_CB:
                ngDebug() << "what?";
                return false;
            case SSL_ERROR_SYSCALL:
            case SSL_ERROR_SSL: {
                unsigned long sslerror = ERR_get_error();
                ngDebug() << "underlying socket is closed. lib:" << ERR_GET_LIB(sslerror) << "reason:" << ERR_GET_REASON(sslerror);
                return false;
            }
            default:
                ngDebug() << "unkown returned value of SSL_shutdown().";
                return false;
            }
        } else if (result > 0) {
            return true;
        } else {  // result == 0
            // process the second SSL_shutdown();
            // https://www.openssl.org/docs/manmaster/man3/SSL_shutdown.html
            if (tried > 0) {
                return 0;
            } else {
                ++tried;
            }
        }
    }
}

template<typename SocketType>
void SslConnection<SocketType>::abort()
{
    rawSocket->abort();
}

template<typename SocketType>
Certificate SslConnection<SocketType>::localCertificate() const
{
    Certificate cert;
    if (ssl) {
        X509 *x = SSL_get_certificate(ssl.get());
        if (x) {
            openssl_setCertificate(&cert, x);
        }
    }
    return cert;
}

template<typename SocketType>
Certificate SslConnection<SocketType>::peerCertificate() const
{
    Certificate cert;
    if (ssl) {
        X509 *x = SSL_get_peer_certificate(ssl.get());
        if (x) {
            openssl_setCertificate(&cert, x);
        }
    }
    return cert;
}

vector<Certificate> STACKOFX509_to_Certificates(STACK_OF(X509) * x509)
{
    vector<Certificate> certificates;
    for (int i = 0; i < sk_X509_num(x509); ++i) {
        if (X509 *entry = static_cast<X509 *>(sk_X509_value(x509, i))) {
            Certificate cert;
            openssl_setCertificate(&cert, entry);
            certificates.push_back(cert);
        }
    }
    return certificates;
}

template<typename SocketType>
vector<Certificate> SslConnection<SocketType>::localCertificateChain() const
{
    vector<Certificate> certificates;
    if (!ssl)
        return certificates;
    // FIXME store in sslconfig.
    return certificates;
}

template<typename SocketType>
vector<Certificate> SslConnection<SocketType>::peerCertificateChain() const
{
    vector<Certificate> certificates;
    if (!ssl)
        return certificates;

    STACK_OF(X509) *x509 = SSL_get_peer_cert_chain(ssl.get());
    if (x509) {
        certificates = STACKOFX509_to_Certificates(x509);
    }
    return certificates;
}

template<typename SocketType>
Ssl::PeerVerifyMode SslConnection<SocketType>::peerVerifyMode() const
{
    if (!ssl) {
        return Ssl::AutoVerifyPeer;
    }
    int mode = SSL_get_verify_mode(ssl.get());
    if (mode == SSL_VERIFY_NONE) {
        return Ssl::VerifyNone;
    } else if (mode == SSL_VERIFY_PEER) {
        return Ssl::VerifyPeer;
    } else if (mode == SSL_VERIFY_FAIL_IF_NO_PEER_CERT) {
        return Ssl::QueryPeer;
    } else if (mode == SSL_VERIFY_CLIENT_ONCE) {
        return Ssl::AutoVerifyPeer;
    } else {
        return Ssl::AutoVerifyPeer;
    }
}

template<typename SocketType>
SslCipher SslConnection<SocketType>::cipher() const
{
    if (!ssl) {
        return SslCipher();
    }
    const SSL_CIPHER *sessionCipher = SSL_get_current_cipher(ssl.get());
    return SslCipherPrivate::from_SSL_CIPHER(sessionCipher);
}

template<typename SocketType>
SslSocket::SslMode SslConnection<SocketType>::mode() const
{
    if (asServer) {
        return SslSocket::SslServerMode;
    } else {
        return SslSocket::SslClientMode;
    }
}

template<typename SocketType>
Ssl::SslProtocol SslConnection<SocketType>::sslProtocol() const
{
    if (!ssl)
        return Ssl::UnknownProtocol;
    int ver = SSL_version(ssl.get());
    switch (ver) {
    case 0x2:
        return Ssl::SslV2;
    case 0x300:
        return Ssl::SslV3;
    case 0x301:
        return Ssl::TlsV1_0;
    case 0x302:
        return Ssl::TlsV1_1;
    case 0x303:
        return Ssl::TlsV1_2;
    }
    return Ssl::UnknownProtocol;
}

class SslSocketPrivate : public SslConnection<SocketLike>
{
public:
    SslSocketPrivate(const SslConfiguration &config);
    bool isValid() const;

    string errorString;
    Socket::SocketError error;
};

SslSocketPrivate::SslSocketPrivate(const SslConfiguration &config)
    : SslConnection<SocketLike>(config)
    , error(Socket::NoError)
{
}

bool SslSocketPrivate::isValid() const
{
    if (error != Socket::NoError) {
        return false;
    } else {
        return rawSocket->isValid();
    }
}

SslSocket::SslSocket(HostAddress::NetworkLayerProtocol protocol, const SslConfiguration &config)
    : d_ptr(new SslSocketPrivate(config))
{
    NG_D(SslSocket);
    d->rawSocket = asSocketLike(new Socket(protocol));
    d->asServer = false;
}

SslSocket::SslSocket(intptr_t socketDescriptor, const SslConfiguration &config)
    : d_ptr(new SslSocketPrivate(config))
{
    NG_D(SslSocket);
    d->rawSocket = asSocketLike(new Socket(socketDescriptor));
    d->asServer = false;
}

SslSocket::SslSocket(shared_ptr<Socket> rawSocket, const SslConfiguration &config)
    : d_ptr(new SslSocketPrivate(config))
{
    NG_D(SslSocket);
    d->rawSocket = asSocketLike(rawSocket);
    d->asServer = false;
}

SslSocket::SslSocket(shared_ptr<SocketLike> rawSocket, const SslConfiguration &config)
    : d_ptr(new SslSocketPrivate(config))
{
    NG_D(SslSocket);
    d->rawSocket = rawSocket;
    d->asServer = false;
}

SslSocket::~SslSocket()
{
    delete d_ptr;
}

bool SslSocket::handshake(bool asServer, const string &hostName)
{
    NG_D(SslSocket);
    if (d->ssl) {
        return false;
    }
    return d->handshake(asServer, hostName);
}

Certificate SslSocket::localCertificate() const
{
    NG_D(const SslSocket);
    return d->localCertificate();
}

vector<Certificate> SslSocket::localCertificateChain() const
{
    NG_D(const SslSocket);
    return d->localCertificateChain();
}

Certificate SslSocket::peerCertificate() const
{
    NG_D(const SslSocket);
    return d->peerCertificate();
}

vector<Certificate> SslSocket::peerCertificateChain() const
{
    NG_D(const SslSocket);
    return d->peerCertificateChain();
}

Ssl::PeerVerifyMode SslSocket::peerVerifyMode() const
{
    NG_D(const SslSocket);
    return d->peerVerifyMode();
}

string SslSocket::peerVerifyName() const
{
    NG_D(const SslSocket);
    return d->peerVerifyName;
}

PrivateKey SslSocket::privateKey() const
{
    NG_D(const SslSocket);
    return d->config.privateKey();
}

Ssl::SslProtocol SslSocket::sslProtocol() const
{
    NG_D(const SslSocket);
    return d->sslProtocol();
}

SslCipher SslSocket::cipher() const
{
    NG_D(const SslSocket);
    return d->cipher();
}

SslSocket::SslMode SslSocket::mode() const
{
    NG_D(const SslSocket);
    return d->mode();
}

SslConfiguration SslSocket::sslConfiguration() const
{
    NG_D(const SslSocket);
    return d->config;
}

vector<SslError> SslSocket::sslErrors() const
{
    NG_D(const SslSocket);
    return d->errors;
}

void SslSocket::setSslConfiguration(const SslConfiguration &configuration)
{
    NG_D(SslSocket);
    d->config = configuration;
}

void SslSocket::setPeerVerifyName(const string &peerVerifyName)
{
    NG_D(SslSocket);
    d->peerVerifyName = peerVerifyName;
}

void SslSocket::setTlsExtHostName(const string &tlsExtHostName)
{
    NG_D(SslSocket);
    d->tlsExtHostName = tlsExtHostName;
}

shared_ptr<SocketLike> SslSocket::backend() const
{
    NG_D(const SslSocket);
    return d->rawSocket;
}

SslSocket *SslSocket::accept()
{
    NG_D(SslSocket);
    while (true) {
        shared_ptr<SocketLike> rawSocket = d->rawSocket->accept();
        if (rawSocket) {
            unique_ptr<SslSocket> s(new SslSocket(rawSocket, d->config));
            if (s->d_func()->handshake(true, string())) {
                return s.release();
            }
        }
    }
}

Socket *SslSocket::acceptRaw()
{
    NG_D(SslSocket);
    return d->rawSocket->acceptRaw();
}

bool SslSocket::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    NG_D(SslSocket);
    return d->rawSocket->bind(address, port, mode);
}

bool SslSocket::bind(uint16_t port, Socket::BindMode mode)
{
    NG_D(SslSocket);
    return d->rawSocket->bind(port, mode);
}

bool SslSocket::connect(const HostAddress &addr, uint16_t port)
{
    NG_D(SslSocket);
    if (!d->rawSocket->connect(addr, port)) {
        return false;
    }
    return d->handshake(false, string());
}

bool SslSocket::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    NG_D(SslSocket);
    if (!d->rawSocket->connect(hostName, port, dnsCache)) {
        return false;
    }
    return d->handshake(false, hostName);
}

void SslSocket::close()
{
    NG_D(SslSocket);
    d->close();
}

void SslSocket::abort()
{
    NG_D(SslSocket);
    d->abort();
}

bool SslSocket::listen(int backlog)
{
    NG_D(SslSocket);
    return d->rawSocket->listen(backlog);
}

bool SslSocket::setOption(Socket::SocketOption option, int value)
{
    NG_D(SslSocket);
    return d->rawSocket->setOption(option, value);
}

int SslSocket::option(Socket::SocketOption option) const
{
    NG_D(const SslSocket);
    return d->rawSocket->option(option);
}

Socket::SocketError SslSocket::error() const
{
    NG_D(const SslSocket);
    if (d->error) {
        return d->error;
    } else {
        return d->rawSocket->error();
    }
}

string SslSocket::errorString() const
{
    NG_D(const SslSocket);
    if (!d->errorString.empty()) {
        return d->errorString;
    } else {
        return d->rawSocket->errorString();
    }
}

bool SslSocket::isValid() const
{
    NG_D(const SslSocket);
    return d->isValid();
}

HostAddress SslSocket::localAddress() const
{
    NG_D(const SslSocket);
    return d->rawSocket->localAddress();
}

uint16_t SslSocket::localPort() const
{
    NG_D(const SslSocket);
    return d->rawSocket->localPort();
}

HostAddress SslSocket::peerAddress() const
{
    NG_D(const SslSocket);
    return d->rawSocket->peerAddress();
}

string SslSocket::peerName() const
{
    NG_D(const SslSocket);
    return d->rawSocket->peerName();
}

uint16_t SslSocket::peerPort() const
{
    NG_D(const SslSocket);
    return d->rawSocket->peerPort();
}

intptr_t SslSocket::fileno() const
{
    NG_D(const SslSocket);
    return d->rawSocket->fileno();
}

Socket::SocketType SslSocket::type() const
{
    NG_D(const SslSocket);
    return d->rawSocket->type();
}

Socket::SocketState SslSocket::state() const
{
    NG_D(const SslSocket);
    return d->rawSocket->state();
}

HostAddress::NetworkLayerProtocol SslSocket::protocol() const
{
    NG_D(const SslSocket);
    return d->rawSocket->protocol();
}

string SslSocket::localAddressURI() const
{
    NG_D(const SslSocket);
    return "ssl+" + d->rawSocket->localAddressURI();
}

string SslSocket::peerAddressURI() const
{
    NG_D(const SslSocket);
    return "ssl+" + d->rawSocket->peerAddressURI();
}

int32_t SslSocket::peek(char *data, int32_t size)
{
    NG_D(SslSocket);
    return d->peek(data, size);
}

int32_t SslSocket::peekRaw(char *data, int32_t size)
{
    NG_D(SslSocket);
    return d->peekRaw(data, size);
}


int32_t SslSocket::recv(char *data, int32_t size)
{
    NG_D(SslSocket);
    return d->recv(data, size, false);
}

int32_t SslSocket::recvall(char *data, int32_t size)
{
    NG_D(SslSocket);
    return d->recv(data, size, true);
}

int32_t SslSocket::send(const char *data, int32_t size)
{
    NG_D(SslSocket);
    return d->send(data, size, false);
}

int32_t SslSocket::sendall(const char *data, int32_t size)
{
    NG_D(SslSocket);
    return d->send(data, size, true);
}

string SslSocket::recv(int32_t size)
{
    NG_D(SslSocket);
    string bs;
    bs.resize(size);

    int32_t bytes = d->recv(&bs[0], bs.size(), false);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return string();
}

string SslSocket::recvall(int32_t size)
{
    NG_D(SslSocket);
    string bs;
    bs.resize(size);

    int32_t bytes = d->recv(&bs[0], bs.size(), true);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return string();
}

int32_t SslSocket::send(const string &data)
{
    NG_D(SslSocket);
    int32_t bytesSent = d->send(data.data(), data.size(), false);
    if (bytesSent == 0 && !d->isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}

int32_t SslSocket::sendall(const string &data)
{
    NG_D(SslSocket);
    return d->send(data.data(), data.size(), true);
}

SslSocket *SslSocket::createConnection(const HostAddress &host, uint16_t port, const SslConfiguration &config,
                                       Socket::SocketError *error, int allowProtocol)
{
    function<SslSocket *(HostAddress::NetworkLayerProtocol)> func =
            [config](HostAddress::NetworkLayerProtocol protocol) -> SslSocket * {
        return new SslSocket(protocol, config);
    };
    return qtng::createConnection<SslSocket>(host, port, error, allowProtocol, func);
}

SslSocket *SslSocket::createConnection(const string &hostName, uint16_t port, const SslConfiguration &config,
                                       Socket::SocketError *error, shared_ptr<SocketDnsCache> dnsCache,
                                       int allowProtocol)
{
    function<SslSocket *(HostAddress::NetworkLayerProtocol)> func =
            [config](HostAddress::NetworkLayerProtocol protocol) -> SslSocket * {
        return new SslSocket(protocol, config);
    };
    return qtng::createConnection<SslSocket>(hostName, port, error, dnsCache, allowProtocol, func);
}

SslSocket *SslSocket::createServer(const HostAddress &host, uint16_t port, const SslConfiguration &config, int backlog)
{
    function<SslSocket *(HostAddress::NetworkLayerProtocol)> func =
            [config](HostAddress::NetworkLayerProtocol protocol) -> SslSocket * {
        return new SslSocket(protocol, config);
    };
    return qtng::createServer<SslSocket>(host, port, backlog, func);
}

namespace {

class SslSocketLikeImpl : public SocketLike
{
public:
    SslSocketLikeImpl(shared_ptr<SslSocket> s);
public:
    virtual Socket::SocketError error() const override;
    virtual string errorString() const override;
    virtual bool isValid() const override;
    virtual HostAddress localAddress() const override;
    virtual uint16_t localPort() const override;
    virtual HostAddress peerAddress() const override;
    virtual string peerName() const override;
    virtual uint16_t peerPort() const override;
    virtual intptr_t fileno() const override;
    virtual Socket::SocketType type() const override;
    virtual Socket::SocketState state() const override;
    virtual HostAddress::NetworkLayerProtocol protocol() const override;
    virtual string localAddressURI() const override;
    virtual string peerAddressURI() const override;

    virtual Socket *acceptRaw() override;
    virtual shared_ptr<SocketLike> accept() override;
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override;
    virtual bool bind(uint16_t port, Socket::BindMode mode) override;
    virtual bool connect(const HostAddress &addr, uint16_t port) override;
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;
    virtual void close() override;
    virtual void abort() override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, int value) override;
    virtual int option(Socket::SocketOption option) const override;

    virtual int32_t peek(char *data, int32_t size) override;
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t recv(char *data, int32_t size) override;
    virtual int32_t recvall(char *data, int32_t size) override;
    virtual int32_t send(const char *data, int32_t size) override;
    virtual int32_t sendall(const char *data, int32_t size) override;
    virtual string recv(int32_t size) override;
    virtual string recvall(int32_t size) override;
    virtual int32_t send(const string &data) override;
    virtual int32_t sendall(const string &data) override;
public:
    shared_ptr<SslSocket> s;
};

SslSocketLikeImpl::SslSocketLikeImpl(shared_ptr<SslSocket> s)
    : s(s)
{
}

Socket::SocketError SslSocketLikeImpl::error() const
{
    return s->error();
}

string SslSocketLikeImpl::errorString() const
{
    return s->errorString();
}

bool SslSocketLikeImpl::isValid() const
{
    return s->isValid();
}

HostAddress SslSocketLikeImpl::localAddress() const
{
    return s->localAddress();
}

uint16_t SslSocketLikeImpl::localPort() const
{
    return s->localPort();
}

HostAddress SslSocketLikeImpl::peerAddress() const
{
    return s->peerAddress();
}

string SslSocketLikeImpl::peerName() const
{
    return s->peerName();
}

uint16_t SslSocketLikeImpl::peerPort() const
{
    return s->peerPort();
}

intptr_t SslSocketLikeImpl::fileno() const
{
    return s->fileno();
}

Socket::SocketType SslSocketLikeImpl::type() const
{
    return s->type();
}

Socket::SocketState SslSocketLikeImpl::state() const
{
    return s->state();
}

HostAddress::NetworkLayerProtocol SslSocketLikeImpl::protocol() const
{
    return s->protocol();
}

string SslSocketLikeImpl::localAddressURI() const
{
    return s->localAddressURI();
}

string SslSocketLikeImpl::peerAddressURI() const
{
    return s->peerAddressURI();
}

Socket *SslSocketLikeImpl::acceptRaw()
{
    return s->acceptRaw();
}

shared_ptr<SocketLike> SslSocketLikeImpl::accept()
{
    return asSocketLike(s->accept());
}

bool SslSocketLikeImpl::bind(const HostAddress &address, uint16_t port = 0,
                             Socket::BindMode mode = Socket::DefaultForPlatform)
{
    return s->bind(address, port, mode);
}

bool SslSocketLikeImpl::bind(uint16_t port, Socket::BindMode mode)
{
    return s->bind(port, mode);
}

bool SslSocketLikeImpl::connect(const HostAddress &addr, uint16_t port)
{
    return s->connect(addr, port);
}

bool SslSocketLikeImpl::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    return s->connect(hostName, port, dnsCache);
}

void SslSocketLikeImpl::close()
{
    s->close();
}

void SslSocketLikeImpl::abort()
{
    s->abort();
}

bool SslSocketLikeImpl::listen(int backlog)
{
    return s->listen(backlog);
}

bool SslSocketLikeImpl::setOption(Socket::SocketOption option, int value)
{
    return s->setOption(option, value);
}

int SslSocketLikeImpl::option(Socket::SocketOption option) const
{
    return s->option(option);
}

int32_t SslSocketLikeImpl::peek(char *data, int32_t size)
{
    return s->peek(data, size);
}

int32_t SslSocketLikeImpl::peekRaw(char *data, int32_t size)
{
    return s->peekRaw(data, size);
}

int32_t SslSocketLikeImpl::recv(char *data, int32_t size)
{
    return s->recv(data, size);
}

int32_t SslSocketLikeImpl::recvall(char *data, int32_t size)
{
    return s->recvall(data, size);
}

int32_t SslSocketLikeImpl::send(const char *data, int32_t size)
{
    return s->send(data, size);
}

int32_t SslSocketLikeImpl::sendall(const char *data, int32_t size)
{
    return s->sendall(data, size);
}

string SslSocketLikeImpl::recv(int32_t size)
{
    return s->recv(size);
}

string SslSocketLikeImpl::recvall(int32_t size)
{
    return s->recvall(size);
}

int32_t SslSocketLikeImpl::send(const string &data)
{
    return s->send(data);
}

int32_t SslSocketLikeImpl::sendall(const string &data)
{
    return s->sendall(data);
}

}  // anonymous namespace

shared_ptr<SocketLike> asSocketLike(shared_ptr<SslSocket> s)
{
    return shared_ptr<SocketLike>(new SslSocketLikeImpl(s));
}

shared_ptr<SslSocket> convertSocketLikeToSslSocket(shared_ptr<SocketLike> socket)
{
    shared_ptr<SslSocketLikeImpl> impl = dynamic_pointer_cast<SslSocketLikeImpl>(socket);
    if (!impl) {
        return shared_ptr<SslSocket>();
    } else {
        return impl->s;
    }
}

namespace {

class EncryptedSocketLike : public SocketLike
{
public:
    EncryptedSocketLike(shared_ptr<Cipher> cipher, shared_ptr<SocketLike> s);
public:
    virtual Socket::SocketError error() const override;
    virtual string errorString() const override;
    virtual bool isValid() const override;
    virtual HostAddress localAddress() const override;
    virtual uint16_t localPort() const override;
    virtual HostAddress peerAddress() const override;
    virtual string peerName() const override;
    virtual uint16_t peerPort() const override;
    virtual intptr_t fileno() const override;
    virtual Socket::SocketType type() const override;
    virtual Socket::SocketState state() const override;
    virtual HostAddress::NetworkLayerProtocol protocol() const override;
    virtual string localAddressURI() const override;
    virtual string peerAddressURI() const override;

    virtual Socket *acceptRaw() override;
    virtual shared_ptr<SocketLike> accept() override;
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override;
    virtual bool bind(uint16_t port, Socket::BindMode mode) override;
    virtual bool connect(const HostAddress &addr, uint16_t port) override;
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;
    virtual void close() override;
    virtual void abort() override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, int value) override;
    virtual int option(Socket::SocketOption option) const override;

    int32_t recv(char *data, int32_t size, bool all);
    int32_t send(const char *data, int32_t size, bool all);

    virtual int32_t peek(char *data, int32_t size) override;
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t recv(char *data, int32_t size) override;
    virtual int32_t recvall(char *data, int32_t size) override;
    virtual int32_t send(const char *data, int32_t size) override;
    virtual int32_t sendall(const char *data, int32_t size) override;
    virtual string recv(int32_t size) override;
    virtual string recvall(int32_t size) override;
    virtual int32_t send(const string &data) override;
    virtual int32_t sendall(const string &data) override;
public:
    shared_ptr<Cipher> incomingCipher;
    shared_ptr<Cipher> outgoingCipher;
    shared_ptr<SocketLike> s;
};

EncryptedSocketLike::EncryptedSocketLike(shared_ptr<Cipher> cipher, shared_ptr<SocketLike> s)
    : incomingCipher(cipher->copy(Cipher::Decrypt))
    , outgoingCipher(cipher->copy(Cipher::Encrypt))
    , s(s)
{
}

Socket::SocketError EncryptedSocketLike::error() const
{
    return s->error();
}

string EncryptedSocketLike::errorString() const
{
    return s->errorString();
}

bool EncryptedSocketLike::isValid() const
{
    return s->isValid();
}

HostAddress EncryptedSocketLike::localAddress() const
{
    return s->localAddress();
}

uint16_t EncryptedSocketLike::localPort() const
{
    return s->localPort();
}

HostAddress EncryptedSocketLike::peerAddress() const
{
    return s->peerAddress();
}

string EncryptedSocketLike::peerName() const
{
    return s->peerName();
}

uint16_t EncryptedSocketLike::peerPort() const
{
    return s->peerPort();
}

intptr_t EncryptedSocketLike::fileno() const
{
    return s->fileno();
}

Socket::SocketType EncryptedSocketLike::type() const
{
    return s->type();
}

Socket::SocketState EncryptedSocketLike::state() const
{
    return s->state();
}

HostAddress::NetworkLayerProtocol EncryptedSocketLike::protocol() const
{
    return s->protocol();
}

string EncryptedSocketLike::localAddressURI() const
{
    return "encrypted+" + s->localAddressURI();
}

string EncryptedSocketLike::peerAddressURI() const
{
    return "encrypted+" + s->peerAddressURI();
}

Socket *EncryptedSocketLike::acceptRaw()
{
    return s->acceptRaw();
}

shared_ptr<SocketLike> EncryptedSocketLike::accept()
{
    return s->accept();
}

bool EncryptedSocketLike::bind(const HostAddress &address, uint16_t port = 0,
                               Socket::BindMode mode = Socket::DefaultForPlatform)
{
    return s->bind(address, port, mode);
}

bool EncryptedSocketLike::bind(uint16_t port, Socket::BindMode mode)
{
    return s->bind(port, mode);
}

bool EncryptedSocketLike::connect(const HostAddress &addr, uint16_t port)
{
    return s->connect(addr, port);
}

bool EncryptedSocketLike::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    return s->connect(hostName, port, dnsCache);
}

void EncryptedSocketLike::close()
{
    s->close();
}

void EncryptedSocketLike::abort()
{
    s->abort();
}

bool EncryptedSocketLike::listen(int backlog)
{
    return s->listen(backlog);
}

bool EncryptedSocketLike::setOption(Socket::SocketOption option, int value)
{
    return s->setOption(option, value);
}

int EncryptedSocketLike::option(Socket::SocketOption option) const
{
    return s->option(option);
}

int32_t EncryptedSocketLike::recv(char *data, int32_t size, bool all)
{
    string buf(size, '\0');
    string decrypted;
    decrypted.reserve(size);

    int32_t bs;
    if (all) {
        bs = s->recvall(&buf[0], buf.size());
    } else {
        bs = s->recv(&buf[0], buf.size());
    }

    if (bs <= 0) {
        return bs;
    } else {
        decrypted = incomingCipher->addData(buf.data(), bs);
        if (decrypted.size() != bs) {
            ngWarning() << "EncryptedSocketLike can not decrypt data: expected" << bs << "bytes, got"
                         << decrypted.size() << "bytes";
            return -1;
        }
    }

    assert(decrypted.size() <= size);
    memcpy(data, decrypted.data(), static_cast<size_t>(decrypted.size()));
    return decrypted.size();
}

int32_t EncryptedSocketLike::send(const char *data, int32_t size, bool)
{
    if (size <= 0) {
        return -1;
    }
    const string &encrypted = outgoingCipher->addData(data, size);
    int32_t bs = s->sendall(encrypted);  // only support sendall.
    if (bs < encrypted.size()) {
        return -1;
    } else {
        return size;
    }
}

int32_t EncryptedSocketLike::peek(char *data, int32_t size)
{
    return s->peek(data, size);
}

int32_t EncryptedSocketLike::peekRaw(char *data, int32_t size)
{
    return s->peekRaw(data, size);
}

int32_t EncryptedSocketLike::recv(char *data, int32_t size)
{
    return recv(data, size, false);
}

int32_t EncryptedSocketLike::recvall(char *data, int32_t size)
{
    return recv(data, size, true);
}

int32_t EncryptedSocketLike::send(const char *data, int32_t size)
{
    return send(data, size, false);
}

int32_t EncryptedSocketLike::sendall(const char *data, int32_t size)
{
    return send(data, size, true);
}

string EncryptedSocketLike::recv(int32_t size)
{
    string buf(size, '\0');
    int32_t bs = recv(&buf[0], buf.size(), false);
    if (bs <= 0) {
        return string();
    } else if (bs == size) {
        return buf;
    } else {
        buf.resize(bs);
        return buf;
    }
}

string EncryptedSocketLike::recvall(int32_t size)
{
    string buf(size, '\0');
    int32_t bs = recv(&buf[0], buf.size(), true);
    if (bs <= 0) {
        return string();
    } else if (bs == size) {
        return buf;
    } else {
        buf.resize(bs);
        return buf;
    }
}

int32_t EncryptedSocketLike::send(const string &data)
{
    return send(data.data(), data.size(), false);
}

int32_t EncryptedSocketLike::sendall(const string &data)
{
    return send(data.data(), data.size(), true);
}

}  // anonymous namespace

shared_ptr<SocketLike> encrypted(shared_ptr<Cipher> cipher, shared_ptr<SocketLike> socket)
{
    if (!cipher || !cipher->isValid() || !cipher->isStream()) {
        return shared_ptr<SocketLike>();
    }
    return shared_ptr<SocketLike>(new EncryptedSocketLike(cipher, socket));
}

}  // namespace qtng
