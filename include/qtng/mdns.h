#ifndef QTNG_MDNS_H
#define QTNG_MDNS_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/hostaddress.h"
#include "qtng/utils/platform.h"

namespace qtng {

// A service instance discovered via mDNS (RFC 6763).
struct MdnsService
{
    MdnsService()
        : port(0)
    {
    }

    std::string instance;  // e.g. "MyPrinter" (without trailing dot)
    std::string type;      // e.g. "_http._tcp.local"
    std::string host;      // SRV target host, e.g. "host.local"
    std::uint16_t port;    // service port
    std::map<std::string, std::string> txt;  // TXT key/value attributes
    std::vector<HostAddress> addresses;      // A/AAAA records of the target host
};

class MdnsBrowserPrivate;
// mDNS client: browses service instances and resolves .local names (RFC 6762).
class MdnsBrowser
{
public:
    explicit MdnsBrowser(HostAddress::NetworkLayerProtocol proto = HostAddress::IPv4Protocol);
    ~MdnsBrowser();

    // Bind a UDP socket. When port == 5353 the multicast group is joined so
    // LAN-wide discovery works; pass 0 (ephemeral) for directed testing.
    bool open(std::uint16_t port = 5353);
    void close();
    bool isOpen() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;

    // Direct queries to a specific unicast address (e.g. a responder on the
    // same host during tests) instead of the multicast group.
    void setServer(const HostAddress &addr, std::uint16_t port);

    // Discover instances of a service type such as "_http._tcp.local".
    std::vector<MdnsService> browse(const std::string &type, float timeoutSecs = 3.0f);
    // Resolve the target host addresses of a service instance.
    std::vector<HostAddress> resolve(const MdnsService &service, float timeoutSecs = 3.0f);
    // Look up A/AAAA records for a .local host name (e.g. "printer.local").
    std::vector<HostAddress> lookup(const std::string &name, float timeoutSecs = 3.0f);

    std::string errorString() const;

private:
    MdnsBrowserPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(MdnsBrowser)
    NG_DISABLE_COPY_MOVE(MdnsBrowser)
};

class MdnsResponderPrivate;
// mDNS responder: registers local services and host records and answers
// queries with unicast responses (RFC 6762 §6.7 allows unicast replies).
class MdnsResponder
{
public:
    explicit MdnsResponder(HostAddress::NetworkLayerProtocol proto = HostAddress::IPv4Protocol);
    ~MdnsResponder();

    // Bind a UDP socket. When port == 5353 the multicast group is joined.
    bool open(const HostAddress &addr, std::uint16_t port = 5353);
    bool open(std::uint16_t port = 5353);
    void close();
    bool isOpen() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;

    // Default host name used as the SRV target for registered services.
    void setHostName(const std::string &name);
    std::string hostName() const;

    // Register a service instance: e.g. registerService("MyPrinter",
    // "_http._tcp.local", 8080, {{"path", "/print"}}). Returns false if the
    // (instance, type) pair is already registered.
    bool registerService(const std::string &instance, const std::string &type, std::uint16_t port,
                         const std::map<std::string, std::string> &txt = std::map<std::string, std::string>());
    bool unregisterService(const std::string &instance, const std::string &type);

    // Register A/AAAA records for a host name such as "host.local".
    bool registerHost(const std::string &name, const std::vector<HostAddress> &addresses);
    bool unregisterHost(const std::string &name);

    std::string errorString() const;

private:
    MdnsResponderPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(MdnsResponder)
    NG_DISABLE_COPY_MOVE(MdnsResponder)
};

}  // namespace qtng

#endif  // QTNG_MDNS_H
