#ifdef QTNG_HAVE_ZLIB
#include "qtng/gzip.h"
#endif
#ifndef QTNG_NO_CRYPTO
#include "qtng/ssl.h"
#endif

#include "qtng/http.h"
#include "qtng/socks5_proxy.h"
#include "qtng/random.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/random.h"
#include "qtng/md.h"
#include "qtng/io_utils.h"
#include "qtng/utils/url.h"
#include "qtng/msgpack.h"
#include "qtng/utils/logging.h"
#include "qtng/private/http_p.h"

using namespace std;

NG_LOGGER("qtng.http");

namespace qtng {

static int urlDefaultPort(const utils::Url &url)
{
    if (url.port() >= 0) {
        return url.port();
    }
    if (url.scheme() == "https" || url.scheme() == "wss") {
        return 443;
    }
    return 80;
}

static string urlResourcePath(const utils::Url &url)
{
    string path = url.path();
    if (!url.query().empty()) {
        path += '?' + url.query();
    }
    return path;
}

static utils::Url resolveRelativeUrl(const utils::Url &base, const string &relative)
{
    if (relative.find("://") != string::npos) {
        return utils::Url(relative);
    }
    if (!relative.empty() && relative[0] == '/') {
        utils::Url resolved = base;
        resolved.setPath(relative);
        resolved.setFragment("");
        return resolved;
    }
    utils::Url resolved = base;
    string path = base.path();
    if (!path.empty() && path.back() != '/') {
        const size_t slash = path.rfind('/');
        path = slash != string::npos ? path.substr(0, slash + 1) : "/";
    }
    path += relative;
    resolved.setPath(path);
    return resolved;
}

static vector<string> splitWhitespace(const string &text)
{
    vector<string> parts;
    string current;
    for (char ch : text) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

static string joinWithSeparator(char sep, const vector<string> &parts, size_t start = 0)
{
    string result;
    for (size_t i = start; i < parts.size(); ++i) {
        if (i > start) {
            result.push_back(sep);
        }
        result += parts[i];
    }
    return result;
}

static inline string joinLines(const vector<string> &lines)
{
    string buf;
    buf.reserve(1024 * 4);
    for (const string &line : lines) {
        buf += line;
    }
    return buf;
}

inline MsgPackStream &operator<<(MsgPackStream &ds, const HttpHeader &header)
{
    return ds << header.name << header.value;
}

inline MsgPackStream &operator>>(MsgPackStream &ds, HttpHeader &header)
{
    return ds >> header.name >> header.value;
}

FormData::FormData()
{
    const string possibleCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    const int randomPartLength = 16;

    string randomPart;
    for (int i = 0; i < randomPartLength; ++i) {
        int index = static_cast<int>(utils::RandomGenerator::global().bounded(static_cast<uint32_t>(possibleCharacters.size())));
        char nextChar = possibleCharacters[static_cast<size_t>(index)];
        randomPart.push_back(nextChar);
    }

    boundary = string("----WebKitFormBoundary") + randomPart;
}

string formatHeaderParam(const string &name, const string &value)
{
    return name + "=\"" + value + "\"";
}

string FormData::toByteArray() const
{
    string body;
    for (vector<FormData::Query>::const_iterator itor = queries.cbegin(); itor != queries.cend(); ++itor) {
        body.append("--");
        body.append(boundary);
        body.append("\r\n");
        body.append("Content-Disposition: form-data;");
        body.append(formatHeaderParam("name", itor->name));
        body.append("\r\n\r\n");
        body.append(itor->value);
        body.append("\r\n");
    }
    for (vector<FormData::File>::const_iterator itor = files.cbegin(); itor != files.cend(); ++itor) {
        body.append("--");
        body.append(boundary);
        body.append("\r\n");
        body.append("Content-Disposition: form-data;");
        body.append(formatHeaderParam("name", itor->name));
        body.append("; ");
        body.append(formatHeaderParam("filename", itor->filename));
        body.append("\r\n");
        body.append("Content-Type: ");
        body.append(itor->contentType);
        body.append("\r\n\r\n");
        body.append(itor->data);
        body.append("\r\n");
    }
    body.append("--");
    body.append(boundary);
    body.append("--");
    return body;
}

class HttpRequestPrivate
{
public:
    HttpRequestPrivate();
    ~HttpRequestPrivate();
    HttpRequestPrivate(const HttpRequestPrivate &other);
public:
    shared_ptr<SocketLike> connection;
    string method;
    utils::Url url;
    utils::UrlQuery query;
    vector<HttpCookie> cookies;
    shared_ptr<FileLike> body;
    string userAgent;
    int64_t maxBodySize;
    int maxRedirects;
    float connectionTimeout;
    float timeout;
    HttpRequest::Priority priority;
    HttpVersion version;
    bool streamResponse;
    bool isWebSocket;
};

HttpRequestPrivate::HttpRequestPrivate()
    : method("GET")
    , maxBodySize(-1)
    , maxRedirects(8)
    , connectionTimeout(-1.0)
    , timeout(-1.0)
    , priority(HttpRequest::NormalPriority)
    , version(Unknown)
    , streamResponse(false)
    , isWebSocket(false)
{
}

HttpRequestPrivate::~HttpRequestPrivate() { }

HttpRequestPrivate::HttpRequestPrivate(const HttpRequestPrivate &other)
    : connection(other.connection)
    , method(other.method)
    , url(other.url)
    , query(other.query)
    , cookies(other.cookies)
    , body(other.body)
    , userAgent(other.userAgent)
    , maxBodySize(other.maxBodySize)
    , maxRedirects(other.maxRedirects)
    , connectionTimeout(other.connectionTimeout)
    , timeout(other.timeout)
    , priority(other.priority)
    , version(other.version)
    , streamResponse(other.streamResponse)
    , isWebSocket(other.isWebSocket)
{
}

HttpRequest::HttpRequest()
    : d(new HttpRequestPrivate())
{
}

HttpRequest::~HttpRequest() { }

HttpRequest::HttpRequest(const HttpRequest &other)
    : d(other.d)
{
    this->headers = other.headers;
}

HttpRequest::HttpRequest(HttpRequest &&other)
{
    swap(d, other.d);
    headers = std::move(other.headers);
}

HttpRequest &HttpRequest::operator=(const HttpRequest &other)
{
    this->headers = other.headers;
    this->d = other.d;
    return *this;
}

string HttpRequest::method() const
{
    return d->method;
}

void HttpRequest::setMethod(const string &method)
{
    d->method = method;
}

utils::Url HttpRequest::url() const
{
    return d->url;
}

void HttpRequest::setUrl(const utils::Url &url)
{
    d->url = url;
}

utils::UrlQuery HttpRequest::query() const
{
    return d->query;
}

void HttpRequest::setQuery(const map<string, string> &query)
{
    d->query.clear();
    for (map<string, string>::const_iterator itor = query.cbegin(); itor != query.cend(); ++itor) {
        d->query.addQueryItem(itor->first, itor->second);
    }
}

void HttpRequest::setQuery(const utils::UrlQuery &query)
{
    d->query = query;
}

vector<HttpCookie> HttpRequest::cookies() const
{
    return d->cookies;
}

void HttpRequest::setCookies(const vector<HttpCookie> &cookies)
{
    d->cookies = cookies;
}

shared_ptr<FileLike> HttpRequest::body() const
{
    return d->body;
}

void HttpRequest::setBody(const string &body)
{
    d->body = FileLike::bytes(body);
}

void HttpRequest::setBody(shared_ptr<FileLike> body)
{
    d->body = body;
}

string HttpRequest::userAgent() const
{
    return d->userAgent;
}

void HttpRequest::setUserAgent(const string &userAgent)
{
    d->userAgent = userAgent;
}

int64_t HttpRequest::maxBodySize() const
{
    return d->maxBodySize;
}

void HttpRequest::setMaxBodySize(int64_t maxBodySize)
{
    d->maxBodySize = maxBodySize;
}

int HttpRequest::maxRedirects() const
{
    return d->maxRedirects;
}

void HttpRequest::setMaxRedirects(int maxRedirects)
{
    d->maxRedirects = maxRedirects;
}

HttpRequest::Priority HttpRequest::priority() const
{
    return d->priority;
}

void HttpRequest::setPriority(HttpRequest::Priority priority)
{
    d->priority = priority;
}

HttpVersion HttpRequest::version() const
{
    return d->version;
}

void HttpRequest::setVersion(HttpVersion version)
{
    d->version = version;
}

void HttpRequest::setStreamResponse(bool streamResponse)
{
    d->streamResponse = streamResponse;
}

bool HttpRequest::streamResponse() const
{
    return d->streamResponse;
}

float HttpRequest::connectionTimeout() const
{
    return d->connectionTimeout;
}

void HttpRequest::setConnectionTimeout(float connectionTimeout)
{
    d->connectionTimeout = connectionTimeout;
}

float HttpRequest::timeout() const
{
    return d->timeout;
}

void HttpRequest::setTimeout(float timeout)
{
    d->timeout = timeout;
}

shared_ptr<SocketLike> HttpRequest::connection() const
{
    return d->connection;
}

void HttpRequest::useConnection(shared_ptr<SocketLike> connection)
{
    d->maxRedirects = 0;
    d->connection = connection;
}

void HttpRequest::setBody(const FormData &formData)
{
    string contentType =
            utils::formatMessage("multipart/form-data; boundary=%1", {formData.boundary});
    setHeader("Content-Type", contentType);
    const string mimeHeader("MIME-Version");
    if (!hasHeader(mimeHeader)) {
        setHeader(mimeHeader, string("1.0"));
    }
    setBody(formData.toByteArray());
}

void HttpRequest::setBody(const map<string, string> form)
{
    utils::UrlQuery query;
    for (map<string, string>::const_iterator itor = form.cbegin(); itor != form.cend(); ++itor) {
        query.addQueryItem(itor->first, itor->second);
    }
    setBody(query);
}

void HttpRequest::setBody(const utils::UrlQuery &form)
{
    setHeader("Content-Type", string("application/x-www-form-urlencoded"));
    setBody(form.toString());
}

class HttpResponsePrivate
{
public:
    HttpResponsePrivate();
    ~HttpResponsePrivate();
    HttpResponsePrivate(const HttpResponsePrivate &other);
public:
    utils::Url url;
    string statusText;
    vector<HttpCookie> cookies;
    HttpRequest request;
    string body;
    vector<HttpResponse> history;
    shared_ptr<RequestError> error;
    shared_ptr<SocketLike> stream;
    int64_t elapsed;
    int statusCode;
    HttpVersion version;
    bool consumed;
};

HttpResponsePrivate::HttpResponsePrivate()
    : elapsed(0)
    , statusCode(0)
    , version(Http1_1)
    , consumed(false)
{
}

HttpResponsePrivate::~HttpResponsePrivate() { }

HttpResponsePrivate::HttpResponsePrivate(const HttpResponsePrivate &other)
    : url(other.url)
    , statusText(other.statusText)
    , cookies(other.cookies)
    , request(other.request)
    , body(other.body)
    , history(other.history)
    , error(other.error)
    , stream(other.stream)
    , elapsed(other.elapsed)
    , statusCode(other.statusCode)
    , version(other.version)
    , consumed(other.consumed)
{
}

HttpResponse::HttpResponse()
    : d(new HttpResponsePrivate())
{
}

HttpResponse::~HttpResponse() { }

HttpResponse::HttpResponse(const HttpResponse &other)
    : d(other.d)
{
    this->headers = other.headers;
}

HttpResponse::HttpResponse(HttpResponse &&other)
{
    swap(d, other.d);
    headers = std::move(other.headers);
}

HttpResponse &HttpResponse::operator=(const HttpResponse &other)
{
    d = other.d;
    headers = other.headers;
    return *this;
}

utils::Url HttpResponse::url() const
{
    return d->url;
}

void HttpResponse::setUrl(const utils::Url &url)
{
    d->url = url;
}

int HttpResponse::statusCode() const
{
    return d->statusCode;
}

void HttpResponse::setStatusCode(int statusCode)
{
    d->statusCode = statusCode;
}

string HttpResponse::statusText() const
{
    return d->statusText;
}

void HttpResponse::setStatusText(const string &statusText)
{
    d->statusText = statusText;
}

vector<HttpCookie> HttpResponse::cookies() const
{
    return d->cookies;
}

void HttpResponse::setCookies(const vector<HttpCookie> &cookies)
{
    d->cookies = cookies;
}

HttpRequest HttpResponse::request() const
{
    return d->request;
}

void HttpResponse::setRequest(const HttpRequest &request)
{
    d->request = request;
}

shared_ptr<SocketLike> HttpResponse::takeStream(string *readBytes)
{
    if (d->consumed) {
        ngWarning()
                << "the stream is consumed. do you remember to set the streamResponse property of request to true?";
    }
    d->consumed = true;
    if (readBytes) {
        *readBytes = d->body;
        d->body.clear();
    } else if (!d->body.empty()) {
        ngWarning() << "you should take care the left bytes after parsing header. please pass a non-null byte array "
                        "to takeStream():"
                     << d->body.size();
    }
    return d->stream;
}

RequestError *toRequestError(ChunkedBlockReader::Error error)
{
    switch (error) {
    case ChunkedBlockReader::ChunkedEncodingError:
        return new ChunkedEncodingError();
    case ChunkedBlockReader::UnrewindableBodyError:
        return new UnrewindableBodyError();
    case ChunkedBlockReader::ConnectionError:
        return new ConnectionError();
    default:
        return nullptr;
    }
}

shared_ptr<FileLike> HttpResponse::bodyAsFile(bool processGzip, bool processChunked)
{
    if (d->consumed) {
        ngWarning()
                << "the stream is consumed. do you remember to set the streamResponse property of request to true?";
        return shared_ptr<FileLike>();
    }
    d->consumed = true;
    if (d->error && !hasHttpError()) {
        ngWarning() << "the response has error, there is no avaliable body file.";
        return shared_ptr<FileLike>();
    }

    // XXX if not consumed and body is not empty, it must be from the header splitter.
    // read it from stream
    int64_t contentLength = getContentLength();
    shared_ptr<FileLike> bodyFile;
    if (contentLength >= 0) {
        if (contentLength > INT_MAX || (d->request.maxBodySize() >= 0 && contentLength > d->request.maxBodySize())) {
            setError(new UnrewindableBodyError());
            return shared_ptr<FileLike>();
        } else {
            if (d->body.size() > contentLength) {
                ngWarning() << "response body got too much bytes.";
                bodyFile = FileLike::bytes(d->body);
            } else if (d->body.size() < contentLength) {
                if (!d->stream) {
                    setError(new UnrewindableBodyError());
                    return shared_ptr<FileLike>();
                }
                bodyFile = make_shared<PlainBodyFile>(contentLength, d->body, d->stream);
            } else {
                bodyFile = FileLike::bytes(d->body);
            }
        }
    } else {  // if (contentLength < 0) without `Content-Length` header.
        if (!d->stream) {
            setError(new UnrewindableBodyError());
            return shared_ptr<FileLike>();
        }
        const string &transferEncodingHeader = header("Transfer-Encoding");
        bool isChunked = (transferEncodingHeader == string("chunked"));
        if (isChunked && processChunked) {
            removeHeader("Transfer-Encoding");
            bodyFile = make_shared<ChunkedBodyFile>(d->request.maxBodySize(), d->body, d->stream);
        } else {
            bodyFile = make_shared<PlainBodyFile>(contentLength, d->body, d->stream);
        }
    }
    d->body.clear();

    if (processGzip) {
        const string &contentEncodingHeader = header("Content-Encoding");
        const string &transferEncodingHeader = header("Transfer-Encoding");
#ifdef QTNG_HAVE_ZLIB
        if (contentEncodingHeader == string("gzip")
            || contentEncodingHeader == string("deflate")) {
            removeHeader("Content-Encoding");
            bodyFile = make_shared<GzipFile>(bodyFile, GzipFile::Decompress);
        } else if (transferEncodingHeader == string("gzip")
                   || transferEncodingHeader == string("deflate")) {
            removeHeader("Transfer-Encoding");
            bodyFile = make_shared<GzipFile>(bodyFile, GzipFile::Decompress);
        } else
#endif
                if (!contentEncodingHeader.empty() || !transferEncodingHeader.empty()) {
            ngWarning() << "unsupported content encoding:" << contentEncodingHeader << transferEncodingHeader;
        }
    }
    return bodyFile;
}

string HttpResponse::body()
{
    if (d->consumed) {
        return d->body;
    }
    shared_ptr<FileLike> bodyFile = this->bodyAsFile();
    if (!bodyFile) {
        return string();
    }
    bool ok;
    const string &data = bodyFile->readall(&ok);
    if (!ok) {
#ifdef QTNG_HAVE_ZLIB
        if (dynamic_pointer_cast<GzipFile>(bodyFile)) {
            setError(new ContentDecodingError());
        } else
#endif
                if (dynamic_pointer_cast<ChunkedBodyFile>(bodyFile)) {
            RequestError *error = nullptr;
            error = toRequestError(dynamic_pointer_cast<ChunkedBodyFile>(bodyFile)->error);
            if (error) {
                setError(error);
            } else {
                setError(new ConnectionError());
            }
        } else {
            setError(new ConnectionError());
        }
    }
    d->body = data;
    return data;
}

void HttpResponse::setBody(const string &body)
{
    d->body = body;
    d->consumed = true;
}

int64_t HttpResponse::elapsed() const
{
    return d->elapsed;
}

void HttpResponse::setElapsed(int64_t elapsed)
{
    d->elapsed = elapsed;
}

vector<HttpResponse> HttpResponse::history() const
{
    return d->history;
}

void HttpResponse::setHistory(const vector<HttpResponse> &history)
{
    d->history = history;
}

HttpVersion HttpResponse::version() const
{
    return d->version;
}

void HttpResponse::setVersion(HttpVersion version)
{
    d->version = version;
}

string HttpResponse::text()
{
    return body();
}

string HttpResponse::html()
{
    // TODO detect encoding;
    return body();
}

bool HttpResponse::isOk() const
{
    return !d->error && d->statusCode < 400;
}

bool HttpResponse::hasNetworkError() const
{
    return !!d->error && dynamic_pointer_cast<ConnectionError>(d->error) != nullptr;
}

bool HttpResponse::hasHttpError() const
{
    return !!d->error && dynamic_pointer_cast<HTTPError>(d->error) != nullptr;
}

shared_ptr<RequestError> HttpResponse::error() const
{
    return d->error;
}

void HttpResponse::setError(shared_ptr<RequestError> error)
{
    d->error = error;
}

HttpSessionPrivate::HttpSessionPrivate(HttpSession *q_ptr)
    : defaultVersion(HttpVersion::Http1_1)
    , q_ptr(q_ptr)
    , debugLevel(0)
    , managingCookies(true)
    , keepAlive(true)
{
    defaultUserAgent = "Mozilla/5.0 (X11; Linux x86_64; rv:52.0) Gecko/20100101 Firefox/52.0";
}

HttpSessionPrivate::~HttpSessionPrivate() { }

static utils::Url hostOnly(const utils::Url &url)
{
    utils::Url h;
    h.setScheme(url.scheme());
    h.setHost(url.host());
    h.setPort(url.port());
    return h;
}

ConnectionPool::ConnectionPool()
    : dnsCache(new SocketDnsCache)
    , proxySwitcher(new SimpleProxySwitcher)
    , maxConnectionsPerServer(5)
    , timeToLive(60)
    , defaultConnectionTimeout(10.0)
    , defaultTimeout(20.0)
    , operations(new CoroutineGroup)
{
    operations->spawnWithName("removeUnusedConnections", [this] { removeUnusedConnections(); });
}

ConnectionPool::~ConnectionPool()
{
    delete operations;
}

shared_ptr<ConnectionPoolItem> ConnectionPool::getItem(const string &url)
{
    const string &h = hostOnly(utils::Url(url)).toString();
    shared_ptr<ConnectionPoolItem> &item = items[h];
    if (!item) {
        item.reset(new ConnectionPoolItem());
    }
    item->lastUsed = utils::DateTime::currentDateTimeUtc();
    if (!item->semaphore) {
        item->semaphore.reset(new Semaphore(maxConnectionsPerServer));
    }
    return item;
}

shared_ptr<Semaphore> ConnectionPool::getSemaphore(const string &url)
{
    shared_ptr<ConnectionPoolItem> item = getItem(url);
    return item->semaphore;
}

void ConnectionPool::recycle(const string &url, shared_ptr<SocketLike> connection)
{
    shared_ptr<ConnectionPoolItem> item = getItem(url);
    if (item->connections.size() < maxConnectionsPerServer) {
        item->connections.push_back(connection);
    }
}

shared_ptr<SocketLike> ConnectionPool::oldConnectionForUrl(const string &url)
{
    shared_ptr<ConnectionPoolItem> item = getItem(url);

    while (!item->connections.empty()) {
        shared_ptr<SocketLike> connection = item->connections.front();
        item->connections.erase(item->connections.begin());
        if (!connection->isValid()) {
            continue;
        }

        char tbuf;
        // should i use `peek()`?
        if (connection->peekRaw(&tbuf, 1) >= 0) {
            // ngDebug() << "reuse connect" << connection->localPort();
            return connection;
            //} else {
            //    ngDebug() << "abandon connect" << connection->localPort();
        }
    }
    return shared_ptr<SocketLike>();
}

shared_ptr<SocketLike> ConnectionPool::newConnectionForUrl(const string &urlStr, RequestError **error)
{
    utils::Url url(urlStr);
    shared_ptr<SocketLike> connection;
    const uint16_t port = static_cast<uint16_t>(urlDefaultPort(url));

    shared_ptr<SocketProxy> socketProxy = proxySwitcher->selectSocketProxy(urlStr);
    if (socketProxy) {
        try {
            connection = socketProxy->connect(url.host(), port);
        } catch (Socks5Exception &) {
            // handle error on next.
        }
        if (!connection || !connection->isValid()) {
            *error = new ConnectionError();
            return shared_ptr<SocketLike>();
        }
    } else {
        shared_ptr<Socket> rawSocket;
        rawSocket.reset(Socket::createConnection(url.host(), port, nullptr, dnsCache));
        if (!rawSocket) {
            *error = new ConnectionError();
            return shared_ptr<SocketLike>();
        }

        if (!rawSocket || !rawSocket->isValid()) {
            *error = new ConnectionError();
            return shared_ptr<SocketLike>();
        }
        connection = asSocketLike(rawSocket);
    }

    if (url.scheme() == "https" || url.scheme() == "wss") {
#ifndef QTNG_NO_CRYPTO
        shared_ptr<SslSocket> ssl(new SslSocket(connection, sslConfig));
        if (!ssl->handshake(false, url.host())) {
            *error = new ConnectionError();
            return shared_ptr<SocketLike>();
        }
        connection = asSocketLike(ssl);
#else
        *error = new ConnectionError();
        return shared_ptr<SocketLike>();
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
        const utils::DateTime &now = utils::DateTime::currentDateTimeUtc();
        map<string, shared_ptr<ConnectionPoolItem>> newItems;
        for (map<string, shared_ptr<ConnectionPoolItem>>::const_iterator itor = items.cbegin(); itor != items.cend();
             ++itor) {
            if (itor->second->lastUsed.secsTo(now) < timeToLive || itor->second->semaphore->isUsed()) {
                newItems[itor->first] = itor->second;
            }
        }
        items = newItems;
    }
}

shared_ptr<SocketProxy> ConnectionPool::socketProxy() const
{
    shared_ptr<SimpleProxySwitcher> sps = dynamic_pointer_cast<SimpleProxySwitcher>(proxySwitcher);
    if (sps) {
        if (!sps->socketProxies.empty()) {
            return sps->socketProxies.front();
        }
        //        else if (!sps->httpProxies.empty()) {
        //           return sps->httpProxies.front();
        //        }
    }
    return shared_ptr<SocketProxy>();
}

shared_ptr<HttpProxy> ConnectionPool::httpProxy() const
{
    shared_ptr<SimpleProxySwitcher> sps = dynamic_pointer_cast<SimpleProxySwitcher>(proxySwitcher);
    if (sps) {
        if (!sps->httpProxies.empty()) {
            return sps->httpProxies.at(0);
        }
    }
    return shared_ptr<HttpProxy>();
}

void ConnectionPool::setSocketProxy(shared_ptr<SocketProxy> proxy)
{
    shared_ptr<SimpleProxySwitcher> sps = dynamic_pointer_cast<SimpleProxySwitcher>(proxySwitcher);
    if (sps) {
        sps->socketProxies.clear();
        if (proxy) {
            sps->socketProxies.push_back(proxy);
        }
    }
}

void ConnectionPool::setHttpProxy(shared_ptr<HttpProxy> proxy)
{
    shared_ptr<SimpleProxySwitcher> sps = dynamic_pointer_cast<SimpleProxySwitcher>(proxySwitcher);
    if (sps) {
        sps->httpProxies.clear();
        if (proxy) {
            sps->httpProxies.push_back(proxy);
        }
    }
}

RequestError *toRequestError(HeaderSplitter::Error error)
{
    switch (error) {
    case HeaderSplitter::ConnectionError:
        return new ConnectionError();
    case HeaderSplitter::EncodingError:
        return new InvalidHeader();
    case HeaderSplitter::ExhausedMaxLine:
        return new InvalidHeader();
    case HeaderSplitter::LineTooLong:
        return new InvalidHeader();
    default:
        return nullptr;
    }
}

class SendRequestBodyCoroutine : public Coroutine
{
public:
    SendRequestBodyCoroutine(Coroutine * parentCoroutine, shared_ptr<SocketLike> connection,
                             shared_ptr<FileLike> body);
public:
    virtual void run() override;
private:
    Coroutine * parentCoroutine;
    shared_ptr<SocketLike> connection;
    shared_ptr<FileLike> body;
};

SendRequestBodyCoroutine::SendRequestBodyCoroutine(Coroutine * parentCoroutine,
                                                   shared_ptr<SocketLike> connection, shared_ptr<FileLike> body)
    : Coroutine()
    , parentCoroutine(parentCoroutine)
    , connection(connection)
    , body(body)
{
}

void SendRequestBodyCoroutine::run()
{
    if (!sendfile(body, connection) && parentCoroutine) {
        parentCoroutine->kill(new CoroutineInterruptedException());
    }
}

HttpResponse HttpSessionPrivate::send(HttpRequest &request)
{
    RequestError *error = nullptr;

    utils::Url &url = request.d->url;
    HttpResponse response;
    response.d->url = url;
    response.d->request = request;
    if ((!request.d->isWebSocket && url.scheme() != "http"
         && url.scheme() != "https")
        || (request.d->isWebSocket && url.scheme() != "ws"
            && url.scheme() != "wss")) {
        if (debugLevel > 0) {
            ngDebug() << "invalid scheme:" << url.scheme();
        }
        response.setError(new InvalidScheme());
        return response;
    }
    if (request.d->method.empty()) {
        if (debugLevel > 0) {
            ngDebug() << "empty method";
        }
        response.setError(new InvalidHeader());
        return response;
    }
    if (!request.d->query.query().empty()) {
        utils::UrlQuery merged(url.query());
        for (const auto &item : request.d->query.items()) {
            merged.addQueryItem(item.first, item.second);
        }
        url.setQuery(merged.query());
        response.d->url = url;
    }

    if (cacheManager
        && (request.d->method == "GET" || request.d->method == "HEAD"
            || request.d->method == "OPTIONS")) {
        const string &cacheControlHeader = request.header(KnownHeader::CacheControlHeader);
        if (cacheControlHeader.find("no-cache") == string::npos) {
            if (cacheManager->getResponse(&response)) {
                return response;
            }
        }
    }

    if (request.d->version == HttpVersion::Unknown) {
        request.d->version = defaultVersion;
    }

    mergeCookies(request, url.toString());
    vector<HttpHeader> allHeaders = makeHeaders(request, url.toString());

    string versionBytes;
    if (request.d->version == HttpVersion::Http1_0) {
        versionBytes = "HTTP/1.0";
    } else if (request.d->version == HttpVersion::Http1_1) {
        versionBytes = "HTTP/1.1";
        //    } else if(request.d->version == HttpVersion::Http2_0) {
        //        versionBytes = "HTTP/2.0";
    } else {
        if (debugLevel > 0) {
            ngDebug() << "invalid http version:" << request.d->version;
        }
        response.setError(new UnsupportedVersion());
        return response;
    }

    vector<string> lines;
    string resourcePath = urlResourcePath(url);
    if (resourcePath.empty()) {
        resourcePath = "/";
    }
    const string &commandLine = utils::toUpper(request.d->method) + string(" ") + resourcePath
            + string(" ") + versionBytes + string("\r\n");
    lines.push_back(commandLine);
    for (int i = 0; i < allHeaders.size(); ++i) {
        const HttpHeader &header = allHeaders.at(i);
        lines.push_back(header.name + string(": ") + header.value + string("\r\n"));
    }
    lines.push_back(string("\r\n"));
    if (debugLevel > 0) {
        for (const string &line : lines) {
            ngDebug() << "sending headers:" << line;
        }
    }
    const string headerBytes = joinLines(lines);

    unique_ptr<ScopedLock<Semaphore>> ptrLock;
    shared_ptr<Semaphore> lock;

    shared_ptr<SocketLike> connection = request.connection();
    if (!connection) {
        lock = getSemaphore(url.toString());
        ptrLock.reset(new ScopedLock<Semaphore>(*lock));
        if (!ptrLock->isSuccess()) {
            response.setError(new ConnectionError());
            return response;
        }

        // try keep-alive connections first.
        if (keepAlive) {
            connection = oldConnectionForUrl(url.toString());
        }
        // make a new connection.
        if (!connection) {
            float timeout = request.d->connectionTimeout < 0 ? defaultConnectionTimeout : request.d->connectionTimeout;
            try {
                Timeout t(timeout);
                connection = newConnectionForUrl(url.toString(), &error);
            } catch (TimeoutException &) {
                response.setError(new ConnectTimeout());
                return response;
            }
            if (error != nullptr) {
                response.setError(error);
                return response;
            }
        }
    }

    if (connection->sendall(headerBytes) != headerBytes.size()) {
        response.setError(new ConnectionError());
        return response;
    }

    HeaderSplitter headerSplitter(connection, debugLevel);
    HeaderSplitter::Error headerSplitterError;
    unique_ptr<Coroutine> sendingReuqestBodyCoroutine(
            new SendRequestBodyCoroutine(Coroutine::current(), connection, request.d->body));
    if (request.d->body) {
        if (debugLevel > 0) {
            ngDebug() << "sending body:" << request.d->body->size();
        }
        sendingReuqestBodyCoroutine->start();
        try {
            headerSplitter.buf = connection->recv(1024 * 8);
            if (sendingReuqestBodyCoroutine->isRunning()) {
                sendingReuqestBodyCoroutine->kill();
            }
            sendingReuqestBodyCoroutine->join();
            sendingReuqestBodyCoroutine.reset();
        } catch (CoroutineInterruptedException &) {
            if (debugLevel > 0) {
                ngDebug() << "the server terminated connection while sending body." << headerSplitter.buf.size();
            }
            sendingReuqestBodyCoroutine->join();
            if (headerSplitter.buf.empty()) {
                response.setError(new ConnectionError());
                return response;
            }
        } catch (...) {
            if (sendingReuqestBodyCoroutine->isRunning()) {
                sendingReuqestBodyCoroutine->kill();
            }
            sendingReuqestBodyCoroutine->join();
            throw;
        }
    }

    // parse first line.
    const string &firstLine = headerSplitter.nextLine(&headerSplitterError);
    error = toRequestError(headerSplitterError);
    if (error != nullptr) {
        if (debugLevel > 0) {
            ngDebug() << "read http response header error:" << error->what();
        }
        response.setError(error);
        return response;
    }
    vector<string> commands = splitWhitespace(firstLine);
    if (commands.size() < 3) {
        response.setError(new InvalidHeader());
        return response;
    }
    if (commands.at(0) == "HTTP/1.0") {
        response.d->version = Http1_0;
    } else if (commands.at(0) == "HTTP/1.1") {
        response.d->version = Http1_1;
    } else {
        response.setError(new InvalidHeader());
        return response;
    }
    bool ok;
    response.d->statusCode = utils::parseInt(commands.at(1), &ok);
    if (!ok) {
        response.setError(new InvalidHeader());
        return response;
    }
    response.d->statusText = joinWithSeparator(' ', commands, 2);

    // parse headers.
    const int MaxHeaders = 64;
    vector<HttpHeader> headers = headerSplitter.headers(MaxHeaders, &headerSplitterError);
    if (headerSplitterError != HeaderSplitter::NoError) {
        response.setError(toRequestError(headerSplitterError));
        return response;
    } else {
        response.setHeaders(headers);
        if (debugLevel > 0) {
            for (const HttpHeader &header : headers) {
                ngDebug() << "receiving header:" << header.name << header.value;
            }
        }
    }

    // merge cookies.
    if (managingCookies && response.hasHeader("Set-Cookie")) {
        for (const string &value : response.multiHeader("Set-Cookie")) {
            const vector<HttpCookie> &cookies = HttpCookie::parseCookies(value);
            if (debugLevel > 0 && !cookies.empty()) {
                ngDebug() << "receiving cookie:" << cookies[0].toRawForm();
            }
            response.d->cookies.insert(response.d->cookies.end(), cookies.begin(), cookies.end());
        }
        cookieJar.setCookiesFromUrl(response.d->cookies, response.d->url.toString());
    }

    // read body.
    response.d->body = headerSplitter.buf;
    response.d->stream = connection;
    if (!request.streamResponse() && response.d->statusCode != HttpStatus::NoContent) {
        if (utils::toUpper(request.method()) == "HEAD") {
            response.d->consumed = true;
            response.d->body.clear();
        } else {
            const string &body = response.body();
            if (response.d->error) {
                return response;
            }
            if (debugLevel == 1 && !body.empty()) {
                ngDebug() << "receiving body:" << body.size();
            } else if (debugLevel > 1 && !body.empty()) {
                ngDebug() << "receiving body:" << body;
            }
            // HttpStatus::SwitchProtocol connection can not be recycled().
            if (ptrLock && connection->isValid() && response.statusCode() >= 200
                && utils::equalsIgnoreCase(response.header(KnownHeader::ConnectionHeader), "keep-alive") && keepAlive) {
                recycle(response.d->url.toString(), connection);
            }
        }
        response.d->stream.reset();
    }

    // response.d->statusCode < 200 is not error.
    if (response.d->statusCode >= 400) {
        response.setError(new HTTPError(response.d->statusCode));
    } else {
        const string rm = utils::toUpper(request.method());
        if ((rm == "GET" || rm == "HEAD" || rm == "OPTIONS")
            && cacheManager && !request.streamResponse()) {
            bool doCache = true;
            const string &requestHeader = utils::toLower(request.header(KnownHeader::CacheControlHeader));
            if (requestHeader.find("no-cache") != string::npos || requestHeader.find("no-store") != string::npos) {
                doCache = false;
            } else {
                const string &responseHeader = utils::toLower(response.header(KnownHeader::CacheControlHeader));
                if (responseHeader.find("public") != string::npos || responseHeader.find("private") != string::npos) {
                    doCache = true;
                } else if (responseHeader.find("no-cache") != string::npos || responseHeader.find("no-store") != string::npos) {
                    doCache = false;
                } else {
                    doCache = false;
                }
            }
            if (doCache) {
                cacheManager->addResponse(response);
            }
        }
    }
    return response;
}

vector<HttpHeader> HttpSessionPrivate::makeHeaders(HttpRequest &request, const string &urlStr) const
{
    const utils::Url url(urlStr);
    vector<HttpHeader> allHeaders = request.allHeaders();

    if (!request.hasHeader("Connection") && request.version() == Http1_1) {
        if (keepAlive) {
            allHeaders.insert(allHeaders.begin(), HttpHeader("Connection", string("keep-alive")));
        } else {
            allHeaders.insert(allHeaders.begin(), HttpHeader("Connection", string("close")));
        }
    }
    if (!request.hasHeader("Content-Length") && request.d->body) {
        int64_t requestBodySize = request.d->body->size();
        if (requestBodySize > 0) {
            allHeaders.insert(allHeaders.begin(), HttpHeader("Content-Length", utils::number(static_cast<long long>(requestBodySize))));
        }
    }
    if (!request.hasHeader("User-Agent")) {
        if (request.userAgent().empty()) {
            allHeaders.insert(allHeaders.begin(), HttpHeader("User-Agent", defaultUserAgent));
        } else {
            allHeaders.insert(allHeaders.begin(), HttpHeader("User-Agent", request.userAgent()));
        }
    }
    if (!request.hasHeader("Host")) {
        string httpHost = url.host();
        if (url.port() != -1) {
            httpHost += ":" + utils::number(url.port());
        }
        allHeaders.insert(allHeaders.begin(), HttpHeader("Host", httpHost));
    }
    if (!request.hasHeader("Accept")) {
        allHeaders.push_back(HttpHeader("Accept", string("*/*")));
    }
    if (!request.hasHeader("Accept-Language")) {
        allHeaders.push_back(HttpHeader("Accept-Language", string("en-US,en;q=0.5")));
    }
    if (!request.hasHeader("Accept-Encoding")) {
#ifdef QTNG_HAVE_ZLIB
        allHeaders.push_back(HttpHeader("Accept-Encoding", string("gzip, deflate")));
#else
        allHeaders.push_back(HttpHeader("Accept-Encoding", string("identity")));
#endif
    }
    if (!request.d->cookies.empty() && !request.hasHeader("Cookie")) {
        string result;
        bool first = true;
        for (const HttpCookie &cookie : request.d->cookies) {
            if (!first)
                result += "; ";
            first = false;
            result += cookie.toRawForm(HttpCookie::NameAndValueOnly);
        }
        allHeaders.push_back(HttpHeader("Cookie", result));
    }
    return allHeaders;
}

void HttpSessionPrivate::mergeCookies(HttpRequest &request, const string &urlStr)
{
    const utils::Url url(urlStr);
    if (!managingCookies) {
        return;
    }
    vector<HttpCookie> cookies = cookieJar.cookiesForUrl(urlStr);
    if (cookies.empty()) {
        return;
    }
    for (const HttpCookie &cookie : cookies) {
        bool found = false;
        for (const HttpCookie &newCookie : request.d->cookies) {
            if (newCookie.hasSameIdentifier(cookie) && newCookie.isSecure() == cookie.isSecure()
                && newCookie.isHttpOnly() == cookie.isHttpOnly()) {
                found = true;
                break;
            }
        }
        if (!found) {
            request.d->cookies.push_back(cookie);
        }
    }
}

void HttpSessionPrivate::prepareWebSocketRequest(HttpRequest &request, string &secKey)
{
    secKey = utils::bytesToBase64(randomBytes(16));
    request.setStreamResponse(true);
    request.d->isWebSocket = true;
    request.setMethod("GET");
    request.addHeader(UpgradeHeader, "websocket");
    request.addHeader(ConnectionHeader, "Upgrade");
    request.addHeader("Sec-WebSocket-Key", secKey);
    request.addHeader("Sec-WebSocket-Version", "13");
    vector<string> ps = webSocketConfiguration.protocols();
    if (!ps.empty()) {
        request.addHeader("Sec-WebSocket-Protocol", utils::join(ps, ", "));
    }
}

// implemented in websocket.cpp
void setWebSocketConnectionPrivateResponse(WebSocketConnectionPrivate *d, HttpResponse response);

shared_ptr<WebSocketConnection> HttpSessionPrivate::makeWebSocketConnection(HttpResponse &response,
                                                                                const string &secKey)
{
    if (!response.isOk()) {
        // TODO set error value and return.
        return shared_ptr<WebSocketConnection>();
    }
    if (response.statusCode() != SwitchProtocol) {
        // TODO set error value and return.
        return shared_ptr<WebSocketConnection>();
    }

    const string &upgradeHeader = response.header("Upgrade");
    const string &connectionHeader = response.header("Connection");
    if (upgradeHeader != "websocket" || connectionHeader != "upgrade") {
        // TODO set error value and return.
        return shared_ptr<WebSocketConnection>();
    }

    const string uuid("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    const string &t = secKey + uuid;
    const string &myKey = utils::bytesToBase64(MessageDigest::hash(t, MessageDigest::Sha1));
    const string &itsKey = response.header("Sec-WebSocket-Accept");
    if (myKey != itsKey) {
        // TODO set error value and return.
        return shared_ptr<WebSocketConnection>();
    }

    string headBytes;
    shared_ptr<SocketLike> raw = response.takeStream(&headBytes);
    if (!raw) {
        ngWarning() << "the web socket steam is null.";
    }

    shared_ptr<WebSocketConnection> connection =
            make_shared<WebSocketConnection>(raw, headBytes, WebSocketConnection::Client, webSocketConfiguration);
    connection->setDebugLevel(this->debugLevel);
    setWebSocketConnectionPrivateResponse(connection->d_func(), response);
    return connection;
}

void setProxySwitcher(HttpSession *session, shared_ptr<BaseProxySwitcher> switcher)
{
    if (switcher) {
        HttpSessionPrivate::getPrivateHelper(session)->proxySwitcher = switcher;
    } else {
        HttpSessionPrivate::getPrivateHelper(session)->proxySwitcher.reset(new SimpleProxySwitcher());
    }
}

HttpSession::HttpSession()
    : d_ptr(new HttpSessionPrivate(this))
{
}

HttpSession::~HttpSession()
{
    delete d_ptr;
}

HttpResponse HttpSession::get(const string &url)
{
    HttpRequest request;
    request.setMethod("GET");
    request.setUrl(url);
    return send(request);
}

HttpResponse HttpSession::get(const string &url, const map<string, string> &query)
{
    HttpRequest request;
    request.setMethod("GET");
    request.setUrl(url);
    request.setQuery(query);
    return send(request);
}

HttpResponse HttpSession::get(const string &url, const map<string, string> &query,
                              const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("GET");
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    return send(request);
}

HttpResponse HttpSession::get(const string &url, const utils::UrlQuery &query)
{
    HttpRequest request;
    request.setMethod("GET");
    request.setUrl(url);
    request.setQuery(query);
    return send(request);
}

HttpResponse HttpSession::get(const string &url, const utils::UrlQuery &query, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("GET");
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    return send(request);
}

HttpResponse HttpSession::head(const string &url)
{
    HttpRequest request;
    request.setMethod("HEAD");
    request.setUrl(url);
    return send(request);
}

HttpResponse HttpSession::head(const string &url, const map<string, string> &query)
{
    HttpRequest request;
    request.setMethod("HEAD");
    request.setUrl(url);
    request.setQuery(query);
    return send(request);
}

HttpResponse HttpSession::head(const string &url, const map<string, string> &query,
                               const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("HEAD");
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    return send(request);
}

HttpResponse HttpSession::head(const string &url, const utils::UrlQuery &query)
{
    HttpRequest request;
    request.setMethod("HEAD");
    request.setUrl(url);
    request.setQuery(query);
    return send(request);
}

HttpResponse HttpSession::head(const string &url, const utils::UrlQuery &query, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("HEAD");
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    return send(request);
}

HttpResponse HttpSession::options(const string &url)
{
    HttpRequest request;
    request.setMethod("OPTIONS");
    request.setUrl(url);
    return send(request);
}

HttpResponse HttpSession::options(const string &url, const map<string, string> &query)
{
    HttpRequest request;
    request.setMethod("OPTIONS");
    request.setUrl(url);
    request.setQuery(query);
    return send(request);
}

HttpResponse HttpSession::options(const string &url, const map<string, string> &query,
                                  const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("OPTIONS");
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    return send(request);
}

HttpResponse HttpSession::options(const string &url, const utils::UrlQuery &query)
{
    HttpRequest request;
    request.setMethod("OPTIONS");
    request.setUrl(url);
    request.setQuery(query);
    return send(request);
}

HttpResponse HttpSession::options(const string &url, const utils::UrlQuery &query, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("OPTIONS");
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    return send(request);
}

HttpResponse HttpSession::delete_(const string &url)
{
    HttpRequest request;
    request.setMethod("DELETE");
    request.setUrl(url);
    return send(request);
}

HttpResponse HttpSession::delete_(const string &url, const map<string, string> &query)
{
    HttpRequest request;
    request.setMethod("DELETE");
    request.setUrl(url);
    request.setQuery(query);
    return send(request);
}

HttpResponse HttpSession::delete_(const string &url, const map<string, string> &query,
                                  const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("DELETE");
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    return send(request);
}

HttpResponse HttpSession::delete_(const string &url, const utils::UrlQuery &query)
{
    HttpRequest request;
    request.setMethod("DELETE");
    request.setUrl(url);
    request.setQuery(query);
    return send(request);
}

HttpResponse HttpSession::delete_(const string &url, const utils::UrlQuery &query, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("DELETE");
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    return send(request);
}

HttpResponse HttpSession::post(const string &url, const string &body)
{
    HttpRequest request;
    request.setMethod("POST");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const string &url, shared_ptr<FileLike> body)
{
    HttpRequest request;
    request.setMethod("POST");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const string &url, const map<string, string> &body)
{
    HttpRequest request;
    request.setMethod("POST");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const string &url, const utils::UrlQuery &body)
{
    HttpRequest request;
    request.setMethod("POST");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const string &url, const FormData &body)
{
    HttpRequest request;
    request.setMethod("POST");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const string &url, const string &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("POST");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const string &url, const map<string, string> &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("POST");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const string &url, const utils::UrlQuery &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("POST");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::post(const string &url, const FormData &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("POST");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const string &url, const string &body)
{
    HttpRequest request;
    request.setMethod("PATCH");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const string &url, shared_ptr<FileLike> body)
{
    HttpRequest request;
    request.setMethod("PATCH");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const string &url, const map<string, string> &body)
{
    HttpRequest request;
    request.setMethod("PATCH");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const string &url, const utils::UrlQuery &body)
{
    HttpRequest request;
    request.setMethod("PATCH");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const string &url, const FormData &body)
{
    HttpRequest request;
    request.setMethod("PATCH");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const string &url, const string &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("PATCH");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const string &url, const map<string, string> &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("PATCH");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const string &url, const utils::UrlQuery &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("PATCH");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::patch(const string &url, const FormData &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("PATCH");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const string &url, const string &body)
{
    HttpRequest request;
    request.setMethod("PUT");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const string &url, shared_ptr<FileLike> body)
{
    HttpRequest request;
    request.setMethod("PUT");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const string &url, const map<string, string> &body)
{
    HttpRequest request;
    request.setMethod("PUT");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const string &url, const utils::UrlQuery &body)
{
    HttpRequest request;
    request.setMethod("PUT");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const string &url, const FormData &body)
{
    HttpRequest request;
    request.setMethod("PUT");
    request.setUrl(url);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const string &url, const string &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("PUT");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const string &url, const map<string, string> &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("PUT");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const string &url, const utils::UrlQuery &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("PUT");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

HttpResponse HttpSession::put(const string &url, const FormData &body, const map<string, string> &headers)
{
    HttpRequest request;
    request.setMethod("PUT");
    request.setUrl(url);
    request.setHeaders(headers);
    request.setBody(body);
    return send(request);
}

shared_ptr<WebSocketConnection> HttpSession::ws(const string &url)
{
    NG_D(HttpSession);
    HttpRequest request;
    string secKey;
    request.setUrl(url);
    d->prepareWebSocketRequest(request, secKey);
    HttpResponse response = send(request);
    return d->makeWebSocketConnection(response, secKey);
}

shared_ptr<WebSocketConnection> HttpSession::ws(const string &url, const map<string, string> &query)
{
    NG_D(HttpSession);
    HttpRequest request;
    string secKey;
    request.setUrl(url);
    request.setQuery(query);
    d->prepareWebSocketRequest(request, secKey);
    HttpResponse response = send(request);
    return d->makeWebSocketConnection(response, secKey);
}

shared_ptr<WebSocketConnection> HttpSession::ws(const string &url, const map<string, string> &query,
                                                const map<string, string> &headers)
{
    NG_D(HttpSession);
    HttpRequest request;
    string secKey;
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    d->prepareWebSocketRequest(request, secKey);
    HttpResponse response = send(request);
    return d->makeWebSocketConnection(response, secKey);
}

shared_ptr<WebSocketConnection> HttpSession::ws(const string &url, const utils::UrlQuery &query)
{
    NG_D(HttpSession);
    HttpRequest request;
    string secKey;
    request.setUrl(url);
    request.setQuery(query);
    d->prepareWebSocketRequest(request, secKey);
    HttpResponse response = send(request);
    return d->makeWebSocketConnection(response, secKey);
}

shared_ptr<WebSocketConnection> HttpSession::ws(const string &url, const utils::UrlQuery &query,
                                                const map<string, string> &headers)
{
    NG_D(HttpSession);
    HttpRequest request;
    string secKey;
    request.setUrl(url);
    request.setQuery(query);
    request.setHeaders(headers);
    d->prepareWebSocketRequest(request, secKey);
    HttpResponse response = send(request);
    return d->makeWebSocketConnection(response, secKey);
}

static bool isRedirect(int httpCode)
{
    switch (httpCode) {
    case 300:  // HTTP_MULT_CHOICE
    case 301:  // HTTP_MOVED_PERM
    case 302:  // HTTP_MOVED_TEMP
    case 303:  // HTTP_SEE_OTHER
    case 307:  // HTTP_TEMP_REDIRECT
    case 308:  // HTTP_PERM_REDIRECT
        return true;
    default:
        return false;
    }
}

HttpResponse HttpSession::send(HttpRequest &request)
{
    NG_D(HttpSession);
    float requestTimeout = request.timeout() < 0 ? d->defaultTimeout : request.timeout();
    utils::ElapsedTimer timer;
    timer.restart();

    HttpResponse response;
    vector<HttpResponse> history;
    Timeout timeout(requestTimeout);
    try {
        response = d->send(request);
    } catch (TimeoutException &) {
        response.setUrl(request.url());
        response.setError(new class RequestTimeout());
        response.setElapsed(timer.elapsed());
        return response;
    }
    if (request.maxRedirects() > 0 && !request.connection()) {
        int tries = 0;
        while (response.isOk() && isRedirect(response.statusCode())) {
            if (tries > request.maxRedirects()) {
                response.setError(new TooManyRedirects());
                response.setElapsed(timer.elapsed());
                return response;
            }
            HttpRequest newRequest = request;
            newRequest.setMaxRedirects(request.maxRedirects() - tries - 1);
            if (response.statusCode() == 303 || response.statusCode() == 307) {
                newRequest.setMethod(request.method());
                newRequest.setBody(request.body());
            } else {
                newRequest.setMethod("GET");  // not rfc behavior, but many browser do this.
                newRequest.setBody(string());
            }
            newRequest.setUrl(resolveRelativeUrl(request.url(), response.getLocation()));
            if (!newRequest.url().isValid()) {
                response.setError(new InvalidURL());
                return response;
            }
            try {
                HttpResponse newResponse = d->send(newRequest);
                history.push_back(response);
                response = newResponse;
                ++tries;
            } catch (TimeoutException &) {
                HttpResponse newResponse;
                newResponse.setUrl(newRequest.url());
                newResponse.setError(new class RequestTimeout());
                history.push_back(response);
                response = newResponse;
                break;
            }
        }
    }
    response.setHistory(history);
    response.setElapsed(timer.elapsed());
    return response;
}

HttpCookieJar &HttpSession::cookieJar()
{
    NG_D(HttpSession);
    return d->cookieJar;
}

HttpCookie HttpSession::cookie(const string &url, const string &name)
{
    NG_D(HttpSession);
    const HttpCookieJar &jar = d->cookieJar;
    vector<HttpCookie> cookies = jar.cookiesForUrl(url);
    for (int i = 0; i < cookies.size(); ++i) {
        const HttpCookie &cookie = cookies.at(i);
        if (cookie.name() == name) {
            return cookie;
        }
    }
    return HttpCookie();
}

void HttpSession::setManagingCookies(bool managingCookies)
{
    NG_D(HttpSession);
    d->managingCookies = managingCookies;
}

void HttpSession::setMaxConnectionsPerServer(int maxConnectionsPerServer)
{
    NG_D(HttpSession);
    if (maxConnectionsPerServer <= 0) {
        maxConnectionsPerServer = INT_MAX;
    }
    d->maxConnectionsPerServer = maxConnectionsPerServer;
    // TODO update semphores
}

int HttpSession::maxConnectionsPerServer()
{
    NG_D(HttpSession);
    return d->maxConnectionsPerServer;
}

void HttpSession::setDebugLevel(int level)
{
    NG_D(HttpSession);
    d->debugLevel = level;
}

void HttpSession::disableDebug()
{
    NG_D(HttpSession);
    d->debugLevel = 0;
}

void HttpSession::setKeepAlive(bool keepAlive)
{
    NG_D(HttpSession);
    d->keepAlive = keepAlive;
}

bool HttpSession::keepAlive() const
{
    NG_D(const HttpSession);
    return d->keepAlive;
}

string HttpSession::defaultUserAgent() const
{
    NG_D(const HttpSession);
    return d->defaultUserAgent;
}

void HttpSession::setDefaultUserAgent(const string &userAgent)
{
    NG_D(HttpSession);
    d->defaultUserAgent = userAgent;
}

HttpVersion HttpSession::defaultVersion() const
{
    NG_D(const HttpSession);
    return d->defaultVersion;
}

void HttpSession::setDefaultVersion(HttpVersion defaultVersion)
{
    NG_D(HttpSession);
    d->defaultVersion = defaultVersion;
}

float HttpSession::defaultConnnectionTimeout() const
{
    NG_D(const HttpSession);
    return d->defaultConnectionTimeout;
}

void HttpSession::setDefaultConnectionTimeout(float timeout)
{
    NG_D(HttpSession);
    d->defaultConnectionTimeout = timeout;
}

float HttpSession::defaultTimeout() const
{
    NG_D(const HttpSession);
    return d->defaultTimeout;
}

void HttpSession::setDefaultTimeout(float defaultTimeout)
{
    NG_D(HttpSession);
    d->defaultTimeout = defaultTimeout;
}

void HttpSession::setDnsCache(shared_ptr<SocketDnsCache> dnsCache)
{
    NG_D(HttpSession);
    d->dnsCache = dnsCache;
}

shared_ptr<SocketDnsCache> HttpSession::dnsCache() const
{
    NG_D(const HttpSession);
    return d->dnsCache;
}

shared_ptr<SocketProxy> HttpSession::socketProxy() const
{
    NG_D(const HttpSession);
    return d->socketProxy();
}

void HttpSession::setSocketProxy(shared_ptr<SocketProxy> proxy)
{
    NG_D(HttpSession);
    d->setSocketProxy(proxy);
}

shared_ptr<HttpProxy> HttpSession::httpProxy() const
{
    NG_D(const HttpSession);
    return d->httpProxy();
}

void HttpSession::setHttpProxy(shared_ptr<HttpProxy> proxy)
{
    NG_D(HttpSession);
    d->setHttpProxy(proxy);
}

shared_ptr<HttpCacheManager> HttpSession::cacheManager() const
{
    NG_D(const HttpSession);
    return d->cacheManager;
}

void HttpSession::setCacheManager(shared_ptr<HttpCacheManager> cacheManager)
{
    NG_D(HttpSession);
    d->cacheManager = cacheManager;
}

#ifndef QTNG_NO_CRYPTO

SslConfiguration &HttpSession::sslConfiguration()
{
    NG_D(HttpSession);
    return d->sslConfig;
}

#endif

WebSocketConfiguration &HttpSession::webSocketConfiguration()
{
    NG_D(HttpSession);
    return d->webSocketConfiguration;
}

HttpCacheManager::HttpCacheManager() { }

HttpCacheManager::~HttpCacheManager() { }

bool HttpCacheManager::addResponse(HttpResponse &response)
{
    const string &url = response.url().toString();
    int statusCode = response.statusCode();
    const string &statusText = response.statusText();
    const vector<HttpHeader> headers = response.allHeaders();
    const string &body = response.body();
    string bs;
    MsgPackStream ds(&bs, true);
    ds << statusCode << statusText << headers << body;
    if (!ds.isOk()) {
        return false;
    }
    return store(url, bs);
}

bool HttpCacheManager::getResponse(HttpResponse *response)
{
    const string &url = response->url().toString();
    if (url.empty()) {
        return false;
    }
    const string &bs = load(url);
    if (bs.empty()) {
        return false;
    }
    MsgPackStream ds(bs);
    int statusCode;
    string statusText;
    vector<HttpHeader> headers;
    string body;
    ds >> statusCode >> statusText >> headers >> body;
    if (!ds.isOk()) {
        return false;
    }
    response->setStatusCode(statusCode);
    response->setStatusText(statusText);
    response->setHeaders(headers);
    response->setBody(body);
    return true;
}

bool HttpCacheManager::store(const string &, const string &)
{
    return false;
}

string HttpCacheManager::load(const string &)
{
    return string();
}

class HttpMemoryCacheManagerPrivate
{
public:
    HttpMemoryCacheManagerPrivate()
        : expireTime(60 * 60 * 24)  // one day
    {
    }
public:
    map<string, string> cache;
    float expireTime;
};

HttpMemoryCacheManager::HttpMemoryCacheManager()
    : d_ptr(new HttpMemoryCacheManagerPrivate())
{
}

HttpMemoryCacheManager::~HttpMemoryCacheManager()
{
    delete d_ptr;
}

float HttpMemoryCacheManager::expireTime() const
{
    NG_D(const HttpMemoryCacheManager);
    return d->expireTime;
}

void HttpMemoryCacheManager::setExpireTime(float expireTime)
{
    NG_D(HttpMemoryCacheManager);
    d->expireTime = expireTime;
}

bool HttpMemoryCacheManager::store(const string &url, const string &data)
{
    NG_D(HttpMemoryCacheManager);
    d->cache[url] = data;
    return true;
}

string HttpMemoryCacheManager::load(const string &url)
{
    NG_D(HttpMemoryCacheManager);
    return d->cache.count(url) ? d->cache.at(url) : string();
}

bool HttpDiskCacheManager::store(const string &url, const string &data)
{
    const string &filename = utils::bytesToHex(MessageDigest::hash(url, MessageDigest::Sha256));
    const string &fullpath = (cacheDir / filename).path();
    auto f = PosixPath(fullpath).open("w");
    if (!f) {
        return false;
    }
    int64_t bs = f->write(data);
    if (bs != static_cast<int64_t>(data.size())) {
        return false;
    }
    return true;
}

string HttpDiskCacheManager::load(const string &url)
{
    const string &filename = utils::bytesToHex(MessageDigest::hash(url, MessageDigest::Sha256));
    const string &fullpath = (cacheDir / filename).path();
    auto f = PosixPath(fullpath).open("r");
    if (!f) {
        return string();
    }
    return f->readall(nullptr);
}

RequestError::~RequestError() { }

string RequestError::what() const
{
    return "An HTTP error occurred.";
}

string HTTPError::what() const
{
    return utils::formatMessage("server respond error. httpCode:%1", {utils::number(statusCode)});
}

string ConnectionError::what() const
{
    return "A Connection error occurred.";
}

string ProxyError::what() const
{
    return "A proxy error occurred.";
}

string SSLError::what() const
{
    return "A SSL error occurred.";
}

string RequestTimeout::what() const
{
    return "The request timed out.";
}

string ConnectTimeout::what() const
{
    return "The request timed out while trying to connect to the remote server.";
}

string ReadTimeout::what() const
{
    return "The server did not send any data in the allotted amount of time.";
}

string URLRequired::what() const
{
    return "A valid URL is required to make a request.";
}

string TooManyRedirects::what() const
{
    return "Too many redirects.";
}

string MissingSchema::what() const
{
    return "The URL schema (e.g. http or https) is missing.";
}

string InvalidScheme::what() const
{
    return "The URL schema can not be handled.";
}

string UnsupportedVersion::what() const
{
    return "The HTTP version is not supported yet.";
}

string InvalidURL::what() const
{
    return "The URL provided was somehow invalid.";
}

string InvalidHeader::what() const
{
    return "Can not parse the http header.";
}

string ChunkedEncodingError::what() const
{
    return "The server declared chunked encoding but sent an invalid chunk.";
}

string ContentDecodingError::what() const
{
    return "Failed to decode response content";
}

string StreamConsumedError::what() const
{
    return "The content for this response was already consumed";
}

string RetryError::what() const
{
    return "Custom retries logic failed";
}

string UnrewindableBodyError::what() const
{
    return "Requests encountered an error when trying to rewind a body";
}

}  // namespace qtng
