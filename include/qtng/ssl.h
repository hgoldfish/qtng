#ifndef QTNG_SSL_H
#define QTNG_SSL_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtng/socket.h"
#include "qtng/certificate.h"
#include "qtng/utils/platform.h"

namespace qtng {

class SslCipherPrivate;
class SslCipher
{
public:
    SslCipher();
    explicit SslCipher(const std::string &name);
    SslCipher(const std::string &name, Ssl::SslProtocol protocol);
    SslCipher(const SslCipher &other);
    ~SslCipher();
public:
    std::string authenticationMethod() const;
    std::string encryptionMethod() const;
    bool isNull() const;
    std::string keyExchangeMethod() const;
    std::string name() const;
    Ssl::SslProtocol protocol() const;
    std::string protocolString() const;
    int supportedBits() const;
    int usedBits() const;
public:
    inline bool operator!=(const SslCipher &other) const { return !operator==(other); }
    SslCipher &operator=(SslCipher &&other)
    {
        swap(other);
        return *this;
    }
    SslCipher &operator=(const SslCipher &other);
    void swap(SslCipher &other) { std::swap(d, other.d); }
    bool operator==(const SslCipher &other) const;
private:
    friend class SslCipherPrivate;
    std::unique_ptr<SslCipherPrivate> d;
};

class ChooseTlsExtNameCallback
{
public:
    virtual std::string choose(const std::string &hostName) = 0;
};

class SslConfigurationPrivate;
class SslConfiguration
{
public:
    SslConfiguration();
    SslConfiguration(const SslConfiguration &other);
#ifdef Q_COMPILER_RVALUE_REFS
    SslConfiguration(SslConfiguration &&other);
#endif
    ~SslConfiguration();
public:
    std::vector<std::string> allowedNextProtocols() const;
    std::vector<Certificate> caCertificates() const;
    std::vector<SslCipher> ciphers() const;
    bool isNull() const;
    Certificate localCertificate() const;
    Ssl::PeerVerifyMode peerVerifyMode() const;
    int peerVerifyDepth() const;
    PrivateKey privateKey() const;
    bool onlySecureProtocol() const;
    bool supportCompression() const;
    bool sendTlsExtHostName() const;
    std::shared_ptr<ChooseTlsExtNameCallback> tlsExtHostNameCallback() const;

    void addCaCertificate(const Certificate &certificate);
    void addCaCertificates(const std::vector<Certificate> &certificates);
    void setLocalCertificate(const Certificate &certificate);
    bool setLocalCertificate(const std::string &path, Ssl::EncodingFormat format = Ssl::Pem);
    void setPeerVerifyDepth(int depth);
    void setPeerVerifyMode(Ssl::PeerVerifyMode mode);
    void setPrivateKey(const PrivateKey &key);
    bool setPrivateKey(const std::string &fileName, Ssl::EncodingFormat format = Ssl::Pem,
                       const std::string &passPhrase = std::string());
    void setSslProtocol(Ssl::SslProtocol protocol);
    void setAllowedNextProtocols(const std::vector<std::string> &protocols);
    void setOnlySecureProtocol(bool onlySecureProtocol);
    void setSupportCompression(bool supportCompression);
    void setSendTlsExtHostName(bool sendTlsExtHostName);
    void setTlsExtHostNameCallback(std::shared_ptr<ChooseTlsExtNameCallback> callback);
public:
    static std::vector<SslCipher> supportedCiphers();
    static SslConfiguration testPurpose(const std::string &commonName, const std::string &countryCode,
                                        const std::string &organization);
public:
    inline bool operator!=(const SslConfiguration &other) const { return !operator==(other); }
    SslConfiguration &operator=(SslConfiguration &&other)
    {
        swap(other);
        return *this;
    }
    SslConfiguration &operator=(const SslConfiguration &other);
    void swap(SslConfiguration &other) { std::swap(d, other.d); }
    bool operator==(const SslConfiguration &other) const;
private:
    std::shared_ptr<SslConfigurationPrivate> d;
    friend class SslConfigurationPrivate;
};

class SslErrorPrivate;
class SslError
{
public:
    enum Error {
        NoError,
        UnableToGetIssuerCertificate,
        UnableToDecryptCertificateSignature,
        UnableToDecodeIssuerPublicKey,
        CertificateSignatureFailed,
        CertificateNotYetValid,
        CertificateExpired,
        InvalidNotBeforeField,
        InvalidNotAfterField,
        SelfSignedCertificate,
        SelfSignedCertificateInChain,
        UnableToGetLocalIssuerCertificate,
        UnableToVerifyFirstCertificate,
        CertificateRevoked,
        InvalidCaCertificate,
        PathLengthExceeded,
        InvalidPurpose,
        CertificateUntrusted,
        CertificateRejected,
        SubjectIssuerMismatch,  // hostname mismatch?
        AuthorityIssuerSerialNumberMismatch,
        NoPeerCertificate,
        HostNameMismatch,
        NoSslSupport,
        CertificateBlacklisted,
        UnspecifiedError = -1
    };

    SslError();
    SslError(Error error);
    SslError(Error error, const Certificate &certificate);
    SslError(const SslError &other);
    ~SslError();
public:
    Error error() const;
    std::string errorString() const;
    Certificate certificate() const;
public:
    void swap(SslError &other) { std::swap(d, other.d); }
    SslError &operator=(SslError &&other)
    {
        swap(other);
        return *this;
    }
    SslError &operator=(const SslError &other);
    bool operator==(const SslError &other) const;
    inline bool operator!=(const SslError &other) const { return !(*this == other); }
private:
    std::unique_ptr<SslErrorPrivate> d;
};
uint qHash(const SslError &key, uint seed = 0);
std::ostream &operator<<(std::ostream &debug, const SslError &error);
std::ostream &operator<<(std::ostream &debug, const SslError::Error &error);

class Socket;
class SslSocketPrivate;
class SocketLike;
class SslSocket
{
public:
    enum SslMode {
        UnencryptedMode = 0,
        SslClientMode = 1,
        SslServerMode = 2,
    };
    enum NextProtocolNegotiationStatus {
        NextProtocolNegotiationNone = 0,
        NextProtocolNegotiationNegotiated = 1,
        NextProtocolNegotiationUnsupported = 2,
    };
public:
    SslSocket(HostAddress::NetworkLayerProtocol protocol = HostAddress::IPv4Protocol,
              const SslConfiguration &config = SslConfiguration());
    SslSocket(std::intptr_t socketDescriptor, const SslConfiguration &config = SslConfiguration());
    SslSocket(std::shared_ptr<Socket> rawSocket, const SslConfiguration &config = SslConfiguration());
    SslSocket(std::shared_ptr<SocketLike> rawSocket, const SslConfiguration &config = SslConfiguration());
    virtual ~SslSocket();
public:
    bool handshake(bool asServer, const std::string &hostName = std::string());
    Certificate localCertificate() const;
    std::vector<Certificate> localCertificateChain() const;
    std::string nextNegotiatedProtocol() const;
    NextProtocolNegotiationStatus nextProtocolNegotiationStatus() const;
    SslMode mode() const;
    Certificate peerCertificate() const;
    std::vector<Certificate> peerCertificateChain() const;
    int peerVerifyDepth() const;
    Ssl::PeerVerifyMode peerVerifyMode() const;
    std::string peerVerifyName() const;
    PrivateKey privateKey() const;
    SslCipher cipher() const;
    Ssl::SslProtocol sslProtocol() const;
    SslConfiguration sslConfiguration() const;
    std::vector<SslError> sslErrors() const;
    void setSslConfiguration(const SslConfiguration &configuration);
    void setPeerVerifyName(const std::string &peerVerifyName);
    void setTlsExtHostName(const std::string &tlsExtHostName);
    std::shared_ptr<SocketLike> backend() const;
public:
    Socket::SocketError error() const;
    std::string errorString() const;
    bool isValid() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    HostAddress peerAddress() const;
    std::string peerName() const;
    std::uint16_t peerPort() const;
    virtual std::intptr_t fileno() const;
    Socket::SocketType type() const;
    Socket::SocketState state() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    std::string localAddressURI() const;
    std::string peerAddressURI() const;

    SslSocket *accept();
    Socket *acceptRaw();
    bool bind(const HostAddress &address, std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool bind(std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool connect(const HostAddress &addr, std::uint16_t port);
    bool connect(const std::string &hostName, std::uint16_t port,
                 std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>());
    void close();
    void abort();
    bool listen(int backlog);
    bool setOption(Socket::SocketOption option, int value);
    int option(Socket::SocketOption option) const;

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t peekRaw(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);

    static SslSocket *createConnection(const HostAddress &host, std::uint16_t port,
                                       const SslConfiguration &config = SslConfiguration(),
                                       Socket::SocketError *error = nullptr,
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static SslSocket *createConnection(const std::string &hostName, std::uint16_t port,
                                       const SslConfiguration &config = SslConfiguration(),
                                       Socket::SocketError *error = nullptr,
                                       std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>(),
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static SslSocket *createServer(const HostAddress &host, std::uint16_t port,
                                   const SslConfiguration &config = SslConfiguration(), int backlog = 50);
private:
    SslSocketPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(SslSocket)
    NG_DISABLE_COPY(SslSocket)
};

std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<SslSocket> s);

inline std::shared_ptr<SocketLike> asSocketLike(SslSocket *s)
{
    return asSocketLike(std::shared_ptr<SslSocket>(s));
}

std::shared_ptr<SslSocket> convertSocketLikeToSslSocket(std::shared_ptr<SocketLike> socket);

// XXX we always assume the cipher is stream cipher
std::shared_ptr<SocketLike> encrypted(std::shared_ptr<Cipher> cipher, std::shared_ptr<SocketLike> socket);

}  // namespace qtng
#endif  // QTNG_SSL_H
