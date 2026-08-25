#ifndef QTNG_HTTP_UTILS_H
#define QTNG_HTTP_UTILS_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/socket_utils.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/string_utils.h"

namespace qtng {

enum HttpVersion {
    Unknown = 0,
    Http1_0 = 1,
    Http1_1 = 2,
    Http2_0 = 3,
    http3_0 = 4,
};

enum HttpStatus {
    Continue = 100,
    SwitchProtocol = 101,
    Processing = 102,

    OK = 200,
    Created = 201,
    Accepted = 202,
    NonAuthoritative = 203,
    NoContent = 204,
    ResetContent = 205,
    PartialContent = 206,
    MultiStatus = 207,
    AlreadyReported = 208,
    IMUsed = 226,

    MultipleChoices = 300,
    MovedPermanently = 301,
    Found = 302,
    SeeOther = 303,
    NotModified = 304,
    UseProxy = 305,
    TemporaryRedirect = 307,
    PermanentRedirect = 308,

    BadRequest = 400,
    Unauthorized = 401,
    PaymentRequired = 402,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    NotAcceptable = 406,
    ProxyAuthenticationRequired = 407,
    RequestTimeout = 408,
    Conflict = 409,
    Gone = 410,
    LengthRequired = 411,
    PreconditionFailed = 412,
    RequestEntityTooLarge = 413,
    RequestURITooLong = 414,
    UnsupportedMediaType = 415,
    RequestedRangeNotSatisfiable = 416,
    ExpectationFailed = 417,
    ImaTeapot = 418,
    UnprocessableEntity = 422,
    Locked = 423,
    FailedDependency = 424,
    UpgradeRequired = 426,
    PreconditionRequired = 428,
    TooManyRequests = 429,
    RequestHeaderFieldsTooLarge = 441,

    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504,
    HTTPVersionNotSupported = 505,
    VariantAlsoNegotiates = 506,
    InsufficientStorage = 507,
    LoopDetected = 508,
    NotExtended = 510,
    NetworkAuthenticationRequired = 511,
};

bool toMessage(HttpStatus status, std::string *shortMessage, std::string *longMessage);

enum KnownHeader {
    ContentTypeHeader,
    ContentLengthHeader,
    ContentEncodingHeader,
    TransferEncodingHeader,
    LocationHeader,
    LastModifiedHeader,
    CookieHeader,
    SetCookieHeader,
    ContentDispositionHeader,  // added for QMultipartMessage
    ServerHeader,
    UserAgentHeader,
    AcceptHeader,
    AcceptLanguageHeader,
    AcceptQueryHeader,  // RFC 10008: media types a resource accepts for QUERY requests
    AcceptEncodingHeader,
    PragmaHeader,
    CacheControlHeader,
    DateHeader,
    AllowHeader,
    VaryHeader,
    FrameOptionsHeader,
    MIMEVersionHeader,
    ConnectionHeader,
    UpgradeHeader,
    HostHeader,
};

std::string normalizeHeaderName(const std::string &headerName);
qtng::utils::DateTime fromHttpDate(const std::string &value);
std::string toHttpDate(const qtng::utils::DateTime &dt);
std::string toString(KnownHeader knownHeader);

struct HttpHeader
{
    HttpHeader(const std::string &name, const std::string &value)
        : name_(name)
        , value_(value)
    {
    }
    HttpHeader() { }
    bool isValid() const { return !name_.empty(); }
    std::string name() const { return name_; }
    void setName(const std::string &name) { name_ = name; }
    std::string value() const { return value_; }
    void setValue(const std::string &value) { value_ = value; }
private:
    std::string name_;
    std::string value_;
};

template<typename Base>
class WithHttpHeaders : public Base
{
public:
    void setContentType(const std::string &contentType);
    std::string contentType() const;
    void setContentLength(std::int64_t contentLength);
    std::int64_t contentLength() const;
    void setLocation(const std::string &url);
    std::string location() const;
    void setLastModified(const qtng::utils::DateTime &lastModified);
    qtng::utils::DateTime lastModified() const;
    void setModifiedSince(const qtng::utils::DateTime &modifiedSince);
    qtng::utils::DateTime modifiedSince() const;

    void setHeader(const std::string &name, const std::string &value);
    void addHeader(const std::string &name, const std::string &value);
    void addHeader(const HttpHeader &header);
    bool hasHeader(const std::string &name) const;
    bool removeHeader(const std::string &name);
    void setHeader(KnownHeader header, const std::string &value);
    void addHeader(KnownHeader header, const std::string &value);
    bool hasHeader(KnownHeader header) const;
    bool removeHeader(KnownHeader header);
    std::string header(const std::string &name, const std::string &defaultValue = std::string()) const;
    std::string header(KnownHeader header, const std::string &defaultValue = std::string()) const;
    std::vector<std::string> multiHeader(const std::string &headerName) const;
    std::vector<std::string> multiHeader(KnownHeader header) const;
    std::vector<HttpHeader> allHeaders() const { return headers; }
    void setHeaders(const std::map<std::string, std::string> headers);
    void setHeaders(const std::vector<HttpHeader> &headers) { this->headers = headers; }
protected:
    std::vector<HttpHeader> headers;
};
class EmptyClass
{
};
class HttpHeaderManager : public WithHttpHeaders<EmptyClass>
{
};

template<typename Base>
void WithHttpHeaders<Base>::setContentLength(std::int64_t contentLength)
{
    setHeader("Content-Length", qtng::utils::number(static_cast<long long>(contentLength)));
}

template<typename Base>
std::int64_t WithHttpHeaders<Base>::contentLength() const
{
    bool ok = false;
    std::string s = header("Content-Length");
    long long parsed = qtng::utils::parseLongLong(s, &ok);
    if (ok) {
        const std::int64_t l = static_cast<std::int64_t>(parsed);
        if (l >= 0) {
            return l;
        } else {
            return -1;
        }
    } else {
        return -1;
    }
}

template<typename Base>
void WithHttpHeaders<Base>::setContentType(const std::string &contentType)
{
    setHeader("Content-Type", contentType);
}

template<typename Base>
std::string WithHttpHeaders<Base>::contentType() const
{
    return header("Content-Type", "text/plain");
}

template<typename Base>
std::string WithHttpHeaders<Base>::location() const
{
    const std::string &value = header("Location");
    if (value.empty()) {
        return std::string();
    }
    return value;
}

template<typename Base>
void WithHttpHeaders<Base>::setLocation(const std::string &url)
{
    setHeader("Location", url);
}

template<typename Base>
qtng::utils::DateTime WithHttpHeaders<Base>::lastModified() const
{
    const std::string &value = header("Last-Modified");
    if (value.empty()) {
        return qtng::utils::DateTime();
    }
    return fromHttpDate(value);
}

template<typename Base>
void WithHttpHeaders<Base>::setLastModified(const qtng::utils::DateTime &lastModified)
{
    setHeader("Last-Modified", toHttpDate(lastModified));
}

template<typename Base>
void WithHttpHeaders<Base>::setModifiedSince(const qtng::utils::DateTime &modifiedSince)
{
    setHeader("Modified-Since", toHttpDate(modifiedSince));
}

template<typename Base>
qtng::utils::DateTime WithHttpHeaders<Base>::modifiedSince() const
{
    const std::string &value = header("Modified-Since");
    if (value.empty()) {
        return qtng::utils::DateTime();
    }
    return fromHttpDate(value);
}

template<typename Base>
bool WithHttpHeaders<Base>::hasHeader(const std::string &headerName) const
{
    for (int i = 0; i < headers.size(); ++i) {
        const HttpHeader &header = headers[i];
        if (qtng::utils::equalsIgnoreCase(header.name(), headerName)) {
            return true;
        }
    }
    return false;
}

template<typename Base>
bool WithHttpHeaders<Base>::removeHeader(const std::string &headerName)
{
    for (int i = 0; i < headers.size(); ++i) {
        const HttpHeader &header = headers[i];
        if (qtng::utils::equalsIgnoreCase(header.name(), headerName)) {
            headers.erase(headers.begin() + i);
            return true;
        }
    }
    return false;
}

template<typename Base>
void WithHttpHeaders<Base>::setHeader(const std::string &name, const std::string &value)
{
    removeHeader(name);
    addHeader(name, value);
}

template<typename Base>
void WithHttpHeaders<Base>::addHeader(const std::string &name, const std::string &value)
{
    headers.push_back(HttpHeader(normalizeHeaderName(name), value));
}

template<typename Base>
void WithHttpHeaders<Base>::addHeader(const HttpHeader &header)
{
    headers.push_back(header);
}

template<typename Base>
void WithHttpHeaders<Base>::setHeader(KnownHeader header, const std::string &value)
{
    setHeader(toString(header), value);
}

template<typename Base>
void WithHttpHeaders<Base>::addHeader(KnownHeader header, const std::string &value)
{
    addHeader(toString(header), value);
}

template<typename Base>
bool WithHttpHeaders<Base>::hasHeader(KnownHeader header) const
{
    return hasHeader(toString(header));
}

template<typename Base>
bool WithHttpHeaders<Base>::removeHeader(KnownHeader header)
{
    return removeHeader(toString(header));
}

template<typename Base>
std::string WithHttpHeaders<Base>::header(const std::string &headerName, const std::string &defaultValue) const
{
    for (int i = 0; i < headers.size(); ++i) {
        const HttpHeader &header = headers[i];
        if (qtng::utils::equalsIgnoreCase(header.name(), headerName)) {
            return header.value();
        }
    }
    return defaultValue;
}

template<typename Base>
std::string WithHttpHeaders<Base>::header(KnownHeader knownHeader, const std::string &defaultValue) const
{
    return header(toString(knownHeader), defaultValue);
}

template<typename Base>
std::vector<std::string> WithHttpHeaders<Base>::multiHeader(const std::string &headerName) const
{
    std::vector<std::string> values;
    for (int i = 0; i < headers.size(); ++i) {
        const HttpHeader &header = headers[i];
        if (qtng::utils::equalsIgnoreCase(header.name(), headerName)) {
            values.push_back(header.value());
        }
    }
    return values;
}

template<typename Base>
std::vector<std::string> WithHttpHeaders<Base>::multiHeader(KnownHeader header) const
{
    return multiHeader(toString(header));
}

template<typename Base>
void WithHttpHeaders<Base>::setHeaders(const std::map<std::string, std::string> hdrs)
{
    this->headers.clear();
    for (const auto &entry : hdrs) {
        this->headers.push_back(HttpHeader(normalizeHeaderName(entry.first), entry.second));
    }
}

std::vector<std::string> splitBytes(const std::string &bs, char sep, int maxSplit = -1);

class HeaderSplitter
{
public:
    enum Error {
        NoError,
        EncodingError,
        ExhausedMaxLine,
        ConnectionError,
        LineTooLong,
    };
public:
    HeaderSplitter(std::shared_ptr<SocketLike> connection, const std::string &buf, int debugLevel = 0)
        : connection_(connection)
        , buf_(buf)
        , debugLevel_(debugLevel)
    {
    }
    HeaderSplitter(std::shared_ptr<SocketLike> connection, int debugLevel = 0)
        : connection_(connection)
        , debugLevel_(debugLevel)
    {
    }
    std::string nextLine(Error *error);
    HttpHeader nextHeader(Error *error);
    std::vector<HttpHeader> headers(int maxHeaders, Error *error);
    std::string buf() const { return buf_; }
    void setBuf(const std::string &buf) { buf_ = buf; }
private:
    NG_DISABLE_COPY_MOVE(HeaderSplitter)
    std::shared_ptr<SocketLike> connection_;
    std::string buf_;
    int debugLevel_;
};

class ChunkedBlockReader
{
public:
    enum Error {
        NoError,
        ChunkedEncodingError,
        UnrewindableBodyError,
        ConnectionError,
    };
public:
    ChunkedBlockReader(std::shared_ptr<FileLike> connection, const std::string &buf)
        : connection_(connection)
        , buf_(buf)
    {
    }
public:
    std::string nextBlock(std::int64_t leftBytes, Error *error);
private:
    NG_DISABLE_COPY_MOVE(ChunkedBlockReader)
    std::shared_ptr<FileLike> connection_;
    std::string buf_;
};

class PlainBodyFile : public FileLike
{
public:
    PlainBodyFile(std::int64_t contentLength, const std::string &partialBody, std::shared_ptr<SocketLike> stream);
    virtual std::int32_t read(char *data, std::int32_t size) override;
    virtual std::int32_t write(const char *, std::int32_t) override { return -1; }
    virtual void close() override { }
    virtual std::int64_t size() override { return contentLength(); }
    std::int64_t contentLength() const { return contentLength_; }
private:
    NG_DISABLE_COPY_MOVE(PlainBodyFile)
    const std::int64_t contentLength_;
    const std::shared_ptr<SocketLike> stream_;
    std::string partialBody_;
    std::int64_t count_;
};

class ChunkedBodyFile : public FileLike
{
public:
    ChunkedBodyFile(std::int64_t maxBodySize, const std::string &partialBody, std::shared_ptr<FileLike> stream);
    virtual std::int32_t read(char *data, std::int32_t size) override;
    virtual std::int32_t write(const char *, std::int32_t) override { return -1; }
    virtual void close() override { }
    virtual std::int64_t size() override { return -1; }
    ChunkedBlockReader::Error error() const { return error_; }
private:
    NG_DISABLE_COPY_MOVE(ChunkedBodyFile)
    ChunkedBlockReader reader_;
    ChunkedBlockReader::Error error_;
    std::string buf_;
    std::int64_t maxBodySize_;
    std::int64_t count_;
    bool eof_;
};

class ChunkedWriter: public FileLike
{
public:
    ChunkedWriter(std::shared_ptr<FileLike> stream)
        : stream_(stream) {}
    virtual ~ChunkedWriter() override;
    virtual std::int32_t read(char *, std::int32_t) override { return -1; }
    virtual std::int32_t write(const char *data, std::int32_t size) override;
    virtual void close() override;
    virtual std::int64_t size() override { return -1; }
private:
    NG_DISABLE_COPY_MOVE(ChunkedWriter)
    std::shared_ptr<FileLike> stream_;
};
}  // namespace qtng

#endif  // QTNG_HTTP_UTILS_H
