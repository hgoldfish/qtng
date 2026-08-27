#ifndef QTNG_TURN_H
#define QTNG_TURN_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "qtng/hostaddress.h"
#include "qtng/utils/platform.h"

namespace qtng {

class TurnClientPrivate;
// TURN client (RFC 8656) over UDP. open() performs Allocate; afterwards the
// client can relay datagrams to arbitrary peers via the TURN server.
class TurnClient
{
public:
    explicit TurnClient(HostAddress::NetworkLayerProtocol proto = HostAddress::IPv4Protocol);
    ~TurnClient();

    // Bind an ephemeral UDP socket and complete Allocate against server:port.
    // Empty username/password means no long-term credentials (server must not
    // require them). Returns false on authentication or network failure.
    bool open(const HostAddress &server, std::uint16_t port, const std::string &username = std::string(),
              const std::string &password = std::string(), float timeoutSecs = 5.0f);
    bool open(const std::string &server, std::uint16_t port, const std::string &username = std::string(),
              const std::string &password = std::string(), float timeoutSecs = 5.0f);
    void close();
    bool isOpen() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;

    // Relayed address/port peers should use to reach this allocation.
    HostAddress relayedAddress() const;
    std::uint16_t relayedPort() const;
    std::uint32_t lifetime() const;

    // Refresh the allocation; returns false if the server rejected or timed out.
    bool refresh(float timeoutSecs = 5.0f);

    // Send to a peer. Automatically creates a permission and binds a channel on
    // first use, then relays via ChannelData frames.
    bool sendTo(const HostAddress &peer, std::uint16_t port, const std::string &data);
    // Explicit Send indication path (no channel binding).
    bool sendIndication(const HostAddress &peer, std::uint16_t port, const std::string &data);
    // Grant the server permission to relay traffic to/from this peer address
    // (CreatePermission). Required before the peer's return traffic is delivered.
    bool permit(const HostAddress &peer, std::uint16_t port, float timeoutSecs = 5.0f);
    // Receive data relayed from any permitted peer. Empty string on timeout;
    // peer/port are filled with the sender's relay-side address.
    std::string recvFrom(HostAddress *peer, std::uint16_t *port, float timeoutSecs = 5.0f);

    std::string errorString() const;
    int errorCode() const;

private:
    TurnClientPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(TurnClient)
    NG_DISABLE_COPY_MOVE(TurnClient)
};

class TurnServerPrivate;
// Minimal TURN server (RFC 8656) with UDP relay allocations, long-term
// credential authentication and per-allocation forwarding coroutines.
class TurnServer
{
public:
    // Returns the password for (username, realm); empty password rejects the user.
    typedef std::function<std::string(const std::string &username, const std::string &realm)> AuthCallback;

    explicit TurnServer(HostAddress::NetworkLayerProtocol proto = HostAddress::IPv4Protocol);
    ~TurnServer();

    // auth == null: no authentication (any request is accepted). With a callback,
    // Allocate/Refresh/CreatePermission/ChannelBind must carry valid long-term
    // credentials; otherwise a 401 challenge with REALM+NONCE is returned.
    bool open(const HostAddress &addr, std::uint16_t port, const std::string &realm = "qtng",
              AuthCallback auth = AuthCallback());
    bool open(std::uint16_t port, const std::string &realm = "qtng", AuthCallback auth = AuthCallback());
    void close();
    bool isOpen() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    std::string errorString() const;
    void setDefaultLifetime(std::uint32_t secs);

private:
    TurnServerPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(TurnServer)
    NG_DISABLE_COPY_MOVE(TurnServer)
};

}  // namespace qtng

#endif  // QTNG_TURN_H
