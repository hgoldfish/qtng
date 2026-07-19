#ifndef QTNG_HTTP_P_H
#define QTNG_HTTP_P_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/http.h"
#include "qtng/locks.h"
#include "qtng/socket.h"
#include "qtng/socket_utils.h"
#include "qtng/coroutine_utils.h"
#include "qtng/http_proxy.h"
#include "qtng/ssl.h"
#include "qtng/websocket.h"
#include "qtng/utils/platform.h"

namespace qtng {

class HttpProxy;
class Socks5Proxy;
class ConnectionPoolItem
{
public:
    ConnectionPoolItem() { }
public:
    qtng::utils::DateTime lastUsed;
    std::shared_ptr<Semaphore> semaphore;
    std::vector<std::shared_ptr<SocketLike>> connections;
};

class ConnectionPool
{
public:
    ConnectionPool();
    virtual ~ConnectionPool();
    std::shared_ptr<Semaphore> getSemaphore(const std::string &url);
    void recycle(const std::string &url, std::shared_ptr<SocketLike> connection);
    std::shared_ptr<SocketLike> oldConnectionForUrl(const std::string &url);
    std::shared_ptr<SocketLike> newConnectionForUrl(const std::string &url, RequestError **error);
    void removeUnusedConnections();
    std::shared_ptr<SocketProxy> socketProxy() const;
    std::shared_ptr<HttpProxy> httpProxy() const;
    void setSocketProxy(std::shared_ptr<SocketProxy> proxy);
    void setHttpProxy(std::shared_ptr<HttpProxy> proxy);
private:
    std::shared_ptr<ConnectionPoolItem> getItem(const std::string &url);
public:
    std::map<std::string, std::shared_ptr<ConnectionPoolItem>> items;
    std::shared_ptr<SocketDnsCache> dnsCache;
    std::shared_ptr<BaseProxySwitcher> proxySwitcher;
#ifndef QTNG_NO_CRYPTO
    SslConfiguration sslConfig;
#endif
    int maxConnectionsPerServer;
    int timeToLive;
    float defaultConnectionTimeout;
    float defaultTimeout;
    CoroutineGroup *operations;
};

class HttpSessionPrivate : public ConnectionPool
{
public:
    HttpSessionPrivate(HttpSession *q_ptr);
    virtual ~HttpSessionPrivate();
    std::vector<HttpHeader> makeHeaders(HttpRequest &request, const std::string &url) const;
    void mergeCookies(HttpRequest &request, const std::string &url);
    HttpResponse send(HttpRequest &req);
    void prepareWebSocketRequest(HttpRequest &request, std::string &secKey);
    std::shared_ptr<WebSocketConnection> makeWebSocketConnection(HttpResponse &response, const std::string &secKey);
public:
    HttpCookieJar cookieJar;
    WebSocketConfiguration webSocketConfiguration;
    std::shared_ptr<HttpCacheManager> cacheManager;
    std::string defaultUserAgent;
    HttpVersion defaultVersion;
    HttpSession *q_ptr;
    int debugLevel;
    bool managingCookies;
    bool keepAlive;
    friend void setProxySwitcher(HttpSession *session, std::shared_ptr<BaseProxySwitcher> switcher);
    static inline HttpSessionPrivate *getPrivateHelper(HttpSession *session) { return session->d_ptr; }
    NG_DECLARE_PUBLIC(HttpSession)
};

}  // namespace qtng

#endif  // QTNG_HTTP_P_H
