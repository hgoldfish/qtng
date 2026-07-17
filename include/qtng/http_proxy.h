#ifndef QTNG_HTTP_PROXY_H
#define QTNG_HTTP_PROXY_H

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

#include <utility>
#include "qtng/http_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

class HttpProxyPrivate;
class HttpProxy : public WithHttpHeaders<SocketProxy>
{
public:
    enum Capability {
        TunnelingCapability = 0x001,
        CachingCapability = 0x008,
    };
public:
    HttpProxy();
    HttpProxy(const std::string &hostName, std::uint16_t port = 0, const std::string &user = std::string(),
              const std::string &password = std::string());
    HttpProxy(const HttpProxy &other);
    HttpProxy(HttpProxy &&other)
        : d_ptr(nullptr)
    {
        std::swap(d_ptr, other.d_ptr);
    }
    ~HttpProxy();
public:
    virtual std::shared_ptr<SocketLike> connect(const std::string &remoteHost, std::uint16_t port) override;
    virtual std::shared_ptr<SocketLike> connect(const HostAddress &remoteHost, std::uint16_t port) override;
public:
    std::string hostName() const;
    std::uint16_t port() const;
    std::string user() const;
    std::string password() const;
    void setHostName(const std::string &hostName);
    void setPort(std::uint16_t port);
    void setUser(const std::string &user);
    void setPassword(const std::string &password);
public:
    void swap(HttpProxy &other) { std::swap(d_ptr, other.d_ptr); }
    bool operator!=(const HttpProxy &other) const { return !(*this == other); }
    HttpProxy &operator=(const HttpProxy &other);
    HttpProxy &operator=(HttpProxy &&other);
    bool operator==(const HttpProxy &other) const;
private:
    HttpProxyPrivate *d_ptr;
    NG_DECLARE_PRIVATE(HttpProxy)
};

class Socks5Proxy;
class BaseProxySwitcher
{
public:
    BaseProxySwitcher();
    virtual ~BaseProxySwitcher();
public:
    virtual std::shared_ptr<SocketProxy> selectSocketProxy(const std::string &url) = 0;
    virtual std::shared_ptr<HttpProxy> selectHttpProxy(const std::string &url) = 0;
};

class SimpleProxySwitcher : public BaseProxySwitcher
{
public:
    virtual std::shared_ptr<SocketProxy> selectSocketProxy(const std::string &url) override;
    virtual std::shared_ptr<HttpProxy> selectHttpProxy(const std::string &url) override;
public:
    std::vector<std::shared_ptr<SocketProxy>> socketProxies;
    std::vector<std::shared_ptr<HttpProxy>> httpProxies;
};

void setProxySwitcher(class HttpSession *session, std::shared_ptr<BaseProxySwitcher> switcher);

}  // namespace qtng

#endif  // QTNG_HTTP_PROXY_H
