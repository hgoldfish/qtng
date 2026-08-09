#ifndef QTNG_HTTP_PROTOCOL_P_H
#define QTNG_HTTP_PROTOCOL_P_H

#include <memory>
#include <string>
#include <vector>

#include "qtng/http.h"
#include "qtng/locks.h"
#include "qtng/socket_utils.h"

namespace qtng {

class HttpSessionPrivate;

// Wire-protocol exchange for one request/response. Session keeps cookies,
// redirects, cache and connection-pool orchestration.
class HttpProtocol
{
public:
    virtual ~HttpProtocol() { }
    virtual HttpVersion version() const = 0;
    // Fills response on the given connection. May recycle the connection via session.
    // ptrLock is the per-origin semaphore lock for HTTP/1 (may be null if request.connection()).
    virtual void exchange(HttpSessionPrivate *session, HttpRequest &request, HttpResponse &response,
                          std::shared_ptr<SocketLike> connection,
                          std::unique_ptr<ScopedLock<Semaphore>> &ptrLock) = 0;
};

class Http1Protocol : public HttpProtocol
{
public:
    virtual HttpVersion version() const override { return HttpVersion::Http1_1; }
    virtual void exchange(HttpSessionPrivate *session, HttpRequest &request, HttpResponse &response,
                          std::shared_ptr<SocketLike> connection,
                          std::unique_ptr<ScopedLock<Semaphore>> &ptrLock) override;
    static std::vector<HttpHeader> makeHeaders(HttpSessionPrivate *session, HttpRequest &request,
                                               const std::string &urlStr);
};

class Http2Protocol : public HttpProtocol
{
public:
    virtual HttpVersion version() const override { return HttpVersion::Http2_0; }
    virtual void exchange(HttpSessionPrivate *session, HttpRequest &request, HttpResponse &response,
                          std::shared_ptr<SocketLike> connection,
                          std::unique_ptr<ScopedLock<Semaphore>> &ptrLock) override;
};

class Http3Protocol : public HttpProtocol
{
public:
    virtual HttpVersion version() const override { return HttpVersion::http3_0; }
    virtual void exchange(HttpSessionPrivate *session, HttpRequest &request, HttpResponse &response,
                          std::shared_ptr<SocketLike> connection,
                          std::unique_ptr<ScopedLock<Semaphore>> &ptrLock) override;
};

}  // namespace qtng

#endif  // QTNG_HTTP_PROTOCOL_P_H
