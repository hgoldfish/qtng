#ifndef QTNG_SOCKS5PROXY_H
#define QTNG_SOCKS5PROXY_H

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "qtng/socket_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

class Socks5Exception
{
public:
    enum Error {
        ProxyConnectionRefusedError,
        ProxyConnectionClosedError,
        ProxyConnectionTimeoutError,
        ProxyNotFoundError,
        ProxyProtocolError,
        ProxyAuthenticationRequiredError,

        SocksFailure,
        ConnectionNotAllowed,
        NetworkUnreachable,
        HostUnreachable,
        ConnectionRefused,
        TTLExpired,
        CommandNotSupported,
        AddressTypeNotSupported,
    };
public:
    Socks5Exception(Error err)
        : err(err)
    {
    }
public:
    Error error() const;
    std::string errorString() const;
    std::string what() const { return errorString(); }
private:
    Error err;
};

class Socks5ProxyPrivate;
class Socks5Proxy : public SocketProxy
{
public:
    enum Capability {
        TunnelingCapability = 0x0001,
        ListeningCapability = 0x0002,
        UdpTunnelingCapability = 0x0003,
        HostNameLookupCapability = 0x0010,
    };
    using Capabilities = int;
public:
    Socks5Proxy();
    Socks5Proxy(const std::string &hostName, std::uint16_t port, const std::string &user = std::string(),
                const std::string &password = std::string());
    Socks5Proxy(const Socks5Proxy &other);
    Socks5Proxy(Socks5Proxy &&other)
        : d_ptr(nullptr)
    {
        std::swap(d_ptr, other.d_ptr);
    }
    ~Socks5Proxy();
public:
    virtual std::shared_ptr<SocketLike> connect(const std::string &remoteHost, std::uint16_t port) override;
    virtual std::shared_ptr<SocketLike> connect(const HostAddress &remoteHost, std::uint16_t port) override;
    std::shared_ptr<SocketLike> listen(std::uint16_t port);

    bool isNull() const;
    Capabilities capabilities() const;
    std::string hostName() const;
    std::uint16_t port() const;
    std::string user() const;
    std::string password() const;
    void setCapabilities(Capabilities capabilities);
    void setHostName(const std::string &hostName);
    void setPort(std::uint16_t port);
    void setUser(const std::string &user);
    void setPassword(const std::string &password);
public:
    void swap(Socks5Proxy &other) { std::swap(d_ptr, other.d_ptr); }
    bool operator!=(const Socks5Proxy &other) const { return !(*this == other); }
    Socks5Proxy &operator=(const Socks5Proxy &other);
    Socks5Proxy &operator=(Socks5Proxy &&other);
    bool operator==(const Socks5Proxy &other) const;
private:
    Socks5ProxyPrivate *d_ptr;
    NG_DECLARE_PRIVATE(Socks5Proxy)
};

}  // namespace qtng

#endif  // QTNG_SOCKS5PROXY_H
