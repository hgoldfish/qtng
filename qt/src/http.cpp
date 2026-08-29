#include <QtCore/qjsondocument.h>
#include <QtCore/qurlquery.h>

#include "bridge/core_access.h"
#include "bridge/http_access.h"
#include "bridge/io_bridge.h"
#include "bridge/socket_access.h"
#include "bridge/ssl_access.h"
#include "bridge/websocket_access.h"
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

// Core error types without a Qt binding counterpart are mapped onto this
// wrapper so the original message survives the bridge instead of collapsing
// into an empty RequestError::what().
class QtGenericRequestError : public RequestError
{
public:
    explicit QtGenericRequestError(const QString &message)
        : message(message)
    {
    }
    QString what() const override { return message; }
private:
    QString message;
};

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

// 桥接 Qt 侧自定义 BaseProxySwitcher 到 core：core 的连接池通过该适配器调用
// Qt 侧 switcher 的 selectSocketProxy/selectHttpProxy，让自定义 switcher 真正
// 参与请求选路（与原版 setProxySwitcher 语义一致）。
class QtProxySwitcherCoreBridge : public qtng_core::BaseProxySwitcher
{
public:
    explicit QtProxySwitcherCoreBridge(QSharedPointer<QTNETWORKNG_NAMESPACE::BaseProxySwitcher> switcher)
        : switcher(std::move(switcher))
    {
    }

    std::shared_ptr<qtng_core::SocketProxy> selectSocketProxy(const std::string &url) override
    {
        if (!switcher) {
            return std::shared_ptr<qtng_core::SocketProxy>();
        }
        const QSharedPointer<SocketProxy> proxy = switcher->selectSocketProxy(toQUrl(qtng_core::utils::Url(url)));
        if (proxy.isNull()) {
            return std::shared_ptr<qtng_core::SocketProxy>();
        }
        return std::make_shared<CoreSocketProxyAdapter>(proxy);
    }

    std::shared_ptr<qtng_core::HttpProxy> selectHttpProxy(const std::string &url) override
    {
        if (!switcher) {
            return std::shared_ptr<qtng_core::HttpProxy>();
        }
        const QSharedPointer<HttpProxy> proxy = switcher->selectHttpProxy(toQUrl(qtng_core::utils::Url(url)));
        if (proxy.isNull()) {
            return std::shared_ptr<qtng_core::HttpProxy>();
        }
        return std::make_shared<CoreHttpProxyAdapter>(proxy);
    }

public:
    QSharedPointer<QTNETWORKNG_NAMESPACE::BaseProxySwitcher> switcher;
};

}  // namespace

class HttpRequestPrivate : public QSharedData
{
public:
    HttpRequestPrivate() = default;
    HttpRequestPrivate(const HttpRequestPrivate &other)
        : core(qtng_core::detail::HttpDeepCopy::request(other.core))
    {
    }
    qtng_core::HttpRequest core;
};

FormData::FormData()
    : boundary_(toQByteArray(qtng_core::FormData::makeBoundary()))
{
}

QByteArray FormData::toByteArray() const
{
    qtng_core::FormData core;
    core.setBoundary(toStdString(boundary()));
    for (const Query &q : queries()) {
        core.addQuery(toStdString(q.name()), toStdString(q.value()));
    }
    for (const File &f : files()) {
        core.addFile(toStdString(f.name()), toStdString(f.filename()), toStdString(f.data()), toStdString(f.contentType()));
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
    const QByteArray contentType = QByteArrayLiteral("multipart/form-data; boundary=") + formData.boundary();
    setHeader(QStringLiteral("Content-Type"), contentType);
    if (!hasHeader(QStringLiteral("MIME-Version"))) {
        setHeader(QStringLiteral("MIME-Version"), QByteArrayLiteral("1.0"));
    }
    setBody(formData.toByteArray());
}
void HttpRequest::setBody(const QJsonDocument &json)
{
    setHeader(QStringLiteral("Content-Type"), QByteArrayLiteral("application/json"));
    setBody(json.toJson(QJsonDocument::Compact));
}
void HttpRequest::setBody(const QJsonObject &json) { setBody(QJsonDocument(json)); }
void HttpRequest::setBody(const QJsonArray &json) { setBody(QJsonDocument(json)); }
void HttpRequest::setBody(const QMap<QString, QString> form) { d->core.setBody(toCoreStringMap(form)); }
void HttpRequest::setBody(const QUrlQuery &form) { d->core.setBody(toCoreUrlQuery(form)); }

void HttpRequest::setContentType(const QString &contentType) { d->core.setContentType(toStdString(contentType)); }
QString HttpRequest::contentType() const { return toQString(d->core.contentType()); }
void HttpRequest::setContentLength(qint64 contentLength) { d->core.setContentLength(contentLength); }
qint64 HttpRequest::contentLength() const { return d->core.contentLength(); }
void HttpRequest::setLocation(const QUrl &url) { d->core.setLocation(toStdString(url.toEncoded(QUrl::FullyEncoded))); }
QUrl HttpRequest::location() const
{
    const QByteArray value = toQByteArray(d->core.location());
    if (value.isEmpty()) {
        return QUrl();
    }
    const QUrl result = QUrl::fromEncoded(value, QUrl::StrictMode);
    return result.isValid() ? result : QUrl();
}
void HttpRequest::setLastModified(const QDateTime &lastModified) { d->core.setLastModified(toCoreDateTime(lastModified)); }
QDateTime HttpRequest::lastModified() const { return toQDateTime(d->core.lastModified()); }
void HttpRequest::setModifiedSince(const QDateTime &modifiedSince) { d->core.setModifiedSince(toCoreDateTime(modifiedSince)); }
QDateTime HttpRequest::modifiedSince() const { return toQDateTime(d->core.modifiedSince()); }

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
QString RequestError::what() const { return QString::fromLatin1("An HTTP error occurred."); }

class HttpResponsePrivate : public QSharedData
{
public:
    HttpResponsePrivate() = default;
    HttpResponsePrivate(const HttpResponsePrivate &other)
        : core(qtng_core::detail::HttpDeepCopy::response(other.core))
    {
    }
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
    coreHistory.reserve(history.size());
    for (const HttpResponse &h : history) {
        coreHistory.push_back(QtHttpBridgeAccess::coreOf(h));
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
QSharedPointer<RequestError> HttpResponse::error() const
{
    shared_ptr<qtng_core::RequestError> coreError = d->core.error();
    if (!coreError) {
        return QSharedPointer<RequestError>();
    }
    // Map core error types onto the Qt binding class hierarchy. Most specific
    // types first: ConnectTimeout/ReadTimeout derive from RequestTimeout and
    // ProxyError/SSLError derive from ConnectionError.
    if (dynamic_pointer_cast<qtng_core::ConnectTimeout>(coreError)) {
        return QSharedPointer<RequestError>(new ConnectTimeout());
    }
    if (dynamic_pointer_cast<qtng_core::ReadTimeout>(coreError)) {
        return QSharedPointer<RequestError>(new ReadTimeout());
    }
    if (dynamic_pointer_cast<class qtng_core::RequestTimeout>(coreError)) {
        // HttpStatus 枚举成员 RequestTimeout(=408) 会遮蔽同名错误类，
        // 需用 elaborated type specifier 引用（与原版 qtnetworkng 一致）。
        return QSharedPointer<RequestError>(new class RequestTimeout());
    }
    if (dynamic_pointer_cast<qtng_core::ProxyError>(coreError)) {
        return QSharedPointer<RequestError>(new ProxyError());
    }
    if (dynamic_pointer_cast<qtng_core::SSLError>(coreError)) {
        return QSharedPointer<RequestError>(new SSLError());
    }
    if (dynamic_pointer_cast<qtng_core::ConnectionError>(coreError)) {
        return QSharedPointer<RequestError>(new ConnectionError());
    }
    shared_ptr<qtng_core::HTTPError> httpError = dynamic_pointer_cast<qtng_core::HTTPError>(coreError);
    if (httpError) {
        return QSharedPointer<RequestError>(new HTTPError(httpError->statusCode()));
    }
    if (dynamic_pointer_cast<qtng_core::URLRequired>(coreError)) {
        return QSharedPointer<RequestError>(new URLRequired());
    }
    if (dynamic_pointer_cast<qtng_core::TooManyRedirects>(coreError)) {
        return QSharedPointer<RequestError>(new TooManyRedirects());
    }
    if (dynamic_pointer_cast<qtng_core::MissingSchema>(coreError)) {
        return QSharedPointer<RequestError>(new MissingSchema());
    }
    if (dynamic_pointer_cast<qtng_core::InvalidScheme>(coreError)) {
        return QSharedPointer<RequestError>(new InvalidScheme());
    }
    if (dynamic_pointer_cast<qtng_core::UnsupportedVersion>(coreError)) {
        return QSharedPointer<RequestError>(new UnsupportedVersion());
    }
    if (dynamic_pointer_cast<qtng_core::InvalidURL>(coreError)) {
        return QSharedPointer<RequestError>(new InvalidURL());
    }
    if (dynamic_pointer_cast<qtng_core::InvalidHeader>(coreError)) {
        return QSharedPointer<RequestError>(new InvalidHeader());
    }
    if (dynamic_pointer_cast<qtng_core::ChunkedEncodingError>(coreError)) {
        return QSharedPointer<RequestError>(new ChunkedEncodingError());
    }
    if (dynamic_pointer_cast<qtng_core::ContentDecodingError>(coreError)) {
        return QSharedPointer<RequestError>(new ContentDecodingError());
    }
    if (dynamic_pointer_cast<qtng_core::StreamConsumedError>(coreError)) {
        return QSharedPointer<RequestError>(new StreamConsumedError());
    }
    if (dynamic_pointer_cast<qtng_core::RetryError>(coreError)) {
        return QSharedPointer<RequestError>(new RetryError());
    }
    if (dynamic_pointer_cast<qtng_core::UnrewindableBodyError>(coreError)) {
        return QSharedPointer<RequestError>(new UnrewindableBodyError());
    }
    return QSharedPointer<RequestError>(new QtGenericRequestError(toQString(coreError->what())));
}

void HttpResponse::setError(QSharedPointer<RequestError> error)
{
    if (!error) {
        d->core.setError(shared_ptr<qtng_core::RequestError>());
        return;
    }
    // Reverse mapping from the Qt binding class hierarchy back to core.
    if (error.dynamicCast<ConnectTimeout>()) {
        d->core.setError(make_shared<qtng_core::ConnectTimeout>());
    } else if (error.dynamicCast<ReadTimeout>()) {
        d->core.setError(make_shared<qtng_core::ReadTimeout>());
    } else if (error.dynamicCast<class RequestTimeout>()) {
        d->core.setError(make_shared<class qtng_core::RequestTimeout>());
    } else if (error.dynamicCast<ProxyError>()) {
        d->core.setError(make_shared<qtng_core::ProxyError>());
    } else if (error.dynamicCast<SSLError>()) {
        d->core.setError(make_shared<qtng_core::SSLError>());
    } else if (error.dynamicCast<ConnectionError>()) {
        d->core.setError(make_shared<qtng_core::ConnectionError>());
    } else if (error.dynamicCast<HTTPError>()) {
        QSharedPointer<HTTPError> httpError = error.dynamicCast<HTTPError>();
        d->core.setError(make_shared<qtng_core::HTTPError>(httpError->statusCode()));
    } else if (error.dynamicCast<URLRequired>()) {
        d->core.setError(make_shared<qtng_core::URLRequired>());
    } else if (error.dynamicCast<TooManyRedirects>()) {
        d->core.setError(make_shared<qtng_core::TooManyRedirects>());
    } else if (error.dynamicCast<MissingSchema>()) {
        d->core.setError(make_shared<qtng_core::MissingSchema>());
    } else if (error.dynamicCast<InvalidScheme>()) {
        d->core.setError(make_shared<qtng_core::InvalidScheme>());
    } else if (error.dynamicCast<UnsupportedVersion>()) {
        d->core.setError(make_shared<qtng_core::UnsupportedVersion>());
    } else if (error.dynamicCast<InvalidURL>()) {
        d->core.setError(make_shared<qtng_core::InvalidURL>());
    } else if (error.dynamicCast<InvalidHeader>()) {
        d->core.setError(make_shared<qtng_core::InvalidHeader>());
    } else if (error.dynamicCast<ChunkedEncodingError>()) {
        d->core.setError(make_shared<qtng_core::ChunkedEncodingError>());
    } else if (error.dynamicCast<ContentDecodingError>()) {
        d->core.setError(make_shared<qtng_core::ContentDecodingError>());
    } else if (error.dynamicCast<StreamConsumedError>()) {
        d->core.setError(make_shared<qtng_core::StreamConsumedError>());
    } else if (error.dynamicCast<RetryError>()) {
        d->core.setError(make_shared<qtng_core::RetryError>());
    } else if (error.dynamicCast<UnrewindableBodyError>()) {
        d->core.setError(make_shared<qtng_core::UnrewindableBodyError>());
    } else {
        d->core.setError(make_shared<qtng_core::RequestError>());
    }
}

void HttpResponse::setContentType(const QString &contentType) { d->core.setContentType(toStdString(contentType)); }
QString HttpResponse::contentType() const { return toQString(d->core.contentType()); }
void HttpResponse::setContentLength(qint64 contentLength) { d->core.setContentLength(contentLength); }
qint64 HttpResponse::contentLength() const { return d->core.contentLength(); }
void HttpResponse::setLocation(const QUrl &url) { d->core.setLocation(toStdString(url.toEncoded(QUrl::FullyEncoded))); }
QUrl HttpResponse::location() const
{
    const QByteArray value = toQByteArray(d->core.location());
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
QDateTime HttpResponse::lastModified() const { return toQDateTime(d->core.lastModified()); }
void HttpResponse::setModifiedSince(const QDateTime &modifiedSince)
{
    d->core.setModifiedSince(toCoreDateTime(modifiedSince));
}
QDateTime HttpResponse::modifiedSince() const { return toQDateTime(d->core.modifiedSince()); }

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
    {
    }

    qtng_core::HttpSession core;
    HttpSession *q_ptr;
    std::shared_ptr<HttpCookieJar> cookieJar;
    std::shared_ptr<WebSocketConfiguration> webSocketConfiguration;
    QSharedPointer<SocketDnsCache> dnsCache;
#ifndef QTNG_NO_CRYPTO
    std::shared_ptr<SslConfiguration> sslConfiguration;
#endif
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
    HttpRequest request;
    request.setMethod(QStringLiteral("POST"));
    request.setUrl(url);
    request.setBody(body);
    return send(request);
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
    HttpRequest request;
    request.setMethod(QStringLiteral("POST"));
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
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

HttpResponse HttpSession::query(const QUrl &url, const QByteArray &body)
{
    return toQtResponse(d_ptr->core.query(toCoreUrl(url).toString(), toStdString(body)));
}

HttpResponse HttpSession::query(const QUrl &url, QSharedPointer<FileLike> body)
{
    return toQtResponse(d_ptr->core.query(toCoreUrl(url).toString(), toCoreFileLike(body)));
}

HttpResponse HttpSession::query(const QUrl &url, const QMap<QString, QString> &body)
{
    return toQtResponse(d_ptr->core.query(toCoreUrl(url).toString(), toCoreStringMap(body)));
}

HttpResponse HttpSession::query(const QUrl &url, const QUrlQuery &body)
{
    return toQtResponse(d_ptr->core.query(toCoreUrl(url).toString(), toCoreUrlQuery(body)));
}

HttpResponse HttpSession::query(const QUrl &url, const QJsonDocument &body)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("QUERY"));
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::query(const QUrl &url, const QJsonObject &body)
{
    return query(url, QJsonDocument(body));
}

HttpResponse HttpSession::query(const QUrl &url, const QJsonArray &body)
{
    return query(url, QJsonDocument(body));
}

HttpResponse HttpSession::query(const QUrl &url, const FormData &body)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("QUERY"));
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::query(const QUrl &url, const QByteArray &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.query(toCoreUrl(url).toString(), toStdString(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::query(const QUrl &url, QSharedPointer<FileLike> body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.query(toCoreUrl(url).toString(), toCoreFileLike(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::query(const QUrl &url, const QMap<QString, QString> &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.query(toCoreUrl(url).toString(), toCoreStringMap(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::query(const QUrl &url, const QUrlQuery &body, const QMap<QString, QByteArray> &headers)
{
    return toQtResponse(d_ptr->core.query(toCoreUrl(url).toString(), toCoreUrlQuery(body), toCoreHeaderMap(headers)));
}

HttpResponse HttpSession::query(const QUrl &url, const QJsonDocument &body, const QMap<QString, QByteArray> &headers)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("QUERY"));
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::query(const QUrl &url, const QJsonObject &body, const QMap<QString, QByteArray> &headers)
{
    return query(url, QJsonDocument(body), headers);
}

HttpResponse HttpSession::query(const QUrl &url, const QJsonArray &body, const QMap<QString, QByteArray> &headers)
{
    return query(url, QJsonDocument(body), headers);
}

HttpResponse HttpSession::query(const QUrl &url, const FormData &body, const QMap<QString, QByteArray> &headers)
{
    HttpRequest request;
    request.setMethod(QStringLiteral("QUERY"));
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::query(const QString &url, const QByteArray &body)
{
    return query(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::query(const QString &url, QSharedPointer<FileLike> body)
{
    return query(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::query(const QString &url, const QMap<QString, QString> &body)
{
    return query(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::query(const QString &url, const QUrlQuery &body)
{
    return query(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::query(const QString &url, const QJsonDocument &body)
{
    return query(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::query(const QString &url, const QJsonObject &body)
{
    return query(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::query(const QString &url, const QJsonArray &body)
{
    return query(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::query(const QString &url, const FormData &body)
{
    return query(QUrl::fromUserInput(url), body);
}

HttpResponse HttpSession::query(const QString &url, const FormData &body, const QMap<QString, QByteArray> &headers)
{
    return query(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::query(const QString &url, const QByteArray &body, const QMap<QString, QByteArray> &headers)
{
    return query(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::query(const QString &url, QSharedPointer<FileLike> body, const QMap<QString, QByteArray> &headers)
{
    return query(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::query(const QString &url, const QMap<QString, QString> &body, const QMap<QString, QByteArray> &headers)
{
    return query(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::query(const QString &url, const QUrlQuery &body, const QMap<QString, QByteArray> &headers)
{
    return query(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::query(const QString &url, const QJsonDocument &body, const QMap<QString, QByteArray> &headers)
{
    return query(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::query(const QString &url, const QJsonObject &body, const QMap<QString, QByteArray> &headers)
{
    return query(QUrl::fromUserInput(url), body, headers);
}

HttpResponse HttpSession::query(const QString &url, const QJsonArray &body, const QMap<QString, QByteArray> &headers)
{
    return query(QUrl::fromUserInput(url), body, headers);
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
    HttpRequest request;
    request.setMethod(QStringLiteral("PUT"));
    request.setUrl(url);
    request.setBody(body);
    return send(request);
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
    HttpRequest request;
    request.setMethod(QStringLiteral("PUT"));
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
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
    HttpRequest request;
    request.setMethod(QStringLiteral("PATCH"));
    request.setUrl(url);
    request.setBody(body);
    return send(request);
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
    HttpRequest request;
    request.setMethod(QStringLiteral("PATCH"));
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
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

QSharedPointer<WebSocketConnection> HttpSession::ws(const QUrl &url)
{
    return webSocketConnectionFromCore(d_ptr->core.ws(toCoreUrl(url).toString()));
}

QSharedPointer<WebSocketConnection> HttpSession::ws(const QUrl &url, const QMap<QString, QString> &query)
{
    return webSocketConnectionFromCore(d_ptr->core.ws(toCoreUrl(url).toString(), toCoreStringMap(query)));
}

QSharedPointer<WebSocketConnection> HttpSession::ws(const QUrl &url, const QMap<QString, QString> &query,
                                                    const QMap<QString, QByteArray> &headers)
{
    return webSocketConnectionFromCore(
            d_ptr->core.ws(toCoreUrl(url).toString(), toCoreStringMap(query), toCoreHeaderMap(headers)));
}

QSharedPointer<WebSocketConnection> HttpSession::ws(const QUrl &url, const QUrlQuery &query)
{
    return webSocketConnectionFromCore(d_ptr->core.ws(toCoreUrl(url).toString(), toCoreUrlQuery(query)));
}

QSharedPointer<WebSocketConnection> HttpSession::ws(const QUrl &url, const QUrlQuery &query,
                                                    const QMap<QString, QByteArray> &headers)
{
    return webSocketConnectionFromCore(
            d_ptr->core.ws(toCoreUrl(url).toString(), toCoreUrlQuery(query), toCoreHeaderMap(headers)));
}

QSharedPointer<WebSocketConnection> HttpSession::ws(const QString &url)
{
    return ws(QUrl::fromUserInput(url));
}

QSharedPointer<WebSocketConnection> HttpSession::ws(const QString &url, const QMap<QString, QString> &query)
{
    return ws(QUrl::fromUserInput(url), query);
}

QSharedPointer<WebSocketConnection> HttpSession::ws(const QString &url, const QMap<QString, QString> &query,
                                                    const QMap<QString, QByteArray> &headers)
{
    return ws(QUrl::fromUserInput(url), query, headers);
}

QSharedPointer<WebSocketConnection> HttpSession::ws(const QString &url, const QUrlQuery &query)
{
    return ws(QUrl::fromUserInput(url), query);
}

QSharedPointer<WebSocketConnection> HttpSession::ws(const QString &url, const QUrlQuery &query,
                                                    const QMap<QString, QByteArray> &headers)
{
    return ws(QUrl::fromUserInput(url), query, headers);
}

std::shared_ptr<HttpCookieJar> HttpSession::cookieJar()
{
    if (!d_ptr->cookieJar) {
        d_ptr->cookieJar = httpCookieJarFromCore(d_ptr->core.cookieJar());
    }
    return d_ptr->cookieJar;
}
HttpCookie HttpSession::cookie(const QUrl &url, const QString &name)
{
    return toQtCookie(d_ptr->core.cookie(toStdString(url.toString()), toStdString(name)));
}
void HttpSession::setManagingCookies(bool managingCookies)
{
    d_ptr->core.setManagingCookies(managingCookies);
}
void HttpSession::setMaxConnectionsPerServer(int maxConnectionsPerServer) { d_ptr->core.setMaxConnectionsPerServer(maxConnectionsPerServer); }
int HttpSession::maxConnectionsPerServer() { return d_ptr->core.maxConnectionsPerServer(); }
void HttpSession::setDebugLevel(int level) { d_ptr->core.setDebugLevel(level); }
void HttpSession::disableDebug() { setDebugLevel(0); }
void HttpSession::setKeepAlive(bool keepAlive) { d_ptr->core.setKeepAlive(keepAlive); }
bool HttpSession::keepAlive() const { return d_ptr->core.keepAlive(); }
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
    d_ptr->dnsCache = dnsCache;
}
QSharedPointer<SocketDnsCache> HttpSession::dnsCache() const
{
    if (d_ptr->dnsCache.isNull()) {
        d_ptr->dnsCache = dnsCacheFromCore(d_ptr->core.dnsCache());
    }
    return d_ptr->dnsCache;
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
std::shared_ptr<WebSocketConfiguration> HttpSession::webSocketConfiguration()
{
    if (!d_ptr->webSocketConfiguration) {
        d_ptr->webSocketConfiguration =
                webSocketConfigurationFromCore(d_ptr->core.webSocketConfiguration());
    }
    return d_ptr->webSocketConfiguration;
}

void HttpSession::setWebSocketConfiguration(const std::shared_ptr<WebSocketConfiguration> &configuration)
{
    d_ptr->core.setWebSocketConfiguration(webSocketConfigurationToCore(configuration));
    d_ptr->webSocketConfiguration = configuration;
}

void setProxySwitcher(HttpSession *session, QSharedPointer<BaseProxySwitcher> switcher)
{
    if (!session) {
        return;
    }
    QSharedPointer<SimpleProxySwitcher> sps = switcher.dynamicCast<SimpleProxySwitcher>();
    if (sps) {
        // SimpleProxySwitcher：把代理复制到会话（写入 core 的 SimpleProxySwitcher 列表），
        // 使 httpProxy()/socketProxy() 等查询接口保持可用。
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
        return;
    }
    // 自定义 BaseProxySwitcher：真正安装到 core 会话，参与连接池的请求选路
    // （原版语义）。null 时恢复为全新的 SimpleProxySwitcher。
    if (switcher.isNull()) {
        qtng_core::setProxySwitcher(&session->d_ptr->core, std::shared_ptr<qtng_core::BaseProxySwitcher>());
    } else {
        qtng_core::setProxySwitcher(&session->d_ptr->core, std::make_shared<QtProxySwitcherCoreBridge>(switcher));
    }
}

#ifndef QTNG_NO_CRYPTO
std::shared_ptr<SslConfiguration> HttpSession::sslConfiguration()
{
    if (!d_ptr->sslConfiguration) {
        d_ptr->sslConfiguration = sslConfigurationFromCore(d_ptr->core.sslConfiguration());
    }
    return d_ptr->sslConfiguration;
}

void HttpSession::setSslConfiguration(const std::shared_ptr<SslConfiguration> &configuration)
{
    d_ptr->core.setSslConfiguration(sslConfigurationToCore(configuration));
    d_ptr->sslConfiguration = configuration;
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

QString HTTPError::what() const { return QString::fromLatin1("server respond error. httpCode:%1").arg(statusCode()); }
QString ConnectionError::what() const { return QString::fromLatin1("A Connection error occurred."); }
QString ProxyError::what() const { return QString::fromLatin1("A proxy error occurred."); }
QString SSLError::what() const { return QString::fromLatin1("A SSL error occurred."); }
QString RequestTimeout::what() const { return QString::fromLatin1("The request timed out."); }
QString ConnectTimeout::what() const { return QString::fromLatin1("The request timed out while trying to connect to the remote server."); }
QString ReadTimeout::what() const { return QString::fromLatin1("The server did not send any data in the allotted amount of time."); }
QString URLRequired::what() const { return QString::fromLatin1("A valid URL is required to make a request."); }
QString TooManyRedirects::what() const { return QString::fromLatin1("Too many redirects."); }
QString MissingSchema::what() const { return QString::fromLatin1("The URL schema (e.g. http or https) is missing."); }
QString InvalidScheme::what() const { return QString::fromLatin1("The URL schema can not be handled."); }
QString UnsupportedVersion::what() const { return QString::fromLatin1("The HTTP version is not supported yet."); }
QString InvalidURL::what() const { return QString::fromLatin1("The URL provided was somehow invalid."); }
QString InvalidHeader::what() const { return QString::fromLatin1("Can not parse the http header."); }
QString ChunkedEncodingError::what() const { return QString::fromLatin1("The server declared chunked encoding but sent an invalid chunk."); }
QString ContentDecodingError::what() const { return QString::fromLatin1("Failed to decode response content"); }
QString StreamConsumedError::what() const { return QString::fromLatin1("The content for this response was already consumed"); }
QString RetryError::what() const { return QString::fromLatin1("Custom retries logic failed"); }
QString UnrewindableBodyError::what() const { return QString::fromLatin1("Requests encountered an error when trying to rewind a body"); }

}  // namespace QTNETWORKNG_NAMESPACE

namespace qtng_bridge {

HttpResponse httpResponseFromCore(qtng_core::HttpResponse core)
{
    return QTNETWORKNG_NAMESPACE::QtHttpBridgeAccess::fromCore(std::move(core));
}

}  // namespace qtng_bridge
