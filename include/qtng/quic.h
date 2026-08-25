#ifndef QTNG_QUIC_H
#define QTNG_QUIC_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qtng/certificate.h"
#include "qtng/hostaddress.h"
#include "qtng/locks.h"
#include "qtng/pkey.h"
#include "qtng/socket.h"
#include "qtng/utils/platform.h"

namespace qtng {

class DatagramLink;
class DatagramPath;
class QuicStream;
class QuicConnectionPrivate;
class QuicStreamPrivate;
class SocketLike;

class QuicConfiguration
{
public:
    QuicConfiguration();

    void setAlpnProtocols(const std::vector<std::string> &protocols);
    std::vector<std::string> alpnProtocols() const;

    void setIdleTimeout(float seconds);
    float idleTimeout() const;

    void setVerifyPeer(bool verify);
    bool verifyPeer() const;

    void setPrivateKey(const PrivateKey &key);
    PrivateKey privateKey() const;

    void setLocalCertificate(const Certificate &cert);
    Certificate localCertificate() const;

    void setMaxData(std::uint64_t bytes);
    std::uint64_t maxData() const;

    void setMaxStreamData(std::uint64_t bytes);
    std::uint64_t maxStreamData() const;
private:
    std::vector<std::string> m_alpn;
    float m_idleTimeout;
    bool m_verifyPeer;
    PrivateKey m_privateKey;
    Certificate m_localCertificate;
    std::uint64_t m_maxData;
    std::uint64_t m_maxStreamData;
};

class QuicConnection
{
    NG_DISABLE_COPY(QuicConnection)
public:
    enum State {
        UnconnectedState = 0,
        HostLookupState,
        ConnectingState,
        ConnectedState,
        ClosingState,
        BoundState,
        ListeningState,
    };

    enum Error {
        NoError = 0,
        SocketError,
        HandshakeError,
        ProtocolError,
        TimeoutError,
        ClosedError,
        UnsupportedVersion,
        UnknownError = 100,
    };
public:
    explicit QuicConnection(std::shared_ptr<DatagramLink> link);
    explicit QuicConnection(HostAddress::NetworkLayerProtocol protocol = HostAddress::IPv4Protocol);
    ~QuicConnection();

    void setConfiguration(const QuicConfiguration &config);
    QuicConfiguration configuration() const;

    Error error() const;
    std::string errorString() const;
    State state() const;
    bool isValid() const;

    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    HostAddress peerAddress() const;
    std::uint16_t peerPort() const;
    std::string peerName() const;

    bool bind(const HostAddress &address, std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool bind(std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool listen(int backlog = 16);
    std::shared_ptr<QuicConnection> accept();

    // Complete a server-side handshake on an already-bound DatagramLink / UDP socket
    // (used by tests and single-connection servers). Blocks until Connected or failure.
    bool serve();

    bool connect(const HostAddress &addr, std::uint16_t port, const std::string &serverName = std::string());
    bool connect(const std::string &hostName, std::uint16_t port, const std::string &serverName = std::string(),
                 std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>());
    // DatagramLink peers (tests / custom transports): peer path is an opaque DatagramPath key.
    bool connect(const DatagramPath &peer, const std::string &serverName = std::string());

    std::shared_ptr<QuicStream> openStream();
    std::shared_ptr<QuicStream> acceptStream();

    void close();
    void abort();

    // Returns the event fired when the TLS handshake completes.
    std::shared_ptr<Event> handshakeDone() const;
private:
    explicit QuicConnection(QuicConnectionPrivate *d);
    friend class QuicConnectionPrivate;
    friend class QuicStream;
    friend class QuicStreamPrivate;
private:
    QuicConnectionPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(QuicConnection)
};

class QuicStream
{
    NG_DISABLE_COPY(QuicStream)
public:
    ~QuicStream();

    std::uint64_t streamId() const;
    bool isValid() const;
    QuicConnection::Error error() const;
    std::string errorString() const;

    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);

    void close();  // send FIN
    void reset(std::uint64_t applicationErrorCode = 0);
private:
    explicit QuicStream(QuicStreamPrivate *d);
    friend class QuicConnection;
    friend class QuicConnectionPrivate;
    friend class QuicStreamPrivate;
private:
    QuicStreamPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(QuicStream)
};

std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<QuicStream> s);

}  // namespace qtng

#endif  // QTNG_QUIC_H
