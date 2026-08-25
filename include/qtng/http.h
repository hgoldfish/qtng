#ifndef QTNG_HTTP_H
#define QTNG_HTTP_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/coroutine.h"
#include "qtng/http_utils.h"
#include "qtng/http_cookie.h"
#include "qtng/utils/mime.h"
#include "qtng/utils/url.h"
#include "qtng/io_utils.h"
#include "qtng/md.h"
#include "qtng/utils/platform.h"

namespace qtng {

namespace detail {

// Deep-copy bridge used by the Qt binding. HttpRequest/HttpResponse are
// intentionally shallow-copyable (their copy constructors share the private
// data); this class is the only sanctioned path to an independent copy and is
// declared in qtng/private/http_p.h.
class HttpDeepCopy;

}  // namespace detail

class FormData
{
public:
    FormData();
    static std::string makeBoundary();
    std::string toByteArray() const;

    struct Query
    {
        Query(const std::string &name, const std::string &value)
            : name_(name)
            , value_(value)
        {
        }
        std::string name() const { return name_; }
        void setName(const std::string &name) { name_ = name; }
        std::string value() const { return value_; }
        void setValue(const std::string &value) { value_ = value; }
    private:
        std::string name_;
        std::string value_;
    };
    struct File
    {
        File(const std::string &name, const std::string &filename, const std::string &data, const std::string &contentType)
            : name_(name)
            , filename_(filename)
            , data_(data)
            , contentType_(contentType)
        {
        }
        std::string name() const { return name_; }
        void setName(const std::string &name) { name_ = name; }
        std::string filename() const { return filename_; }
        void setFilename(const std::string &filename) { filename_ = filename; }
        std::string data() const { return data_; }
        void setData(const std::string &data) { data_ = data; }
        std::string contentType() const { return contentType_; }
        void setContentType(const std::string &contentType) { contentType_ = contentType; }
    private:
        std::string name_;
        std::string filename_;
        std::string data_;
        std::string contentType_;
    };

    void addFile(const std::string &name, const std::string &filename, const std::string &data,
                 const std::string &contentType = std::string())
    {
        std::string newContentType;
        if (contentType.empty()) {
#ifndef NG_OS_ANDROID
            newContentType = qtng::utils::mimeTypeForFileName(filename);
#endif
        } else {
            newContentType = contentType;
        }
        if (newContentType.empty()) {
            newContentType = "application/octet-stream";
        }
        files_.push_back(File(name, filename, data, newContentType));
    }

    void addQuery(const std::string &key, const std::string &value) { queries_.push_back(Query(key, value)); }
    std::vector<Query> &queries() { return queries_; }
    const std::vector<Query> &queries() const { return queries_; }
    void setQueries(const std::vector<Query> &queries) { queries_ = queries; }
    std::vector<File> &files() { return files_; }
    const std::vector<File> &files() const { return files_; }
    void setFiles(const std::vector<File> &files) { files_ = files; }
    std::string boundary() const { return boundary_; }
    void setBoundary(const std::string &boundary) { boundary_ = boundary; }
private:
    std::vector<Query> queries_;
    std::vector<File> files_;
    std::string boundary_;
};

class HttpRequestPrivate;
class HttpRequest : public HttpHeaderManager
{
public:
    enum CacheLoadControl { AlwaysNetwork, PreferNetwork, PreferCache, AlwaysCache };
    enum Priority { HighPriority = 1, NormalPriority = 3, LowPriority = 5 };

    HttpRequest();
    HttpRequest(const std::string &url)
        : HttpRequest()
    {
        setUrl(url);
    }
    HttpRequest(const std::string &method, const std::string &url)
        : HttpRequest()
    {
        setMethod(method);
        setUrl(url);
    }
    virtual ~HttpRequest();
    // Intentional: copyable. Copies share HttpRequestPrivate (including body/connection) via shared_ptr.
    // 有意可拷贝：拷贝通过 shared_ptr 共享 HttpRequestPrivate（含 body/connection）。
    HttpRequest(const HttpRequest &other);
    HttpRequest(HttpRequest &&other);
    HttpRequest &operator=(const HttpRequest &other);
public:
    std::string method() const;
    void setMethod(const std::string &method);
    qtng::utils::Url url() const;
    void setUrl(const qtng::utils::Url &url);
    void setUrl(const std::string &url) { setUrl(qtng::utils::Url(url)); }
    qtng::utils::UrlQuery query() const;
    void setQuery(const std::map<std::string, std::string> &query);
    void setQuery(const qtng::utils::UrlQuery &query);
    std::vector<HttpCookie> cookies() const;
    void setCookies(const std::vector<HttpCookie> &cookies);
    std::shared_ptr<FileLike> body() const;
    void setBody(const std::string &body);
    void setBody(std::shared_ptr<FileLike> body);
    // Full body content, independent of any read position. Only BytesIO
    // (what setBody(string) and its map/UrlQuery/FormData helpers construct)
    // can provide this; other FileLike streams are consumed by the first send
    // attempt and return an empty string. Lets HttpSession resend QUERY/POST
    // bodies after a redirect instead of re-reading an already-consumed stream.
    std::string bodyAsString() const;
    std::string userAgent() const;
    void setUserAgent(const std::string &userAgent);
    std::int64_t maxBodySize() const;
    void setMaxBodySize(std::int64_t maxBodySize);
    int maxRedirects() const;
    void setMaxRedirects(int maxRedirects);
    inline void disableRedirects() { setMaxRedirects(0); }
    Priority priority() const;
    void setPriority(Priority priority);
    HttpVersion version() const;
    void setVersion(HttpVersion version);
    void setStreamResponse(bool streamResponse);
    bool streamResponse() const;
    float connectionTimeout() const;
    void setConnectionTimeout(float connectionTimeout);
    float timeout() const;
    void setTimeout(float timeout);
    std::shared_ptr<SocketLike> connection() const;
    void useConnection(std::shared_ptr<SocketLike> connection);
public:
    void setBody(const FormData &formData);
    void setBody(const std::map<std::string, std::string> form);
    void setBody(const qtng::utils::UrlQuery &form);
private:
    std::shared_ptr<HttpRequestPrivate> d;
    friend class HttpSessionPrivate;
    friend class Http1Protocol;
    friend class detail::HttpDeepCopy;
#ifndef QTNG_NO_HTTP2
    friend class Http2Protocol;
    friend class Http2ClientSessionPrivate;
#endif
};

class RequestError
{
public:
    virtual ~RequestError();
    virtual std::string what() const;
};

class HttpResponsePrivate;
class HttpResponse : public HttpHeaderManager
{
public:
    HttpResponse();
    virtual ~HttpResponse();
    // Intentional: copyable. Copies share HttpResponsePrivate (including stream/consumed) via shared_ptr.
    // 有意可拷贝：拷贝通过 shared_ptr 共享 HttpResponsePrivate（含 stream/consumed）。
    HttpResponse(const HttpResponse &other);
    HttpResponse(HttpResponse &&other);
    HttpResponse &operator=(const HttpResponse &other);
public:
    qtng::utils::Url url() const;
    void setUrl(const qtng::utils::Url &url);
    void setUrl(const std::string &url) { setUrl(qtng::utils::Url(url)); }
    int statusCode() const;
    void setStatusCode(int statusCode);
    std::string statusText() const;
    void setStatusText(const std::string &statusText);
    std::vector<HttpCookie> cookies() const;
    void setCookies(const std::vector<HttpCookie> &cookies);
    HttpRequest request() const;
    void setRequest(const HttpRequest &request);
    std::int64_t elapsed() const;
    void setElapsed(std::int64_t elapsed);
    std::vector<HttpResponse> history() const;
    void setHistory(const std::vector<HttpResponse> &history);
    HttpVersion version() const;
    void setVersion(HttpVersion version);

    std::shared_ptr<SocketLike> takeStream(std::string *readBytes);
    inline std::shared_ptr<FileLike> bodyAsFile(bool processEncoding = true)
    {
        return bodyAsFile(processEncoding, processEncoding);
    }
    std::shared_ptr<FileLike> bodyAsFile(bool processGzip, bool processChunked);
    std::string body();
    void setBody(const std::string &body);
    std::string text();
    std::string html();

    bool isOk() const;
    bool hasNetworkError() const;
    bool hasHttpError() const;
public:
    std::shared_ptr<RequestError> error() const;
    void setError(std::shared_ptr<RequestError> error);
    void setError(RequestError *error) { setError(std::shared_ptr<RequestError>(error)); }
private:
    std::shared_ptr<HttpResponsePrivate> d;
    friend class HttpSessionPrivate;
    friend class Http1Protocol;
    friend class detail::HttpDeepCopy;
#ifndef QTNG_NO_HTTP2
    friend class Http2Protocol;
    friend class Http2ClientSessionPrivate;
#endif
};

class Socks5Proxy;
class HttpProxy;
class HttpSessionPrivate;
class HttpCacheManager;
class WebSocketConnection;
class SslConfiguration;
class WebSocketConfiguration;
class HttpSession
{
public:
    HttpSession();
    virtual ~HttpSession();
public:
    HttpResponse get(const std::string &url);
    HttpResponse get(const std::string &url, const std::map<std::string, std::string> &query);
    HttpResponse get(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
    HttpResponse get(const std::string &url, const qtng::utils::UrlQuery &query);
    HttpResponse get(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);

    HttpResponse head(const std::string &url);
    HttpResponse head(const std::string &url, const std::map<std::string, std::string> &query);
    HttpResponse head(const std::string &url, const std::map<std::string, std::string> &query,
                      const std::map<std::string, std::string> &headers);
    HttpResponse head(const std::string &url, const qtng::utils::UrlQuery &query);
    HttpResponse head(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);

    HttpResponse options(const std::string &url);
    HttpResponse options(const std::string &url, const std::map<std::string, std::string> &query);
    HttpResponse options(const std::string &url, const std::map<std::string, std::string> &query,
                         const std::map<std::string, std::string> &headers);
    HttpResponse options(const std::string &url, const qtng::utils::UrlQuery &query);
    HttpResponse options(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);

    HttpResponse delete_(const std::string &url);
    HttpResponse delete_(const std::string &url, const std::map<std::string, std::string> &query);
    HttpResponse delete_(const std::string &url, const std::map<std::string, std::string> &query,
                         const std::map<std::string, std::string> &headers);
    HttpResponse delete_(const std::string &url, const qtng::utils::UrlQuery &query);
    HttpResponse delete_(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);

    HttpResponse post(const std::string &url, const std::string &body);
    HttpResponse post(const std::string &url, std::shared_ptr<FileLike> body);
    HttpResponse post(const std::string &url, const std::map<std::string, std::string> &body);
    HttpResponse post(const std::string &url, const qtng::utils::UrlQuery &body);
    HttpResponse post(const std::string &url, const FormData &body);
    HttpResponse post(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
    HttpResponse post(const std::string &url, std::shared_ptr<FileLike> body, const std::map<std::string, std::string> &headers);
    HttpResponse post(const std::string &url, const std::map<std::string, std::string> &body, const std::map<std::string, std::string> &headers);
    HttpResponse post(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
    HttpResponse post(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);

    // RFC 10008 (The HTTP QUERY Method): a safe and idempotent request that
    // carries its input as request content, like POST on the wire but with GET
    // semantics, so caches and automatic retries may treat it as harmless.
    // The query content is passed as the body; a Content-Type is required.
    HttpResponse query(const std::string &url, const std::string &body);
    HttpResponse query(const std::string &url, std::shared_ptr<FileLike> body);
    HttpResponse query(const std::string &url, const std::map<std::string, std::string> &body);
    HttpResponse query(const std::string &url, const qtng::utils::UrlQuery &body);
    HttpResponse query(const std::string &url, const FormData &body);
    HttpResponse query(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
    HttpResponse query(const std::string &url, std::shared_ptr<FileLike> body, const std::map<std::string, std::string> &headers);
    HttpResponse query(const std::string &url, const std::map<std::string, std::string> &body, const std::map<std::string, std::string> &headers);
    HttpResponse query(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
    HttpResponse query(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);

    HttpResponse patch(const std::string &url, const std::string &body);
    HttpResponse patch(const std::string &url, std::shared_ptr<FileLike> body);
    HttpResponse patch(const std::string &url, const std::map<std::string, std::string> &body);
    HttpResponse patch(const std::string &url, const qtng::utils::UrlQuery &body);
    HttpResponse patch(const std::string &url, const FormData &body);
    HttpResponse patch(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
    HttpResponse patch(const std::string &url, std::shared_ptr<FileLike> body, const std::map<std::string, std::string> &headers);
    HttpResponse patch(const std::string &url, const std::map<std::string, std::string> &body,
                       const std::map<std::string, std::string> &headers);
    HttpResponse patch(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
    HttpResponse patch(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);

    HttpResponse put(const std::string &url, const std::string &body);
    HttpResponse put(const std::string &url, std::shared_ptr<FileLike> body);
    HttpResponse put(const std::string &url, const std::map<std::string, std::string> &body);
    HttpResponse put(const std::string &url, const qtng::utils::UrlQuery &body);
    HttpResponse put(const std::string &url, const FormData &body);
    HttpResponse put(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers);
    HttpResponse put(const std::string &url, std::shared_ptr<FileLike> body, const std::map<std::string, std::string> &headers);
    HttpResponse put(const std::string &url, const std::map<std::string, std::string> &body, const std::map<std::string, std::string> &headers);
    HttpResponse put(const std::string &url, const qtng::utils::UrlQuery &body, const std::map<std::string, std::string> &headers);
    HttpResponse put(const std::string &url, const FormData &body, const std::map<std::string, std::string> &headers);


    // make web socket connection.
    std::shared_ptr<WebSocketConnection> ws(const std::string &url);
    std::shared_ptr<WebSocketConnection> ws(const std::string &url, const std::map<std::string, std::string> &query);
    std::shared_ptr<WebSocketConnection> ws(const std::string &url, const std::map<std::string, std::string> &query, const std::map<std::string, std::string> &headers);
    std::shared_ptr<WebSocketConnection> ws(const std::string &url, const qtng::utils::UrlQuery &query);
    std::shared_ptr<WebSocketConnection> ws(const std::string &url, const qtng::utils::UrlQuery &query, const std::map<std::string, std::string> &headers);
    int webSocketErrorCode() const;
    std::string webSocketErrorReason() const;

    HttpResponse send(HttpRequest &request);
    std::shared_ptr<HttpCookieJar> cookieJar();
    HttpCookie cookie(const std::string &url, const std::string &name);
    void setManagingCookies(bool managingCookies);

    void setMaxConnectionsPerServer(int maxConnectionsPerServer);
    int maxConnectionsPerServer();

    void setDebugLevel(int level);
    void disableDebug();

    void setKeepAlive(bool keepAlive);
    bool keepAlive() const;

    std::string defaultUserAgent() const;
    void setDefaultUserAgent(const std::string &userAgent);
    HttpVersion defaultVersion() const;
    void setDefaultVersion(HttpVersion defaultVersion);
    float defaultConnnectionTimeout() const;
    void setDefaultConnectionTimeout(float timeout);
    float defaultTimeout() const;
    void setDefaultTimeout(float defaultTimeout);

    void setDnsCache(std::shared_ptr<SocketDnsCache> dnsCache);
    std::shared_ptr<SocketDnsCache> dnsCache() const;
    std::shared_ptr<SocketProxy> socketProxy() const;
    void setSocketProxy(std::shared_ptr<SocketProxy> proxy);
    std::shared_ptr<HttpProxy> httpProxy() const;
    void setHttpProxy(std::shared_ptr<HttpProxy> proxy);
    std::shared_ptr<HttpCacheManager> cacheManager() const;
    void setCacheManager(std::shared_ptr<HttpCacheManager> cacheManager);
#ifndef QTNG_NO_CRYPTO
    std::shared_ptr<SslConfiguration> sslConfiguration();
    void setSslConfiguration(const std::shared_ptr<SslConfiguration> &configuration);
#endif
    std::shared_ptr<WebSocketConfiguration> webSocketConfiguration();
    void setWebSocketConfiguration(const std::shared_ptr<WebSocketConfiguration> &configuration);
private:
    HttpSessionPrivate *d_ptr;
    NG_DECLARE_PRIVATE(HttpSession)
    NG_DISABLE_COPY_MOVE(HttpSession)
};

class HttpCacheManager
{
public:
    HttpCacheManager();
    virtual ~HttpCacheManager();
public:
    virtual bool addResponse(HttpResponse &response);
    virtual bool getResponse(HttpResponse *response);
protected:
    virtual bool store(const std::string &url, const std::string &data);
    virtual std::string load(const std::string &url);
};

class HttpMemoryCacheManagerPrivate;
class HttpMemoryCacheManager : public HttpCacheManager
{
public:
    HttpMemoryCacheManager();
    virtual ~HttpMemoryCacheManager() override;
public:
    float expireTime() const;
    void setExpireTime(float expireTime);
protected:
    std::map<std::string, std::string> &cache();
    virtual bool store(const std::string &url, const std::string &data) override;
    virtual std::string load(const std::string &url) override;
private:
    HttpMemoryCacheManagerPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(HttpMemoryCacheManager)
    NG_DISABLE_COPY_MOVE(HttpMemoryCacheManager)
};

class HttpDiskCacheManager : public HttpCacheManager
{
public:
    HttpDiskCacheManager(const PosixPath &cacheDir)
        : cacheDir(cacheDir)
    {
    }
    HttpDiskCacheManager(const std::string &cacheDir)
        : cacheDir(cacheDir)
    {
    }
protected:
    virtual bool store(const std::string &url, const std::string &data);
    virtual std::string load(const std::string &url);
protected:
    PosixPath cacheDir;
};

class HTTPError : public RequestError
{
public:
    HTTPError(int statusCode)
        : statusCode_(statusCode)
    {
    }
    virtual std::string what() const;
    int statusCode() const { return statusCode_; }
private:
    int statusCode_;
};

class ConnectionError : public RequestError
{
public:
    virtual std::string what() const;
};

class ProxyError : public ConnectionError
{
public:
    virtual std::string what() const;
};

class SSLError : public ConnectionError
{
public:
    virtual std::string what() const;
};

class RequestTimeout : public RequestError
{
public:
    virtual std::string what() const;
};

class ConnectTimeout : public RequestTimeout
{
public:
    virtual std::string what() const;
};

class ReadTimeout : public RequestTimeout
{
public:
    virtual std::string what() const;
};

class URLRequired : public RequestError
{
public:
    virtual std::string what() const;
};

class TooManyRedirects : public RequestError
{
public:
    virtual std::string what() const;
};

class MissingSchema : public RequestError
{
public:
    virtual std::string what() const;
};

class InvalidScheme : public RequestError
{
public:
    virtual std::string what() const;
};

class UnsupportedVersion : public RequestError
{
public:
    virtual std::string what() const;
};

class InvalidURL : public RequestError
{
public:
    virtual std::string what() const;
};

class InvalidHeader : public RequestError
{
public:
    virtual std::string what() const;
};

class ChunkedEncodingError : public RequestError
{
public:
    virtual std::string what() const;
};

class ContentDecodingError : public RequestError
{
public:
    virtual std::string what() const;
};

class StreamConsumedError : public RequestError
{
public:
    virtual std::string what() const;
};

class RetryError : public RequestError
{
public:
    virtual std::string what() const;
};

class UnrewindableBodyError : public RequestError
{
public:
    virtual std::string what() const;
};

}  // namespace qtng

#endif  // QTNG_HTTP_H
