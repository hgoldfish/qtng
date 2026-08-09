#include <QtCore/qjsondocument.h>
#include <QtCore/qurlquery.h>

#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "http.h"
#include "private/http_p.h"
#include "websocket.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

namespace {

qtng_core::HttpHeader toCoreHeader(const HttpHeader &h)
{
    return qtng_core::HttpHeader(toStdString(h.name), toStdString(h.value));
}

HttpHeader toQtHeader(const qtng_core::HttpHeader &h)
{
    return HttpHeader(toQString(h.name), toQByteArray(h.value));
}

void syncHeadersToCore(const HttpHeaderManager &qt, qtng_core::HttpHeaderManager &core)
{
    vector<qtng_core::HttpHeader> headers;
    headers.reserve(static_cast<size_t>(qt.allHeaders().size()));
    for (const HttpHeader &h : qt.allHeaders()) {
        headers.push_back(toCoreHeader(h));
    }
    core.setHeaders(headers);
}

void syncHeadersFromCore(const qtng_core::HttpHeaderManager &core, HttpHeaderManager &qt)
{
    QList<HttpHeader> headers;
    for (const qtng_core::HttpHeader &h : core.allHeaders()) {
        headers.append(toQtHeader(h));
    }
    qt.setHeaders(headers);
}

qtng_core::HttpCookie toCoreCookie(const HttpCookie &c)
{
    qtng_core::HttpCookie cc(toStdString(c.name()), toStdString(c.value()));
    cc.setDomain(toStdString(c.domain()));
    cc.setPath(toStdString(c.path()));
    cc.setSecure(c.isSecure());
    cc.setHttpOnly(c.isHttpOnly());
    return cc;
}

HttpCookie toQtCookie(const qtng_core::HttpCookie &c)
{
    HttpCookie qc(c.name().data(), c.value().data());
    qc.setDomain(toQString(c.domain()));
    qc.setPath(toQString(c.path()));
    qc.setSecure(c.isSecure());
    qc.setHttpOnly(c.isHttpOnly());
    return qc;
}

qtng_core::HttpRequest toCoreRequest(const HttpRequest &req)
{
    qtng_core::HttpRequest core;
    core.setMethod(toStdString(req.method()));
    core.setUrl(toCoreUrl(req.url()));
    core.setQuery(toCoreUrlQuery(req.query()));
    vector<qtng_core::HttpCookie> cookies;
    for (const HttpCookie &c : req.cookies()) {
        cookies.push_back(toCoreCookie(c));
    }
    core.setCookies(cookies);
    if (QSharedPointer<FileLike> body = req.body()) {
        core.setBody(toCoreFileLike(body));
    }
    core.setUserAgent(toStdString(req.userAgent()));
    core.setMaxBodySize(req.maxBodySize());
    core.setMaxRedirects(req.maxRedirects());
    core.setPriority(static_cast<qtng_core::HttpRequest::Priority>(req.priority()));
    core.setVersion(static_cast<qtng_core::HttpVersion>(req.version()));
    core.setStreamResponse(req.streamResponse());
    core.setConnectionTimeout(req.connectionTimeout());
    core.setTimeout(req.timeout());
    if (QSharedPointer<SocketLike> conn = req.connection()) {
        core.useConnection(toCoreSocketLike(conn));
    }
    syncHeadersToCore(req, core);
    return core;
}

HttpRequest toQtRequest(const qtng_core::HttpRequest &core)
{
    HttpRequest req;
    req.setMethod(toQString(core.method()));
    req.setUrl(toQUrl(core.url()));
    req.setQuery(toQUrlQuery(core.query()));
    QList<HttpCookie> cookies;
    for (const qtng_core::HttpCookie &c : core.cookies()) {
        cookies.append(toQtCookie(c));
    }
    req.setCookies(cookies);
    req.setUserAgent(toQString(core.userAgent()));
    req.setMaxBodySize(core.maxBodySize());
    req.setMaxRedirects(core.maxRedirects());
    req.setPriority(static_cast<HttpRequest::Priority>(core.priority()));
    req.setVersion(static_cast<HttpVersion>(core.version()));
    req.setStreamResponse(core.streamResponse());
    req.setConnectionTimeout(core.connectionTimeout());
    req.setTimeout(core.timeout());
    syncHeadersFromCore(core, req);
    return req;
}

HttpResponse toQtResponse(const qtng_core::HttpResponse &core)
{
    HttpResponse resp;
    resp.setUrl(toQUrl(core.url()));
    resp.setStatusCode(core.statusCode());
    resp.setStatusText(toQString(core.statusText()));
    QList<HttpCookie> cookies;
    for (const qtng_core::HttpCookie &c : core.cookies()) {
        cookies.append(toQtCookie(c));
    }
    resp.setCookies(cookies);
    resp.setRequest(toQtRequest(core.request()));
    resp.setElapsed(core.elapsed());
    resp.setVersion(static_cast<HttpVersion>(core.version()));
    syncHeadersFromCore(core, resp);
    return resp;
}

map<string, string> toCoreStringMap(const QMap<QString, QString> &m)
{
    map<string, string> result;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        result.emplace(toStdString(it.key()), toStdString(it.value()));
    }
    return result;
}

map<string, string> toCoreHeaderMap(const QMap<QString, QByteArray> &m)
{
    map<string, string> result;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        result.emplace(toStdString(it.key()), toStdString(it.value()));
    }
    return result;
}

}  // namespace

class HttpRequestPrivate : public QSharedData
{
public:
    qtng_core::HttpRequest core;
};

FormData::FormData()
{
    const QString possibleCharacters = QString::fromLatin1("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    QString randomPart;
    for (int i = 0; i < 16; ++i) {
        randomPart.append(possibleCharacters.at(qrand() % possibleCharacters.size()));
    }
    boundary = QByteArray("----WebKitFormBoundary") + randomPart.toLatin1();
}

QByteArray FormData::toByteArray() const
{
    qtng_core::FormData core;
    core.boundary = toStdString(boundary);
    for (const Query &q : queries) {
        core.addQuery(toStdString(q.name), toStdString(q.value));
    }
    for (const File &f : files) {
        core.addFile(toStdString(f.name), toStdString(f.filename), toStdString(f.data), toStdString(f.contentType));
    }
    return toQByteArray(core.toByteArray());
}

HttpRequest::HttpRequest()
    : d(new HttpRequestPrivate)
{
}

HttpRequest::~HttpRequest() { }

HttpRequest::HttpRequest(const HttpRequest &other)
    : HttpHeaderManager(other)
    , d(other.d)
{
}

HttpRequest::HttpRequest(HttpRequest &&other)
    : HttpHeaderManager(std::move(other))
    , d(std::move(other.d))
{
}

HttpRequest &HttpRequest::operator=(const HttpRequest &other)
{
    HttpHeaderManager::operator=(other);
    d = other.d;
    return *this;
}

QString HttpRequest::method() const { return toQString(d->core.method()); }
void HttpRequest::setMethod(const QString &method) { d->core.setMethod(toStdString(method)); }
QUrl HttpRequest::url() const { return toQUrl(d->core.url()); }
void HttpRequest::setUrl(const QUrl &url) { d->core.setUrl(toCoreUrl(url)); }
QUrlQuery HttpRequest::query() const { return toQUrlQuery(d->core.query()); }
void HttpRequest::setQuery(const QMap<QString, QString> &query) { d->core.setQuery(toCoreStringMap(query)); }
void HttpRequest::setQuery(const QUrlQuery &query) { d->core.setQuery(toCoreUrlQuery(query)); }
QList<HttpCookie> HttpRequest::cookies() const
{
    QList<HttpCookie> result;
    for (const qtng_core::HttpCookie &c : d->core.cookies()) {
        result.append(toQtCookie(c));
    }
    return result;
}
void HttpRequest::setCookies(const QList<HttpCookie> &cookies)
{
    vector<qtng_core::HttpCookie> coreCookies;
    for (const HttpCookie &c : cookies) {
        coreCookies.push_back(toCoreCookie(c));
    }
    d->core.setCookies(coreCookies);
}
QSharedPointer<FileLike> HttpRequest::body() const { return toQtFileLike(d->core.body()); }
void HttpRequest::setBody(const QByteArray &body) { d->core.setBody(toStdString(body)); }
void HttpRequest::setBody(QSharedPointer<FileLike> body) { d->core.setBody(toCoreFileLike(body)); }
QString HttpRequest::userAgent() const { return toQString(d->core.userAgent()); }
void HttpRequest::setUserAgent(const QString &userAgent) { d->core.setUserAgent(toStdString(userAgent)); }
qint64 HttpRequest::maxBodySize() const { return d->core.maxBodySize(); }
void HttpRequest::setMaxBodySize(qint64 maxBodySize) { d->core.setMaxBodySize(maxBodySize); }
int HttpRequest::maxRedirects() const { return d->core.maxRedirects(); }
void HttpRequest::setMaxRedirects(int maxRedirects) { d->core.setMaxRedirects(maxRedirects); }
HttpRequest::Priority HttpRequest::priority() const { return static_cast<Priority>(d->core.priority()); }
void HttpRequest::setPriority(Priority priority) { d->core.setPriority(static_cast<qtng_core::HttpRequest::Priority>(priority)); }
HttpVersion HttpRequest::version() const { return static_cast<HttpVersion>(d->core.version()); }
void HttpRequest::setVersion(HttpVersion version) { d->core.setVersion(static_cast<qtng_core::HttpVersion>(version)); }
void HttpRequest::setStreamResponse(bool streamResponse) { d->core.setStreamResponse(streamResponse); }
bool HttpRequest::streamResponse() const { return d->core.streamResponse(); }
float HttpRequest::connectionTimeout() const { return d->core.connectionTimeout(); }
void HttpRequest::setConnectionTimeout(float connectionTimeout) { d->core.setConnectionTimeout(connectionTimeout); }
float HttpRequest::timeout() const { return d->core.timeout(); }
void HttpRequest::setTimeout(float timeout) { d->core.setTimeout(timeout); }
QSharedPointer<SocketLike> HttpRequest::connection() const { return toQtSocketLike(d->core.connection()); }
void HttpRequest::useConnection(QSharedPointer<SocketLike> connection) { d->core.useConnection(toCoreSocketLike(connection)); }

void HttpRequest::setBody(const FormData &formData) { setBody(formData.toByteArray()); }
void HttpRequest::setBody(const QJsonDocument &json) { setBody(json.toJson(QJsonDocument::Compact)); }
void HttpRequest::setBody(const QJsonObject &json) { setBody(QJsonDocument(json)); }
void HttpRequest::setBody(const QJsonArray &json) { setBody(QJsonDocument(json)); }
void HttpRequest::setBody(const QMap<QString, QString> form) { d->core.setBody(toCoreStringMap(form)); }
void HttpRequest::setBody(const QUrlQuery &form) { d->core.setBody(toCoreUrlQuery(form)); }

RequestError::~RequestError() { }
QString RequestError::what() const { return QString(); }

class HttpResponsePrivate : public QSharedData
{
public:
    qtng_core::HttpResponse core;
};

HttpResponse::HttpResponse()
    : d(new HttpResponsePrivate)
{
}

HttpResponse::~HttpResponse() { }
HttpResponse::HttpResponse(const HttpResponse &other)
    : HttpHeaderManager(other)
    , d(other.d)
{
}

HttpResponse::HttpResponse(HttpResponse &&other)
    : HttpHeaderManager(std::move(other))
    , d(std::move(other.d))
{
}

HttpResponse &HttpResponse::operator=(const HttpResponse &other)
{
    HttpHeaderManager::operator=(other);
    d = other.d;
    return *this;
}

QUrl HttpResponse::url() const { return toQUrl(d->core.url()); }
void HttpResponse::setUrl(const QUrl &url) { d->core.setUrl(toCoreUrl(url)); }
int HttpResponse::statusCode() const { return d->core.statusCode(); }
void HttpResponse::setStatusCode(int statusCode) { d->core.setStatusCode(statusCode); }
QString HttpResponse::statusText() const { return toQString(d->core.statusText()); }
void HttpResponse::setStatusText(const QString &statusText) { d->core.setStatusText(toStdString(statusText)); }
QList<HttpCookie> HttpResponse::cookies() const
{
    QList<HttpCookie> result;
    for (const qtng_core::HttpCookie &c : d->core.cookies()) {
        result.append(toQtCookie(c));
    }
    return result;
}
void HttpResponse::setCookies(const QList<HttpCookie> &cookies)
{
    vector<qtng_core::HttpCookie> coreCookies;
    for (const HttpCookie &c : cookies) {
        coreCookies.push_back(toCoreCookie(c));
    }
    d->core.setCookies(coreCookies);
}
HttpRequest HttpResponse::request() const { return toQtRequest(d->core.request()); }
void HttpResponse::setRequest(const HttpRequest &request) { d->core.setRequest(toCoreRequest(request)); }
qint64 HttpResponse::elapsed() const { return d->core.elapsed(); }
void HttpResponse::setElapsed(qint64 elapsed) { d->core.setElapsed(elapsed); }
QList<HttpResponse> HttpResponse::history() const
{
    QList<HttpResponse> result;
    for (const qtng_core::HttpResponse &h : d->core.history()) {
        result.append(toQtResponse(h));
    }
    return result;
}
void HttpResponse::setHistory(const QList<HttpResponse> &history)
{
    vector<qtng_core::HttpResponse> coreHistory;
    for (const HttpResponse &h : history) {
        qtng_core::HttpResponse item;
        item.setStatusCode(h.statusCode());
        item.setUrl(toCoreUrl(h.url()));
        coreHistory.push_back(item);
    }
    d->core.setHistory(coreHistory);
}
HttpVersion HttpResponse::version() const { return static_cast<HttpVersion>(d->core.version()); }
void HttpResponse::setVersion(HttpVersion version) { d->core.setVersion(static_cast<qtng_core::HttpVersion>(version)); }

QSharedPointer<SocketLike> HttpResponse::takeStream(QByteArray *readBytes)
{
    string bytes;
    QSharedPointer<SocketLike> stream = toQtSocketLike(d->core.takeStream(readBytes ? &bytes : nullptr));
    if (readBytes) {
        *readBytes = toQByteArray(bytes);
    }
    return stream;
}

QSharedPointer<FileLike> HttpResponse::bodyAsFile(bool processGzip, bool processChunked)
{
    return toQtFileLike(d->core.bodyAsFile(processGzip, processChunked));
}

QByteArray HttpResponse::body() { return toQByteArray(d->core.body()); }
void HttpResponse::setBody(const QByteArray &body) { d->core.setBody(toStdString(body)); }
QString HttpResponse::text() { return toQString(d->core.text()); }
QJsonDocument HttpResponse::json() { return QJsonDocument::fromJson(body()); }
QString HttpResponse::html() { return toQString(d->core.html()); }
bool HttpResponse::isOk() const { return d->core.isOk(); }
bool HttpResponse::hasNetworkError() const { return d->core.hasNetworkError(); }
bool HttpResponse::hasHttpError() const { return d->core.hasHttpError(); }
QSharedPointer<RequestError> HttpResponse::error() const { return QSharedPointer<RequestError>(); }
void HttpResponse::setError(QSharedPointer<RequestError> error) { Q_UNUSED(error); }

namespace {

QUrl hostOnly(const QUrl &url)
{
    QUrl h;
    h.setScheme(url.scheme());
    h.setHost(url.host());
    h.setPort(url.port());
    return h;
}

quint16 urlDefaultPort(const QUrl &url)
{
    if (url.port() > 0) {
        return static_cast<quint16>(url.port());
    }
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("https") || scheme == QStringLiteral("wss")) {
        return 443;
    }
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("ws")) {
        return 80;
    }
    return 0;
}

}  // namespace

ConnectionPool::ConnectionPool()
    : dnsCache(new SocketDnsCache())
    , proxySwitcher(QSharedPointer<BaseProxySwitcher>(new SimpleProxySwitcher()))
    , maxConnectionsPerServer(5)
    , timeToLive(60)
    , defaultConnectionTimeout(10.0f)
    , defaultTimeout(20.0f)
    , operations(new CoroutineGroup())
{
    operations->spawnWithName(QStringLiteral("removeUnusedConnections"), [this] { removeUnusedConnections(); });
}

ConnectionPool::~ConnectionPool()
{
    delete operations;
}

QSharedPointer<ConnectionPoolItem> ConnectionPool::getItem(const QUrl &url)
{
    const QUrl host = hostOnly(url);
    QSharedPointer<ConnectionPoolItem> &item = items[host];
    if (item.isNull()) {
        item = QSharedPointer<ConnectionPoolItem>(new ConnectionPoolItem());
    }
    item->lastUsed = QDateTime::currentDateTimeUtc();
    if (item->semaphore.isNull()) {
        item->semaphore = QSharedPointer<Semaphore>(new Semaphore(maxConnectionsPerServer));
    }
    return item;
}

QSharedPointer<Semaphore> ConnectionPool::getSemaphore(const QUrl &url)
{
    return getItem(url)->semaphore;
}

void ConnectionPool::recycle(const QUrl &url, QSharedPointer<SocketLike> connection)
{
    QSharedPointer<ConnectionPoolItem> item = getItem(url);
    if (item->connections.size() < maxConnectionsPerServer) {
        item->connections.append(connection);
    }
}

QSharedPointer<SocketLike> ConnectionPool::oldConnectionForUrl(const QUrl &url)
{
    QSharedPointer<ConnectionPoolItem> item = getItem(url);
    while (!item->connections.isEmpty()) {
        QSharedPointer<SocketLike> connection = item->connections.takeFirst();
        if (!connection || !connection->isValid()) {
            continue;
        }
        char tbuf;
        if (connection->peekRaw(&tbuf, 1) >= 0) {
            return connection;
        }
    }
    return QSharedPointer<SocketLike>();
}

QSharedPointer<SocketLike> ConnectionPool::newConnectionForUrl(const QUrl &url, RequestError **error)
{
    const quint16 port = urlDefaultPort(url);
    QSharedPointer<SocketLike> connection;
    QSharedPointer<SocketProxy> socketProxy = proxySwitcher->selectSocketProxy(url);
    if (socketProxy) {
        connection = socketProxy->connect(url.host(), port);
        if (!connection || !connection->isValid()) {
            *error = new ConnectionError();
            return QSharedPointer<SocketLike>();
        }
    } else {
        QSharedPointer<Socket> rawSocket;
        rawSocket.reset(Socket::createConnection(url.host(), port, nullptr, dnsCache));
        if (!rawSocket || !rawSocket->isValid()) {
            *error = new ConnectionError();
            return QSharedPointer<SocketLike>();
        }
        connection = asSocketLike(rawSocket);
    }

    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("https") || scheme == QStringLiteral("wss")) {
#ifndef QTNG_NO_CRYPTO
        QSharedPointer<SslSocket> ssl(new SslSocket(connection, sslConfig));
        if (!ssl->handshake(false, url.host())) {
            *error = new ConnectionError();
            return QSharedPointer<SocketLike>();
        }
        connection = asSocketLike(ssl);
#else
        *error = new ConnectionError();
        return QSharedPointer<SocketLike>();
#endif
    }
    return connection;
}

void ConnectionPool::removeUnusedConnections()
{
    while (true) {
        try {
            Coroutine::sleep(1.0);
        } catch (CoroutineException &) {
            return;
        }
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QMap<QUrl, QSharedPointer<ConnectionPoolItem>> newItems;
        for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
            if (it.value()->lastUsed.secsTo(now) < timeToLive || it.value()->semaphore->isUsed()) {
                newItems.insert(it.key(), it.value());
            }
        }
        items = newItems;
    }
}

QSharedPointer<SocketProxy> ConnectionPool::socketProxy() const
{
    QSharedPointer<SimpleProxySwitcher> sps = proxySwitcher.dynamicCast<SimpleProxySwitcher>();
    if (sps && !sps->socketProxies.isEmpty()) {
        return sps->socketProxies.front();
    }
    return QSharedPointer<SocketProxy>();
}

QSharedPointer<HttpProxy> ConnectionPool::httpProxy() const
{
    QSharedPointer<SimpleProxySwitcher> sps = proxySwitcher.dynamicCast<SimpleProxySwitcher>();
    if (sps && !sps->httpProxies.isEmpty()) {
        return sps->httpProxies.at(0);
    }
    return QSharedPointer<HttpProxy>();
}

void ConnectionPool::setSocketProxy(QSharedPointer<SocketProxy> proxy)
{
    QSharedPointer<SimpleProxySwitcher> sps = proxySwitcher.dynamicCast<SimpleProxySwitcher>();
    if (sps) {
        sps->socketProxies.clear();
        if (proxy) {
            sps->socketProxies.append(proxy);
        }
    }
}

void ConnectionPool::setHttpProxy(QSharedPointer<HttpProxy> proxy)
{
    QSharedPointer<SimpleProxySwitcher> sps = proxySwitcher.dynamicCast<SimpleProxySwitcher>();
    if (sps) {
        sps->httpProxies.clear();
        if (proxy) {
            sps->httpProxies.append(proxy);
        }
    }
}

class HttpSessionPrivate : public ConnectionPool
{
public:
    explicit HttpSessionPrivate(HttpSession *q)
        : q_ptr(q)
        , debugLevel(0)
        , managingCookies(true)
        , keepAlive(true)
    {
    }

    qtng_core::HttpSession core;
    HttpCookieJar cookieJar;
    WebSocketConfiguration webSocketConfiguration;
    QSharedPointer<HttpCacheManager> cacheManager;
    HttpSession *q_ptr;
    int debugLevel;
    bool managingCookies;
    bool keepAlive;
};

HttpSession::HttpSession()
    : d_ptr(new HttpSessionPrivate(this))
{
}

HttpSession::~HttpSession()
{
    delete d_ptr;
}

#define HTTP_DELEGATE1(method) \
    HttpResponse HttpSession::method(const QUrl &url) \
    { \
        return toQtResponse(d_ptr->core.method(toCoreUrl(url).toString())); \
    } \
    HttpResponse HttpSession::method(const QString &url) \
    { \
        return method(QUrl::fromUserInput(url)); \
    }

HTTP_DELEGATE1(get)
HTTP_DELEGATE1(head)
HTTP_DELEGATE1(options)
HTTP_DELEGATE1(delete_)

HttpResponse HttpSession::propfind(const QUrl &url)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("PROPFIND"));
    request.setUrl(url);
    return send(request);
}

HttpResponse HttpSession::when(const QUrl &url)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("WHEN"));
    request.setUrl(url);
    return send(request);
}

HttpResponse HttpSession::post(const QUrl &url, const QByteArray &body)
{
    return toQtResponse(d_ptr->core.post(toCoreUrl(url).toString(), toStdString(body)));
}

HttpResponse HttpSession::post(const QString &url, const QByteArray &body)
{
    return post(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::put(const QUrl &url, const QByteArray &body)
{
    return toQtResponse(d_ptr->core.put(toCoreUrl(url).toString(), toStdString(body)));
}

HttpResponse HttpSession::put(const QString &url, const QByteArray &body)
{
    return put(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::patch(const QUrl &url, const QByteArray &body)
{
    return toQtResponse(d_ptr->core.patch(toCoreUrl(url).toString(), toStdString(body)));
}

HttpResponse HttpSession::patch(const QString &url, const QByteArray &body)
{
    return patch(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::brew(const QUrl &url, const QByteArray &body)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("BREW"));
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::send(HttpRequest &request)
{
    qtng_core::HttpRequest core = toCoreRequest(request);
    return toQtResponse(d_ptr->core.send(core));
}

HttpCookieJar &HttpSession::cookieJar()
{
    return d_ptr->cookieJar;
}
HttpCookie HttpSession::cookie(const QUrl &url, const QString &name) { Q_UNUSED(url); Q_UNUSED(name); return HttpCookie(); }
void HttpSession::setManagingCookies(bool managingCookies) { d_ptr->managingCookies = managingCookies; }
void HttpSession::setMaxConnectionsPerServer(int maxConnectionsPerServer) { d_ptr->core.setMaxConnectionsPerServer(maxConnectionsPerServer); }
int HttpSession::maxConnectionsPerServer() { return d_ptr->core.maxConnectionsPerServer(); }
void HttpSession::setDebugLevel(int level) { d_ptr->debugLevel = level; d_ptr->core.setDebugLevel(level); }
void HttpSession::disableDebug() { setDebugLevel(0); }
void HttpSession::setKeepAlive(bool keepAlive) { d_ptr->keepAlive = keepAlive; d_ptr->core.setKeepAlive(keepAlive); }
bool HttpSession::keepAlive() const { return d_ptr->keepAlive; }
QString HttpSession::defaultUserAgent() const { return toQString(d_ptr->core.defaultUserAgent()); }
void HttpSession::setDefaultUserAgent(const QString &userAgent) { d_ptr->core.setDefaultUserAgent(toStdString(userAgent)); }
HttpVersion HttpSession::defaultVersion() const { return static_cast<HttpVersion>(d_ptr->core.defaultVersion()); }
void HttpSession::setDefaultVersion(HttpVersion defaultVersion) { d_ptr->core.setDefaultVersion(static_cast<qtng_core::HttpVersion>(defaultVersion)); }
float HttpSession::defaultConnnectionTimeout() const { return d_ptr->core.defaultConnnectionTimeout(); }
void HttpSession::setDefaultConnectionTimeout(float timeout) { d_ptr->core.setDefaultConnectionTimeout(timeout); }
float HttpSession::defaultTimeout() const { return d_ptr->core.defaultTimeout(); }
void HttpSession::setDefaultTimeout(float defaultTimeout) { d_ptr->core.setDefaultTimeout(defaultTimeout); }
void HttpSession::setDnsCache(QSharedPointer<SocketDnsCache> dnsCache) { Q_UNUSED(dnsCache); }
QSharedPointer<SocketDnsCache> HttpSession::dnsCache() const { return QSharedPointer<SocketDnsCache>(); }
QSharedPointer<SocketProxy> HttpSession::socketProxy() const { return QSharedPointer<SocketProxy>(); }
void HttpSession::setSocketProxy(QSharedPointer<SocketProxy> proxy) { Q_UNUSED(proxy); }
QSharedPointer<HttpProxy> HttpSession::httpProxy() const { return QSharedPointer<HttpProxy>(); }
void HttpSession::setHttpProxy(QSharedPointer<HttpProxy> proxy) { Q_UNUSED(proxy); }
QSharedPointer<HttpCacheManager> HttpSession::cacheManager() const { return QSharedPointer<HttpCacheManager>(); }
void HttpSession::setCacheManager(QSharedPointer<HttpCacheManager> cacheManager) { Q_UNUSED(cacheManager); }
WebSocketConfiguration &HttpSession::webSocketConfiguration() { return d_ptr->webSocketConfiguration; }

void setProxySwitcher(HttpSession *session, QSharedPointer<BaseProxySwitcher> switcher)
{
    if (!session) {
        return;
    }
    session->d_ptr->proxySwitcher = switcher;
}

#ifndef QTNG_NO_CRYPTO
SslConfiguration &HttpSession::sslConfiguration()
{
    static SslConfiguration config;
    return config;
}
#endif

HttpCacheManager::HttpCacheManager() { }
HttpCacheManager::~HttpCacheManager() { }
bool HttpCacheManager::addResponse(HttpResponse &response) { Q_UNUSED(response); return false; }
bool HttpCacheManager::getResponse(HttpResponse *response) { Q_UNUSED(response); return false; }
bool HttpCacheManager::store(const QString &url, const QByteArray &data) { Q_UNUSED(url); Q_UNUSED(data); return false; }
QByteArray HttpCacheManager::load(const QString &url) { Q_UNUSED(url); return QByteArray(); }

class HttpMemoryCacheManagerPrivate
{
public:
    QMap<QString, QByteArray> cache;
    float expireTime = 300.0f;
};

HttpMemoryCacheManager::HttpMemoryCacheManager()
    : d_ptr(new HttpMemoryCacheManagerPrivate)
{
}

HttpMemoryCacheManager::~HttpMemoryCacheManager()
{
    delete d_ptr;
}

float HttpMemoryCacheManager::expireTime() const { Q_D(const HttpMemoryCacheManager); return d->expireTime; }
void HttpMemoryCacheManager::setExpireTime(float expireTime) { Q_D(HttpMemoryCacheManager); d->expireTime = expireTime; }
QMap<QString, QByteArray> &HttpMemoryCacheManager::cache() { Q_D(HttpMemoryCacheManager); return d->cache; }
bool HttpMemoryCacheManager::store(const QString &url, const QByteArray &data) { Q_D(HttpMemoryCacheManager); d->cache.insert(url, data); return true; }
QByteArray HttpMemoryCacheManager::load(const QString &url) { Q_D(const HttpMemoryCacheManager); return d->cache.value(url); }

bool HttpDiskCacheManager::store(const QString &url, const QByteArray &data)
{
    QFile f(cacheDir.filePath(QString::number(qHash(url))));
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray HttpDiskCacheManager::load(const QString &url)
{
    QFile f(cacheDir.filePath(QString::number(qHash(url))));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString HTTPError::what() const { return QString::number(statusCode); }
QString ConnectionError::what() const { return QString::fromLatin1("connection error"); }
QString ProxyError::what() const { return QString::fromLatin1("proxy error"); }
QString SSLError::what() const { return QString::fromLatin1("ssl error"); }
QString RequestTimeout::what() const { return QString::fromLatin1("request timeout"); }
QString ConnectTimeout::what() const { return QString::fromLatin1("connect timeout"); }
QString ReadTimeout::what() const { return QString::fromLatin1("read timeout"); }
QString URLRequired::what() const { return QString::fromLatin1("url required"); }
QString TooManyRedirects::what() const { return QString::fromLatin1("too many redirects"); }
QString MissingSchema::what() const { return QString::fromLatin1("missing schema"); }
QString InvalidScheme::what() const { return QString::fromLatin1("invalid scheme"); }
QString UnsupportedVersion::what() const { return QString::fromLatin1("unsupported version"); }
QString InvalidURL::what() const { return QString::fromLatin1("invalid url"); }
QString InvalidHeader::what() const { return QString::fromLatin1("invalid header"); }
QString ChunkedEncodingError::what() const { return QString::fromLatin1("chunked encoding error"); }
QString ContentDecodingError::what() const { return QString::fromLatin1("content decoding error"); }
QString StreamConsumedError::what() const { return QString::fromLatin1("stream consumed"); }
QString RetryError::what() const { return QString::fromLatin1("retry error"); }
QString UnrewindableBodyError::what() const { return QString::fromLatin1("unrewindable body"); }

}  // namespace QTNETWORKNG_NAMESPACE
