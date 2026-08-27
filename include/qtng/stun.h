#ifndef QTNG_STUN_H
#define QTNG_STUN_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/hostaddress.h"
#include "qtng/utils/platform.h"

namespace qtng {

// Result of a STUN Binding query (RFC 8489). Contains the reflexive
// (server-reflexive) address as seen by the STUN server plus round-trip time.
struct StunClientInfo
{
    StunClientInfo();

    bool ok() const { return m_ok; }
    void setOk(bool ok) { m_ok = ok; }

    HostAddress mappedAddress() const { return m_mappedAddress; }
    void setMappedAddress(const HostAddress &addr) { m_mappedAddress = addr; }
    std::uint16_t mappedPort() const { return m_mappedPort; }
    void setMappedPort(std::uint16_t port) { m_mappedPort = port; }

    std::string software() const { return m_software; }
    void setSoftware(const std::string &software) { m_software = software; }

    float rtt() const { return m_rtt; }
    void setRtt(float rtt) { m_rtt = rtt; }

    std::string errorString() const { return m_errorString; }
    void setErrorString(const std::string &error) { m_errorString = error; }

    // STUN error code from an error response (0 when no error response).
    int errorCode() const { return m_errorCode; }
    void setErrorCode(int code) { m_errorCode = code; }

private:
    bool m_ok;
    HostAddress m_mappedAddress;
    std::uint16_t m_mappedPort;
    std::string m_software;
    float m_rtt;
    std::string m_errorString;
    int m_errorCode;
};

class StunClientPrivate;
// STUN client: sends Binding requests over UDP and reports the reflexive address.
class StunClient
{
public:
    explicit StunClient(HostAddress::NetworkLayerProtocol proto = HostAddress::IPv4Protocol);
    ~StunClient();

    // Bind an ephemeral local UDP socket. Called automatically by query() if needed.
    bool open();
    void close();
    bool isOpen() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;

    // Send a Binding request to server:port. On success the returned info's
    // ok() is true and mappedAddress()/mappedPort() hold the reflexive address.
    StunClientInfo query(const HostAddress &server, std::uint16_t port, float timeoutSecs = 3.0f);
    StunClientInfo query(const std::string &server, std::uint16_t port, float timeoutSecs = 3.0f);

    std::string errorString() const;

private:
    StunClientPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(StunClient)
    NG_DISABLE_COPY_MOVE(StunClient)
};

class StunServerPrivate;
// Minimal STUN server (RFC 8489 Binding method) used for NAT traversal and tests.
class StunServer
{
public:
    explicit StunServer(HostAddress::NetworkLayerProtocol proto = HostAddress::IPv4Protocol);
    ~StunServer();

    bool open(const HostAddress &addr, std::uint16_t port = 3478);
    bool open(std::uint16_t port = 3478);
    void close();
    bool isOpen() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    std::string errorString() const;

private:
    StunServerPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(StunServer)
    NG_DISABLE_COPY_MOVE(StunServer)
};

}  // namespace qtng

#endif  // QTNG_STUN_H
