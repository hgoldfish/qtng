#include <QtCore/qjsondocument.h>
#include <QtCore/qurlquery.h>

#include "bridge/core_access.h"
#include "bridge/http_access.h"
#include "bridge/io_bridge.h"
#include "bridge/socket_access.h"
#include "bridge/ssl_access.h"
#include "http.h"
#include "ssl.h"
#include "websocket.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

// Friend of HttpRequest/HttpResponse. Since both hold their core object (and
// thus the headers) in the same private payload, crossing the layer is a move
// for the core->Qt direction and a per-field deep copy for the Qt->core
// direction (core::HttpSession::send() mutates the request in place, and the
// core copy constructor shares the private data).
class QtHttpBridgeAccess
{
public:
    static qtng_core::HttpRequest coreOf(const HttpRequest &req);
    static qtng_core::HttpResponse coreOf(const HttpResponse &resp);
    static HttpRequest fromCore(qtng_core::HttpRequest core);
    static HttpResponse fromCore(qtng_core::HttpResponse core);
};

namespace {

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
    return QtHttpBridgeAccess::coreOf(req);
}

HttpRequest toQtRequest(const qtng_core::HttpRequest &core)
{
    return QtHttpBridgeAccess::fromCore(qtng_core::detail::HttpDeepCopy::request(core));
}

HttpResponse toQtResponse(qtng_core::HttpResponse core)
{
    return QtHttpBridgeAccess::fromCore(std::move(core));
}

qtng_core::HttpResponse toCoreResponse(HttpResponse &resp)
{
    // Whole-payload deep copy through the private bridge. It copies the private
    // data directly and does not run the body accessors, so it cannot consume a
    // streaming body nor re-enter the Qt layer.
    return QtHttpBridgeAccess::coreOf(resp);
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

namespace {

class CoreSocketProxyAdapter : public qtng_core::SocketProxy
{
public:
    explicit CoreSocketProxyAdapter(QSharedPointer<QTNETWORKNG_NAMESPACE::SocketProxy> proxy)
        : proxy(std::move(proxy))
    {
    }

    std::shared_ptr<qtng_core::SocketLike> connect(const qtng_core::HostAddress &addr, std::uint16_t port) override
    {
        if (!proxy) {
            return std::shared_ptr<qtng_core::SocketLike>();
        }
        return toCoreSocketLike(proxy->connect(toQtAddress(addr), port));
    }

    std::shared_ptr<qtng_core::SocketLike> connect(const std::string &addr, std::uint16_t port) override
    {
        if (!proxy) {
            return std::shared_ptr<qtng_core::SocketLike>();
        }
        return toCoreSocketLike(proxy->connect(toQString(addr), port));
    }

    QSharedPointer<QTNETWORKNG_NAMESPACE::SocketProxy> proxy;
};

class QtSocketProxyAdapter : public SocketProxy
{
public:
    explicit QtSocketProxyAdapter(std::shared_ptr<qtng_core::SocketProxy> core)
        : core(std::move(core))
    {
    }

    QSharedPointer<SocketLike> connect(const HostAddress &addr, quint16 port) override
    {
        if (!core) {
            return QSharedPointer<SocketLike>();
        }
        return toQtSocketLike(core->connect(toCoreAddress(addr), port));
    }

    QSharedPointer<SocketLike> connect(const QString &addr, quint16 port) override
    {
        if (!core) {
            return QSharedPointer<SocketLike>();
        }
        return toQtSocketLike(core->connect(toStdString(addr), port));
    }

    std::shared_ptr<qtng_core::SocketProxy> core;
};

class CoreHttpProxyAdapter : public qtng_core::HttpProxy
{
public:
    explicit CoreHttpProxyAdapter(QSharedPointer<QTNETWORKNG_NAMESPACE::HttpProxy> proxy)
        : proxy(std::move(proxy))
    {
    }

    std::shared_ptr<qtng_core::SocketLike> connect(const qtng_core::HostAddress &addr, std::uint16_t port) override
    {
        if (!proxy) {
            return std::shared_ptr<qtng_core::SocketLike>();
        }
        return toCoreSocketLike(proxy->connect(toQtAddress(addr), port));
    }

    std::shared_ptr<qtng_core::SocketLike> connect(const std::string &addr, std::uint16_t port) override
    {
        if (!proxy) {
            return std::shared_ptr<qtng_core::SocketLike>();
        }
        return toCoreSocketLike(proxy->connect(toQString(addr), port));
    }

    QSharedPointer<QTNETWORKNG_NAMESPACE::HttpProxy> proxy;
};

class QtHttpProxyAdapter : public HttpProxy
{
public:
    explicit QtHttpProxyAdapter(std::shared_ptr<qtng_core::HttpProxy> core)
        : core(std::move(core))
    {
        if (this->core) {
            setHostName(toQString(this->core->hostName()));
            setPort(this->core->port());
            setUser(toQString(this->core->user()));
            setPassword(toQString(this->core->password()));
            setHeaders(toQtHeaders(this->core->allHeaders()));
        }
    }

    QSharedPointer<SocketLike> connect(const HostAddress &addr, quint16 port) override
    {
        if (!core) {
            return QSharedPointer<SocketLike>();
        }
        return toQtSocketLike(core->connect(toCoreAddress(addr), port));
    }

    QSharedPointer<SocketLike> connect(const QString &addr, quint16 port) override
    {
        if (!core) {
            return QSharedPointer<SocketLike>();
        }
        return toQtSocketLike(core->connect(toStdString(addr), port));
    }

    std::shared_ptr<qtng_core::HttpProxy> core;
};

class CoreHttpCacheManagerAdapter : public qtng_core::HttpCacheManager
{
public:
    explicit CoreHttpCacheManagerAdapter(QSharedPointer<QTNETWORKNG_NAMESPACE::HttpCacheManager> manager)
        : manager(std::move(manager))
    {
    }

    bool addResponse(qtng_core::HttpResponse &response) override
    {
        if (!manager) {
            return false;
        }
        HttpResponse qr = toQtResponse(response);
        return manager->addResponse(qr);
    }

    bool getResponse(qtng_core::HttpResponse *response) override
    {
        if (!manager || !response) {
            return false;
        }
        HttpResponse qr = toQtResponse(*response);
        const bool ok = manager->getResponse(&qr);
        if (ok) {
            *response = toCoreResponse(qr);
        }
        return ok;
    }

public:
    QSharedPointer<QTNETWORKNG_NAMESPACE::HttpCacheManager> manager;
};

class QtHttpCacheManagerAdapter : public HttpCacheManager
{
public:
    explicit QtHttpCacheManagerAdapter(std::shared_ptr<qtng_core::HttpCacheManager> core)
        : core(std::move(core))
    {
    }

    bool addResponse(HttpResponse &response) override
    {
        if (!core) {
            return false;
        }
        qtng_core::HttpResponse coreResp = toCoreResponse(response);
        return core->addResponse(coreResp);
    }

    bool getResponse(HttpResponse *response) override
    {
        if (!core || !response) {
            return false;
        }
        qtng_core::HttpResponse coreResp = toCoreResponse(*response);
        const bool ok = core->getResponse(&coreResp);
        if (ok) {
            *response = toQtResponse(coreResp);
        }
        return ok;
    }

public:
    std::shared_ptr<qtng_core::HttpCacheManager> core;
};

}  // namespace

class HttpRequestPrivate : public QSharedData
{
public:
    qtng_core::HttpRequest core;
};

FormData::FormData()
    : boundary(toQByteArray(qtng_core::FormData::makeBoundary()))
{
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
    : d(other.d)
{
}

HttpRequest::HttpRequest(HttpRequest &&other)
    : d(std::move(other.d))
{
}

HttpRequest &HttpRequest::operator=(const HttpRequest &other)
{
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

void HttpRequest::setBody(const FormData &formData)
{
    const QByteArray contentType = QByteArrayLiteral("multipart/form-data; boundary=") + formData.boundary;
    setHeader(QStringLiteral("Content-Type"), contentType);
    if (!hasHeader(QStringLiteral("MIME-Version"))) {
        setHeader(QStringLiteral("MIME-Version"), QByteArrayLiteral("1.0"));
    }
    setBody(formData.toByteArray());
}
void HttpRequest::setBody(const QJsonDocument &json) { setBody(json.toJson(QJsonDocument::Compact)); }
void HttpRequest::setBody(const QJsonObject &json) { setBody(QJsonDocument(json)); }
void HttpRequest::setBody(const QJsonArray &json) { setBody(QJsonDocument(json)); }
void HttpRequest::setBody(const QMap<QString, QString> form) { d->core.setBody(toCoreStringMap(form)); }
void HttpRequest::setBody(const QUrlQuery &form) { d->core.setBody(toCoreUrlQuery(form)); }

void HttpRequest::setContentType(const QString &contentType) { d->core.setContentType(toStdString(contentType)); }
QString HttpRequest::getContentType() const { return toQString(d->core.getContentType()); }
void HttpRequest::setContentLength(qint64 contentLength) { d->core.setContentLength(contentLength); }
qint64 HttpRequest::getContentLength() const { return d->core.getContentLength(); }
void HttpRequest::setLocation(const QUrl &url) { d->core.setLocation(toStdString(url.toEncoded(QUrl::FullyEncoded))); }
QUrl HttpRequest::getLocation() const
{
    const QByteArray value = toQByteArray(d->core.getLocation());
    if (value.isEmpty()) {
        return QUrl();
    }
    const QUrl result = QUrl::fromEncoded(value, QUrl::StrictMode);
    return result.isValid() ? result : QUrl();
}
void HttpRequest::setLastModified(const QDateTime &lastModified) { d->core.setLastModified(toCoreDateTime(lastModified)); }
QDateTime HttpRequest::getLastModified() const { return toQDateTime(d->core.getLastModified()); }
void HttpRequest::setModifiedSince(const QDateTime &modifiedSince) { d->core.setModifiedSince(toCoreDateTime(modifiedSince)); }
QDateTime HttpRequest::getModifedSince() const { return toQDateTime(d->core.getModifedSince()); }

void HttpRequest::setHeader(const QString &name, const QByteArray &value)
{
    d->core.setHeader(toStdString(name), toStdString(value));
}
void HttpRequest::addHeader(const QString &name, const QByteArray &value)
{
    d->core.addHeader(toStdString(name), toStdString(value));
}
void HttpRequest::addHeader(const HttpHeader &header) { d->core.addHeader(toCoreHeader(header)); }
bool HttpRequest::hasHeader(const QString &name) const { return d->core.hasHeader(toStdString(name)); }
bool HttpRequest::removeHeader(const QString &name) { return d->core.removeHeader(toStdString(name)); }
void HttpRequest::setHeader(KnownHeader header, const QByteArray &value) { setHeader(toString(header), value); }
void HttpRequest::addHeader(KnownHeader header, const QByteArray &value) { addHeader(toString(header), value); }
bool HttpRequest::hasHeader(KnownHeader header) const { return hasHeader(toString(header)); }
bool HttpRequest::removeHeader(KnownHeader header) { return removeHeader(toString(header)); }
QByteArray HttpRequest::header(const QString &name, const QByteArray &defaultValue) const
{
    return toQByteArray(d->core.header(toStdString(name), toStdString(defaultValue)));
}
QByteArray HttpRequest::header(KnownHeader knownHeader, const QByteArray &defaultValue) const
{
    return header(toString(knownHeader), defaultValue);
}
QList<QByteArray> HttpRequest::multiHeader(const QString &name) const
{
    QList<QByteArray> result;
    for (const string &v : d->core.multiHeader(toStdString(name))) {
        result.append(toQByteArray(v));
    }
    return result;
}
QList<QByteArray> HttpRequest::multiHeader(KnownHeader header) const { return multiHeader(toString(header)); }
QList<HttpHeader> HttpRequest::allHeaders() const { return toQtHeaders(d->core.allHeaders()); }
void HttpRequest::setHeaders(const QMap<QString, QByteArray> headers) { d->core.setHeaders(toCoreHeaderMap(headers)); }
void HttpRequest::setHeaders(const QList<HttpHeader> &headers) { d->core.setHeaders(toCoreHeaders(headers)); }

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

qtng_core::HttpRequest QtHttpBridgeAccess::coreOf(const HttpRequest &req)
{
    // The core copy constructor shares the private payload and
    // core::HttpSession::send() mutates the request in place (version, cookies),
    // so build an independent object through the private deep-copy bridge.
    return qtng_core::detail::HttpDeepCopy::request(req.d->core);
}

qtng_core::HttpResponse QtHttpBridgeAccess::coreOf(const HttpResponse &resp)
{
    return qtng_core::detail::HttpDeepCopy::response(resp.d->core);
}

HttpRequest QtHttpBridgeAccess::fromCore(qtng_core::HttpRequest core)
{
    HttpRequest req;
    req.d->core = std::move(core);
    return req;
}

HttpResponse QtHttpBridgeAccess::fromCore(qtng_core::HttpResponse core)
{
    HttpResponse resp;
    resp.d->core = std::move(core);
    return resp;
}

HttpResponse::~HttpResponse() { }
HttpResponse::HttpResponse(const HttpResponse &other)
    : d(other.d)
{
}

HttpResponse::HttpResponse(HttpResponse &&other)
    : d(std::move(other.d))
{
}

HttpResponse &HttpResponse::operator=(const HttpResponse &other)
{
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

void HttpResponse::setContentType(const QString &contentType) { d->core.setContentType(toStdString(contentType)); }
QString HttpResponse::getContentType() const { return toQString(d->core.getContentType()); }
void HttpResponse::setContentLength(qint64 contentLength) { d->core.setContentLength(contentLength); }
qint64 HttpResponse::getContentLength() const { return d->core.getContentLength(); }
void HttpResponse::setLocation(const QUrl &url) { d->core.setLocation(toStdString(url.toEncoded(QUrl::FullyEncoded))); }
QUrl HttpResponse::getLocation() const
{
    const QByteArray value = toQByteArray(d->core.getLocation());
    if (value.isEmpty()) {
        return QUrl();
    }
    const QUrl result = QUrl::fromEncoded(value, QUrl::StrictMode);
    return result.isValid() ? result : QUrl();
}
void HttpResponse::setLastModified(const QDateTime &lastModified)
{
    d->core.setLastModified(toCoreDateTime(lastModified));
}
QDateTime HttpResponse::getLastModified() const { return toQDateTime(d->core.getLastModified()); }
void HttpResponse::setModifiedSince(const QDateTime &modifiedSince)
{
    d->core.setModifiedSince(toCoreDateTime(modifiedSince));
}
QDateTime HttpResponse::getModifedSince() const { return toQDateTime(d->core.getModifedSince()); }

void HttpResponse::setHeader(const QString &name, const QByteArray &value)
{
    d->core.setHeader(toStdString(name), toStdString(value));
}
void HttpResponse::addHeader(const QString &name, const QByteArray &value)
{
    d->core.addHeader(toStdString(name), toStdString(value));
}
void HttpResponse::addHeader(const HttpHeader &header) { d->core.addHeader(toCoreHeader(header)); }
bool HttpResponse::hasHeader(const QString &name) const { return d->core.hasHeader(toStdString(name)); }
bool HttpResponse::removeHeader(const QString &name) { return d->core.removeHeader(toStdString(name)); }
void HttpResponse::setHeader(KnownHeader header, const QByteArray &value) { setHeader(toString(header), value); }
void HttpResponse::addHeader(KnownHeader header, const QByteArray &value) { addHeader(toString(header), value); }
bool HttpResponse::hasHeader(KnownHeader header) const { return hasHeader(toString(header)); }
bool HttpResponse::removeHeader(KnownHeader header) { return removeHeader(toString(header)); }
QByteArray HttpResponse::header(const QString &name, const QByteArray &defaultValue) const
{
    return toQByteArray(d->core.header(toStdString(name), toStdString(defaultValue)));
}
QByteArray HttpResponse::header(KnownHeader knownHeader, const QByteArray &defaultValue) const
{
    return header(toString(knownHeader), defaultValue);
}
QList<QByteArray> HttpResponse::multiHeader(const QString &name) const
{
    QList<QByteArray> result;
    for (const string &v : d->core.multiHeader(toStdString(name))) {
        result.append(toQByteArray(v));
    }
    return result;
}
QList<QByteArray> HttpResponse::multiHeader(KnownHeader header) const { return multiHeader(toString(header)); }
QList<HttpHeader> HttpResponse::allHeaders() const { return toQtHeaders(d->core.allHeaders()); }
void HttpResponse::setHeaders(const QMap<QString, QByteArray> headers) { d->core.setHeaders(toCoreHeaderMap(headers)); }
void HttpResponse::setHeaders(const QList<HttpHeader> &headers) { d->core.setHeaders(toCoreHeaders(headers)); }

class HttpSessionPrivate
{
public:
    explicit HttpSessionPrivate(HttpSession *q)
        : q_ptr(q)
        , debugLevel(0)
        , managingCookies(true)
        , keepAlive(true)
    {
        bindHttpCookieJarToCore(&cookieJar, &core.cookieJar());
        bindWebSocketConfiguration(&webSocketConfiguration, &core.webSocketConfiguration());
#ifndef QTNG_NO_CRYPTO
        bindSslConfigurationToCore(&sslConfiguration, &core.sslConfiguration());
#endif
    }

    qtng_core::HttpSession core;
    HttpCookieJar cookieJar;
    WebSocketConfiguration webSocketConfiguration;
#ifndef QTNG_NO_CRYPTO
    SslConfiguration sslConfiguration;
#endif
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

HttpResponse HttpSession::get(const QUrl &url)
{
    return toQtResponse(d_ptr->core.get(toCoreUrl(url).toString()));
}

HttpResponse HttpSession::get(const QString &url)
{
    return get(QUrl::fromUserInput(url));
}

HttpResponse HttpSession::head(const QUrl &url)
{
    return toQtResponse(d_ptr->core.head(toCoreUrl(url).toString()));
}

HttpResponse HttpSession::head(const QString &url)
{
    return head(QUrl::fromUserInput(url));
}

HttpResponse HttpSession::options(const QUrl &url)
{
    return toQtResponse(d_ptr->core.options(toCoreUrl(url).toString()));
}

HttpResponse HttpSession::options(const QString &url)
{
    return options(QUrl::fromUserInput(url));
}

HttpResponse HttpSession::delete_(const QUrl &url)
{
    return toQtResponse(d_ptr->core.delete_(toCoreUrl(url).toString()));
}

HttpResponse HttpSession::delete_(const QString &url)
{
    return delete_(QUrl::fromUserInput(url));
}

HttpResponse HttpSession::get(const QUrl &url, const QMap<QString, QString> &query)
{
    return toQtResponse(d_ptr->core.get(toCoreUrl(url).toString(), toCoreStringMap(query)));
}

HttpResponse HttpSession::get(const QUrl &url, const QMap<QString, QString> &query, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.get(toCoreUrl(url).toString(), toCoreStringMap(query), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::get(const QUrl &url, const QUrlQuery &query)
{
    return toQtResponse(d_ptr->core.get(toCoreUrl(url).toString(), toCoreUrlQuery(query)));
}

HttpResponse HttpSession::get(const QUrl &url, const QUrlQuery &query, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.get(toCoreUrl(url).toString(), toCoreUrlQuery(query), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::get(const QString &url, const QMap<QString, QString> &query)
{
    return get(QUrl::fromUserInput(url), query);
}

HttpResponse HttpSession::get(const QString &url, const QMap<QString, QString> &query, const QMap<QString, QByteArray> &headers)
{
    return get(QUrl::fromUserInput(url), query, headers);
}

HttpResponse HttpSession::get(const QString &url, const QUrlQuery &query)
{
    return get(QUrl::fromUserInput(url), query);
}

HttpResponse HttpSession::get(const QString &url, const QUrlQuery &query, const QMap<QString, QByteArray> &headers)
{
    return get(QUrl::fromUserInput(url), query, headers);
}

HttpResponse HttpSession::head(const QUrl &url, const QMap<QString, QString> &query)
{
    return toQtResponse(d_ptr->core.head(toCoreUrl(url).toString(), toCoreStringMap(query)));
}

HttpResponse HttpSession::head(const QUrl &url, const QMap<QString, QString> &query, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.head(toCoreUrl(url).toString(), toCoreStringMap(query), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::head(const QUrl &url, const QUrlQuery &query)
{
    return toQtResponse(d_ptr->core.head(toCoreUrl(url).toString(), toCoreUrlQuery(query)));
}

HttpResponse HttpSession::head(const QUrl &url, const QUrlQuery &query, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.head(toCoreUrl(url).toString(), toCoreUrlQuery(query), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::head(const QString &url, const QMap<QString, QString> &query)
{
    return head(QUrl::fromUserInput(url), query);
}

HttpResponse HttpSession::head(const QString &url, const QMap<QString, QString> &query, const QMap<QString, QByteArray> &headers)
{
    return head(QUrl::fromUserInput(url), query, headers);
}

HttpResponse HttpSession::head(const QString &url, const QUrlQuery &query)
{
    return head(QUrl::fromUserInput(url), query);
}

HttpResponse HttpSession::head(const QString &url, const QUrlQuery &query, const QMap<QString, QByteArray> &headers)
{
    return head(QUrl::fromUserInput(url), query, headers);
}

HttpResponse HttpSession::options(const QUrl &url, const QMap<QString, QString> &query)
{
    return toQtResponse(d_ptr->core.options(toCoreUrl(url).toString(), toCoreStringMap(query)));
}

HttpResponse HttpSession::options(const QUrl &url, const QMap<QString, QString> &query, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.options(toCoreUrl(url).toString(), toCoreStringMap(query), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::options(const QUrl &url, const QUrlQuery &query)
{
    return toQtResponse(d_ptr->core.options(toCoreUrl(url).toString(), toCoreUrlQuery(query)));
}

HttpResponse HttpSession::options(const QUrl &url, const QUrlQuery &query, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.options(toCoreUrl(url).toString(), toCoreUrlQuery(query), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::options(const QString &url, const QMap<QString, QString> &query)
{
    return options(QUrl::fromUserInput(url), query);
}

HttpResponse HttpSession::options(const QString &url, const QMap<QString, QString> &query, const QMap<QString, QByteArray> &headers)
{
    return options(QUrl::fromUserInput(url), query, headers);
}

HttpResponse HttpSession::options(const QString &url, const QUrlQuery &query)
{
    return options(QUrl::fromUserInput(url), query);
}

HttpResponse HttpSession::options(const QString &url, const QUrlQuery &query, const QMap<QString, QByteArray> &headers)
{
    return options(QUrl::fromUserInput(url), query, headers);
}

HttpResponse HttpSession::delete_(const QUrl &url, const QMap<QString, QString> &query)
{
    return toQtResponse(d_ptr->core.delete_(toCoreUrl(url).toString(), toCoreStringMap(query)));
}

HttpResponse HttpSession::delete_(const QUrl &url, const QMap<QString, QString> &query, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.delete_(toCoreUrl(url).toString(), toCoreStringMap(query), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::delete_(const QUrl &url, const QUrlQuery &query)
{
    return toQtResponse(d_ptr->core.delete_(toCoreUrl(url).toString(), toCoreUrlQuery(query)));
}

HttpResponse HttpSession::delete_(const QUrl &url, const QUrlQuery &query, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.delete_(toCoreUrl(url).toString(), toCoreUrlQuery(query), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::delete_(const QString &url, const QMap<QString, QString> &query)
{
    return delete_(QUrl::fromUserInput(url), query);
}

HttpResponse HttpSession::delete_(const QString &url, const QMap<QString, QString> &query, const QMap<QString, QByteArray> &headers)
{
    return delete_(QUrl::fromUserInput(url), query, headers);
}

HttpResponse HttpSession::delete_(const QString &url, const QUrlQuery &query)
{
    return delete_(QUrl::fromUserInput(url), query);
}

HttpResponse HttpSession::delete_(const QString &url, const QUrlQuery &query, const QMap<QString, QByteArray> &headers)
{
    return delete_(QUrl::fromUserInput(url), query, headers);
}

HttpResponse HttpSession::post(const QUrl &url, QSharedPointer<FileLike> body)
{
    return toQtResponse(d_ptr->core.post(toCoreUrl(url).toString(), toCoreFileLike(body)));
}

HttpResponse HttpSession::post(const QUrl &url, const QMap<QString, QString> &body)
{
    return toQtResponse(d_ptr->core.post(toCoreUrl(url).toString(), toCoreStringMap(body)));
}

HttpResponse HttpSession::post(const QUrl &url, const QUrlQuery &body)
{
    return toQtResponse(d_ptr->core.post(toCoreUrl(url).toString(), toCoreUrlQuery(body)));
}

HttpResponse HttpSession::post(const QUrl &url, const QJsonDocument &body)
{
    return post(url, body.toJson(QJsonDocument::Compact));
}

HttpResponse HttpSession::post(const QUrl &url, const QJsonObject &body)
{
    return post(url, QJsonDocument(body));
}

HttpResponse HttpSession::post(const QUrl &url, const QJsonArray &body)
{
    return post(url, QJsonDocument(body));
}

HttpResponse HttpSession::post(const QUrl &url, const FormData &body)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("POST"));
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const QUrl &url, const QByteArray &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.post(toCoreUrl(url).toString(), toStdString(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::post(const QUrl &url, QSharedPointer<FileLike> body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.post(toCoreUrl(url).toString(), toCoreFileLike(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::post(const QUrl &url, const QMap<QString, QString> &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.post(toCoreUrl(url).toString(), toCoreStringMap(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::post(const QUrl &url, const QUrlQuery &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.post(toCoreUrl(url).toString(), toCoreUrlQuery(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::post(const QUrl &url, const QJsonDocument &body, const QMap<QString, QByteArray> &headers)
{
    return post(url, body.toJson(QJsonDocument::Compact), headers);
}

HttpResponse HttpSession::post(const QUrl &url, const QJsonObject &body, const QMap<QString, QByteArray> &headers)
{
    return post(url, QJsonDocument(body), headers);
}

HttpResponse HttpSession::post(const QUrl &url, const QJsonArray &body, const QMap<QString, QByteArray> &headers)
{
    return post(url, QJsonDocument(body), headers);
}

HttpResponse HttpSession::post(const QUrl &url, const FormData &body, const QMap<QString, QByteArray> &headers)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("POST"));
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const QString &url, QSharedPointer<FileLike> body)
{
    return post(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::post(const QString &url, const QMap<QString, QString> &body)
{
    return post(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::post(const QString &url, const QUrlQuery &body)
{
    return post(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::post(const QString &url, const QJsonDocument &body)
{
    return post(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::post(const QString &url, const QJsonObject &body)
{
    return post(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::post(const QString &url, const QJsonArray &body)
{
    return post(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::post(const QString &url, const FormData &body)
{
    return post(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::post(const QString &url, const FormData &body, const QMap<QString, QByteArray> &headers)
{
    return post(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::post(const QString &url, const QByteArray &body, const QMap<QString, QByteArray> &headers)
{
    return post(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::post(const QString &url, QSharedPointer<FileLike> body, const QMap<QString, QByteArray> &headers)
{
    return post(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::post(const QString &url, const QMap<QString, QString> &body, const QMap<QString, QByteArray> &headers)
{
    return post(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::post(const QString &url, const QUrlQuery &body, const QMap<QString, QByteArray> &headers)
{
    return post(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::post(const QString &url, const QJsonDocument &body, const QMap<QString, QByteArray> &headers)
{
    return post(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::post(const QString &url, const QJsonObject &body, const QMap<QString, QByteArray> &headers)
{
    return post(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::post(const QString &url, const QJsonArray &body, const QMap<QString, QByteArray> &headers)
{
    return post(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::put(const QUrl &url, QSharedPointer<FileLike> body)
{
    return toQtResponse(d_ptr->core.put(toCoreUrl(url).toString(), toCoreFileLike(body)));
}

HttpResponse HttpSession::put(const QUrl &url, const QMap<QString, QString> &body)
{
    return toQtResponse(d_ptr->core.put(toCoreUrl(url).toString(), toCoreStringMap(body)));
}

HttpResponse HttpSession::put(const QUrl &url, const QUrlQuery &body)
{
    return toQtResponse(d_ptr->core.put(toCoreUrl(url).toString(), toCoreUrlQuery(body)));
}

HttpResponse HttpSession::put(const QUrl &url, const QJsonDocument &body)
{
    return put(url, body.toJson(QJsonDocument::Compact));
}

HttpResponse HttpSession::put(const QUrl &url, const QJsonObject &body)
{
    return put(url, QJsonDocument(body));
}

HttpResponse HttpSession::put(const QUrl &url, const QJsonArray &body)
{
    return put(url, QJsonDocument(body));
}

HttpResponse HttpSession::put(const QUrl &url, const FormData &body)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("PUT"));
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const QUrl &url, const QByteArray &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.put(toCoreUrl(url).toString(), toStdString(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::put(const QUrl &url, QSharedPointer<FileLike> body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.put(toCoreUrl(url).toString(), toCoreFileLike(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::put(const QUrl &url, const QMap<QString, QString> &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.put(toCoreUrl(url).toString(), toCoreStringMap(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::put(const QUrl &url, const QUrlQuery &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.put(toCoreUrl(url).toString(), toCoreUrlQuery(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::put(const QUrl &url, const QJsonDocument &body, const QMap<QString, QByteArray> &headers)
{
    return put(url, body.toJson(QJsonDocument::Compact), headers);
}

HttpResponse HttpSession::put(const QUrl &url, const QJsonObject &body, const QMap<QString, QByteArray> &headers)
{
    return put(url, QJsonDocument(body), headers);
}

HttpResponse HttpSession::put(const QUrl &url, const QJsonArray &body, const QMap<QString, QByteArray> &headers)
{
    return put(url, QJsonDocument(body), headers);
}

HttpResponse HttpSession::put(const QUrl &url, const FormData &body, const QMap<QString, QByteArray> &headers)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("PUT"));
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const QString &url, QSharedPointer<FileLike> body)
{
    return put(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::put(const QString &url, const QMap<QString, QString> &body)
{
    return put(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::put(const QString &url, const QUrlQuery &body)
{
    return put(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::put(const QString &url, const QJsonDocument &body)
{
    return put(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::put(const QString &url, const QJsonObject &body)
{
    return put(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::put(const QString &url, const QJsonArray &body)
{
    return put(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::put(const QString &url, const FormData &body)
{
    return put(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::put(const QString &url, const FormData &body, const QMap<QString, QByteArray> &headers)
{
    return put(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::put(const QString &url, const QByteArray &body, const QMap<QString, QByteArray> &headers)
{
    return put(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::put(const QString &url, QSharedPointer<FileLike> body, const QMap<QString, QByteArray> &headers)
{
    return put(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::put(const QString &url, const QMap<QString, QString> &body, const QMap<QString, QByteArray> &headers)
{
    return put(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::put(const QString &url, const QUrlQuery &body, const QMap<QString, QByteArray> &headers)
{
    return put(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::put(const QString &url, const QJsonDocument &body, const QMap<QString, QByteArray> &headers)
{
    return put(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::put(const QString &url, const QJsonObject &body, const QMap<QString, QByteArray> &headers)
{
    return put(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::put(const QString &url, const QJsonArray &body, const QMap<QString, QByteArray> &headers)
{
    return put(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::patch(const QUrl &url, QSharedPointer<FileLike> body)
{
    return toQtResponse(d_ptr->core.patch(toCoreUrl(url).toString(), toCoreFileLike(body)));
}

HttpResponse HttpSession::patch(const QUrl &url, const QMap<QString, QString> &body)
{
    return toQtResponse(d_ptr->core.patch(toCoreUrl(url).toString(), toCoreStringMap(body)));
}

HttpResponse HttpSession::patch(const QUrl &url, const QUrlQuery &body)
{
    return toQtResponse(d_ptr->core.patch(toCoreUrl(url).toString(), toCoreUrlQuery(body)));
}

HttpResponse HttpSession::patch(const QUrl &url, const QJsonDocument &body)
{
    return patch(url, body.toJson(QJsonDocument::Compact));
}

HttpResponse HttpSession::patch(const QUrl &url, const QJsonObject &body)
{
    return patch(url, QJsonDocument(body));
}

HttpResponse HttpSession::patch(const QUrl &url, const QJsonArray &body)
{
    return patch(url, QJsonDocument(body));
}

HttpResponse HttpSession::patch(const QUrl &url, const FormData &body)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("PATCH"));
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const QUrl &url, const QByteArray &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.patch(toCoreUrl(url).toString(), toStdString(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::patch(const QUrl &url, QSharedPointer<FileLike> body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.patch(toCoreUrl(url).toString(), toCoreFileLike(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::patch(const QUrl &url, const QMap<QString, QString> &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.patch(toCoreUrl(url).toString(), toCoreStringMap(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::patch(const QUrl &url, const QUrlQuery &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.patch(toCoreUrl(url).toString(), toCoreUrlQuery(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::patch(const QUrl &url, const QJsonDocument &body, const QMap<QString, QByteArray> &headers)
{
    return patch(url, body.toJson(QJsonDocument::Compact), headers);
}

HttpResponse HttpSession::patch(const QUrl &url, const QJsonObject &body, const QMap<QString, QByteArray> &headers)
{
    return patch(url, QJsonDocument(body), headers);
}

HttpResponse HttpSession::patch(const QUrl &url, const QJsonArray &body, const QMap<QString, QByteArray> &headers)
{
    return patch(url, QJsonDocument(body), headers);
}

HttpResponse HttpSession::patch(const QUrl &url, const FormData &body, const QMap<QString, QByteArray> &headers)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("PATCH"));
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const QString &url, QSharedPointer<FileLike> body)
{
    return patch(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::patch(const QString &url, const QMap<QString, QString> &body)
{
    return patch(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::patch(const QString &url, const QUrlQuery &body)
{
    return patch(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::patch(const QString &url, const QJsonDocument &body)
{
    return patch(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::patch(const QString &url, const QJsonObject &body)
{
    return patch(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::patch(const QString &url, const QJsonArray &body)
{
    return patch(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::patch(const QString &url, const FormData &body)
{
    return patch(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::patch(const QString &url, const FormData &body, const QMap<QString, QByteArray> &headers)
{
    return patch(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::patch(const QString &url, const QByteArray &body, const QMap<QString, QByteArray> &headers)
{
    return patch(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::patch(const QString &url, QSharedPointer<FileLike> body, const QMap<QString, QByteArray> &headers)
{
    return patch(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::patch(const QString &url, const QMap<QString, QString> &body, const QMap<QString, QByteArray> &headers)
{
    return patch(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::patch(const QString &url, const QUrlQuery &body, const QMap<QString, QByteArray> &headers)
{
    return patch(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::patch(const QString &url, const QJsonDocument &body, const QMap<QString, QByteArray> &headers)
{
    return patch(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::patch(const QString &url, const QJsonObject &body, const QMap<QString, QByteArray> &headers)
{
    return patch(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::patch(const QString &url, const QJsonArray &body, const QMap<QString, QByteArray> &headers)
{
    return patch(QUrl::fromUserInput(url), body, headers);
}

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
HttpCookie HttpSession::cookie(const QUrl &url, const QString &name)
{
    return toQtCookie(d_ptr->core.cookie(toStdString(url.toString()), toStdString(name)));
}
void HttpSession::setManagingCookies(bool managingCookies)
{
    d_ptr->managingCookies = managingCookies;
    d_ptr->core.setManagingCookies(managingCookies);
}
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
void HttpSession::setDnsCache(QSharedPointer<SocketDnsCache> dnsCache)
{
    d_ptr->core.setDnsCache(dnsCacheCoreOf(dnsCache.data()));
}
QSharedPointer<SocketDnsCache> HttpSession::dnsCache() const
{
    return dnsCacheFromCore(d_ptr->core.dnsCache());
}
QSharedPointer<SocketProxy> HttpSession::socketProxy() const
{
    std::shared_ptr<qtng_core::SocketProxy> core = d_ptr->core.socketProxy();
    if (!core) {
        return QSharedPointer<SocketProxy>();
    }
    if (std::shared_ptr<CoreSocketProxyAdapter> adapter = std::dynamic_pointer_cast<CoreSocketProxyAdapter>(core)) {
        return adapter->proxy;
    }
    return QSharedPointer<SocketProxy>(new QtSocketProxyAdapter(core));
}
void HttpSession::setSocketProxy(QSharedPointer<SocketProxy> proxy)
{
    if (proxy.isNull()) {
        d_ptr->core.setSocketProxy(std::shared_ptr<qtng_core::SocketProxy>());
        return;
    }
    if (QSharedPointer<QtSocketProxyAdapter> adapter = proxy.dynamicCast<QtSocketProxyAdapter>()) {
        d_ptr->core.setSocketProxy(adapter->core);
        return;
    }
    d_ptr->core.setSocketProxy(std::make_shared<CoreSocketProxyAdapter>(proxy));
}
QSharedPointer<HttpProxy> HttpSession::httpProxy() const
{
    std::shared_ptr<qtng_core::HttpProxy> core = d_ptr->core.httpProxy();
    if (!core) {
        return QSharedPointer<HttpProxy>();
    }
    if (std::shared_ptr<CoreHttpProxyAdapter> adapter = std::dynamic_pointer_cast<CoreHttpProxyAdapter>(core)) {
        return adapter->proxy;
    }
    return QSharedPointer<HttpProxy>(new QtHttpProxyAdapter(core));
}
void HttpSession::setHttpProxy(QSharedPointer<HttpProxy> proxy)
{
    if (proxy.isNull()) {
        d_ptr->core.setHttpProxy(std::shared_ptr<qtng_core::HttpProxy>());
        return;
    }
    if (QSharedPointer<QtHttpProxyAdapter> adapter = proxy.dynamicCast<QtHttpProxyAdapter>()) {
        d_ptr->core.setHttpProxy(adapter->core);
        return;
    }
    d_ptr->core.setHttpProxy(std::make_shared<CoreHttpProxyAdapter>(proxy));
}
QSharedPointer<HttpCacheManager> HttpSession::cacheManager() const
{
    std::shared_ptr<qtng_core::HttpCacheManager> core = d_ptr->core.cacheManager();
    if (!core) {
        return QSharedPointer<HttpCacheManager>();
    }
    if (std::shared_ptr<CoreHttpCacheManagerAdapter> adapter = std::dynamic_pointer_cast<CoreHttpCacheManagerAdapter>(core)) {
        return adapter->manager;
    }
    return QSharedPointer<HttpCacheManager>(new QtHttpCacheManagerAdapter(core));
}
void HttpSession::setCacheManager(QSharedPointer<HttpCacheManager> cacheManager)
{
    if (cacheManager.isNull()) {
        d_ptr->core.setCacheManager(std::shared_ptr<qtng_core::HttpCacheManager>());
        return;
    }
    if (QSharedPointer<QtHttpCacheManagerAdapter> adapter = cacheManager.dynamicCast<QtHttpCacheManagerAdapter>()) {
        d_ptr->core.setCacheManager(adapter->core);
        return;
    }
    d_ptr->core.setCacheManager(std::make_shared<CoreHttpCacheManagerAdapter>(cacheManager));
}
WebSocketConfiguration &HttpSession::webSocketConfiguration() { return d_ptr->webSocketConfiguration; }

void setProxySwitcher(HttpSession *session, QSharedPointer<BaseProxySwitcher> switcher)
{
    if (!session) {
        return;
    }
    QSharedPointer<SimpleProxySwitcher> sps = switcher.dynamicCast<SimpleProxySwitcher>();
    if (sps) {
        if (!sps->socketProxies.isEmpty()) {
            session->setSocketProxy(sps->socketProxies.front());
        } else {
            session->setSocketProxy(QSharedPointer<SocketProxy>());
        }
        if (!sps->httpProxies.isEmpty()) {
            session->setHttpProxy(sps->httpProxies.at(0));
        } else {
            session->setHttpProxy(QSharedPointer<HttpProxy>());
        }
    }
}

#ifndef QTNG_NO_CRYPTO
SslConfiguration &HttpSession::sslConfiguration()
{
    return d_ptr->sslConfiguration;
}
#endif

class HttpCacheManagerCoreBridge : public qtng_core::HttpCacheManager
{
public:
    explicit HttpCacheManagerCoreBridge(QTNETWORKNG_NAMESPACE::HttpCacheManager *q)
        : q(q)
    {
    }

    bool store(const std::string &url, const std::string &data) override
    {
        return q->store(toQString(url), toQByteArray(data));
    }

    std::string load(const std::string &url) override
    {
        return toStdString(q->load(toQString(url)));
    }

private:
    QTNETWORKNG_NAMESPACE::HttpCacheManager * const q;
};

class HttpCacheManagerPrivate
{
public:
    explicit HttpCacheManagerPrivate(HttpCacheManager *q)
        : coreBridge(new HttpCacheManagerCoreBridge(q))
    {
    }

    ~HttpCacheManagerPrivate()
    {
        delete coreBridge;
    }

    HttpCacheManagerCoreBridge *coreBridge;
};

HttpCacheManager::HttpCacheManager()
    : d_ptr(new HttpCacheManagerPrivate(this))
{
}

HttpCacheManager::~HttpCacheManager()
{
    delete d_ptr;
}

bool HttpCacheManager::addResponse(HttpResponse &response)
{
    qtng_core::HttpResponse coreResp = toCoreResponse(response);
    return d_ptr->coreBridge->addResponse(coreResp);
}

bool HttpCacheManager::getResponse(HttpResponse *response)
{
    if (!response) {
        return false;
    }
    // Only the URL is needed to look up the cache entry; do not run the full
    // toCoreResponse() here, since it reads body() and would mark the shared
    // core response (and therefore the caller's response) as consumed/errored.
    qtng_core::HttpResponse coreResp;
    coreResp.setUrl(toCoreUrl(response->url()));
    const bool ok = d_ptr->coreBridge->getResponse(&coreResp);
    if (ok) {
        *response = toQtResponse(coreResp);
    }
    return ok;
}

bool HttpCacheManager::store(const QString &url, const QByteArray &data) { Q_UNUSED(url); Q_UNUSED(data); return false; }
QByteArray HttpCacheManager::load(const QString &url) { Q_UNUSED(url); return QByteArray(); }

class HttpMemoryCacheManagerPrivate
{
public:
    QMap<QString, QByteArray> cache;
    float expireTime = 60 * 60 * 24;
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

namespace qtng_bridge {

HttpResponse httpResponseFromCore(qtng_core::HttpResponse core)
{
    return QTNETWORKNG_NAMESPACE::QtHttpBridgeAccess::fromCore(std::move(core));
}

}  // namespace qtng_bridge
