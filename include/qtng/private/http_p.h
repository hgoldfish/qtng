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
#ifndef QTNG_NO_CRYPTO
#  include "qtng/ssl.h"
#endif
#include "qtng/websocket.h"
#include "qtng/utils/platform.h"
#include "qtng/utils/url.h"

namespace qtng {

class HttpProxy;
class Socks5Proxy;
#ifndef QTNG_NO_HTTP2
class Http2ClientSession;
#endif

class HttpRequestPrivate
{
public:
    HttpRequestPrivate();
    ~HttpRequestPrivate();
    HttpRequestPrivate(const HttpRequestPrivate &other);
public:
    std::shared_ptr<SocketLike> connection;
    std::string method;
    utils::Url url;
    utils::UrlQuery query;
    std::vector<HttpCookie> cookies;
    std::shared_ptr<FileLike> body;
    std::string userAgent;
    std::int64_t maxBodySize;
    int maxRedirects;
    float connectionTimeout;
    float timeout;
    HttpRequest::Priority priority;
    HttpVersion version;
    bool streamResponse;
    bool isWebSocket;
};

namespace detail {

// HttpRequest/HttpResponse are shallow-copyable by design: their copy
// constructors share the private data. HttpDeepCopy provides the only
// sanctioned deep copy, used by the Qt binding to hand an independent object
// to the core (core::HttpSession::send() mutates the request in place). It is
// a friend of both classes; this header is private and never installed.
class HttpDeepCopy
{
public:
    static HttpRequest request(const HttpRequest &req);
    static HttpResponse response(const HttpResponse &resp);
};

}  // namespace detail

class HttpResponsePrivate
{
public:
    HttpResponsePrivate();
    ~HttpResponsePrivate();
    HttpResponsePrivate(const HttpResponsePrivate &other);
public:
    utils::Url url;
    std::string statusText;
    std::vector<HttpCookie> cookies;
    HttpRequest request;
    std::string body;
    std::vector<HttpResponse> history;
    std::shared_ptr<RequestError> error;
    std::shared_ptr<SocketLike> stream;
    std::int64_t elapsed;
    int statusCode;
    HttpVersion version;
    bool consumed;
};

class ConnectionPoolItem
{
public:
    ConnectionPoolItem() { }
public:
    qtng::utils::DateTime lastUsed;
    std::shared_ptr<Semaphore> semaphore;
    std::vector<std::shared_ptr<SocketLike>> connections;
#ifndef QTNG_NO_HTTP2
    std::vector<std::shared_ptr<Http2ClientSession>> http2Sessions;
#endif
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
#ifndef QTNG_NO_HTTP2
    std::shared_ptr<Http2ClientSession> http2SessionForUrl(const std::string &url, RequestError **error);
    void recycleHttp2Session(const std::string &url, std::shared_ptr<Http2ClientSession> session);
#endif
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
    int webSocketErrorCode;
    std::string webSocketErrorReason;
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
