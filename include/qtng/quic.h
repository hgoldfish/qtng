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

// A resumable TLS session ticket for QUIC 0-RTT (RFC 8446 PSK + RFC 9001 §8).
// Obtained from QuicConnection::takeSessionTicket() after a successful handshake
// and passed back to QuicConnection::setSessionTicket() before a later connect().
struct QuicSessionTicket {
    std::string ticket;
    std::string ticketNonce;
    std::string resumptionSecret;
    bool isValid() const
    {
        return !ticket.empty() && !resumptionSecret.empty();
    }
};

// Pluggable congestion control (RFC 9002 §7). Implementations drive the send
// path: a packet may only be sent while canSend() holds for the current number
// of bytes in flight.
class QuicCongestionControl
{
public:
    virtual ~QuicCongestionControl() = default;

    virtual std::size_t congestionWindow() const = 0;
    virtual bool canSend(std::size_t bytesInFlight) const = 0;
    virtual void onPacketSent(std::size_t bytesInFlight, std::size_t packetSize) = 0;
    virtual void onAckReceived(std::size_t bytesAcked, std::size_t bytesInFlight) = 0;
    virtual void onLossDetected(std::size_t lostBytes, std::size_t bytesInFlight) = 0;
    virtual void onPersistentCongestion() = 0;
};

class QuicRenoCongestionControlPrivate;
// RFC 9002 §7.2 Reno / NewReno with slow start and congestion avoidance.
class QuicRenoCongestionControl : public QuicCongestionControl
{
    NG_DISABLE_COPY(QuicRenoCongestionControl)
public:
    explicit QuicRenoCongestionControl(std::size_t maxDatagramSize = 1200);
    ~QuicRenoCongestionControl() override;

    std::size_t congestionWindow() const override;
    bool canSend(std::size_t bytesInFlight) const override;
    void onPacketSent(std::size_t bytesInFlight, std::size_t packetSize) override;
    void onAckReceived(std::size_t bytesAcked, std::size_t bytesInFlight) override;
    void onLossDetected(std::size_t lostBytes, std::size_t bytesInFlight) override;
    void onPersistentCongestion() override;
private:
    QuicRenoCongestionControlPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(QuicRenoCongestionControl)
};

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

    // Pluggable congestion control. Defaults to QuicRenoCongestionControl.
    void setCongestionController(std::shared_ptr<QuicCongestionControl> cc);
    std::shared_ptr<QuicCongestionControl> congestionController() const;

    // When enabled, the server sends a RETRY packet before the first Initial to
    // validate the client's address (RFC 9000 §8.1). Off by default.
    void setRequireAddressValidation(bool require);
    bool requireAddressValidation() const;
private:
    std::vector<std::string> m_alpn;
    float m_idleTimeout;
    bool m_verifyPeer;
    PrivateKey m_privateKey;
    Certificate m_localCertificate;
    std::uint64_t m_maxData;
    std::uint64_t m_maxStreamData;
    std::shared_ptr<QuicCongestionControl> m_congestionController;
    bool m_requireAddressValidation;
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
    bool isClientSide() const;

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
    std::shared_ptr<QuicStream> openUniStream();
    std::shared_ptr<QuicStream> acceptStream();

    void close();
    void abort();

    // Returns the event fired when the TLS handshake completes.
    std::shared_ptr<Event> handshakeDone() const;

    // Session resumption / 0-RTT. Call takeSessionTicket() after a handshake to
    // persist the ticket, then setSessionTicket() before the next connect() to
    // enable 0-RTT data on that connection.
    QuicSessionTicket takeSessionTicket() const;
    bool setSessionTicket(const QuicSessionTicket &ticket);
    // True once 0-RTT data was accepted by the server (client side, after handshake).
    bool earlyDataAccepted() const;
    // Request a key update (RFC 9001 §6); the key phase flips on the next packets.
    void updateKeys();
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
