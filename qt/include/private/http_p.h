#ifndef QTNG_HTTP_P_H
#define QTNG_HTTP_P_H

#include "../http.h"
#include "../locks.h"
#include "../socket.h"
#include "../socket_utils.h"
#include "../coroutine_utils.h"
#include "../http_proxy.h"
#include "../ssl.h"
#include "../websocket.h"

QTNETWORKNG_NAMESPACE_BEGIN

class HttpProxy;
class Socks5Proxy;
class ConnectionPoolItem
{
public:
    ConnectionPoolItem() { }
public:
    QDateTime lastUsed;
    QSharedPointer<Semaphore> semaphore;
    QList<QSharedPointer<SocketLike>> connections;
};

class ConnectionPool
{
public:
    ConnectionPool();
    virtual ~ConnectionPool();
    QSharedPointer<Semaphore> getSemaphore(const QUrl &url);
    void recycle(const QUrl &url, QSharedPointer<SocketLike> connection);
    QSharedPointer<SocketLike> oldConnectionForUrl(const QUrl &url);
    QSharedPointer<SocketLike> newConnectionForUrl(const QUrl &url, RequestError **error);
    void removeUnusedConnections();
    QSharedPointer<SocketProxy> socketProxy() const;
    QSharedPointer<HttpProxy> httpProxy() const;
    void setSocketProxy(QSharedPointer<SocketProxy> proxy);
    void setHttpProxy(QSharedPointer<HttpProxy> proxy);
private:
    QSharedPointer<ConnectionPoolItem> getItem(const QUrl &url);
public:
    QMap<QUrl, QSharedPointer<ConnectionPoolItem>> items;
    QSharedPointer<SocketDnsCache> dnsCache;
    QSharedPointer<BaseProxySwitcher> proxySwitcher;
#ifndef QTNG_NO_CRYPTO
    SslConfiguration sslConfig;
#endif
    int maxConnectionsPerServer;
    int timeToLive;
    float defaultConnectionTimeout;
    float defaultTimeout;
    CoroutineGroup *operations;
};

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_HTTP_P_H
