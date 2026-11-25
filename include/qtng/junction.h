#ifndef QTNG_JUNCTION_H
#define QTNG_JUNCTION_H

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qtng/httpd.h"
#include "qtng/http.h"

#include "qtng/config.h"
QTNG_NAMESPACE_BEGIN

class HttpJunctionPrivate;
class HttpJunction
{
public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;
    using Middleware = std::function<HttpResponse(const HttpRequest &, const Handler &)>;
    using ExceptionHandler = std::function<HttpResponse(const HttpRequest &, const std::exception_ptr &)>;

public:
    HttpJunction();
    ~HttpJunction();
public:
    void use(Middleware middleware);
    // Mounts `sub` under `prefix`. `sub` is not owned and must outlive every
    // dispatch of this junction. The prefix is normalized: it always starts
    // with '/', a trailing '/' is dropped.
    void group(const std::string &prefix, HttpJunction *sub);

    void get(const std::string &path, Handler handler);
    void head(const std::string &path, Handler handler);
    void post(const std::string &path, Handler handler);
    void put(const std::string &path, Handler handler);
    void patch(const std::string &path, Handler handler);
    void delete_(const std::string &path, Handler handler);
    void options(const std::string &path, Handler handler);
    void query(const std::string &path, Handler handler);
    void all(const std::string &path, Handler handler);
    void handle(const std::string &method, const std::string &path, Handler handler);

    void setNotFoundHandler(Handler handler);
    void setMethodNotAllowedHandler(Handler handler);
    void setExceptionHandler(ExceptionHandler handler);
public:
    // Resolves and runs a request without touching a socket, which is what
    // makes the junction unit testable.
    HttpResponse dispatch(const HttpRequest &request);

    std::vector<std::string> allowedMethods(const std::string &path) const;
    std::vector<std::string> routes() const;
private:
    HttpJunctionPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(HttpJunction)
    NG_DISABLE_COPY_MOVE(HttpJunction)
};

// HTTP request handler for BaseStreamServer subclasses (TcpServer, KcpServer,
// UtpServer, SslServer). The transport is the caller's choice; this class only
// depends on the BaseRequestHandler protocol, so it must stay default
// constructible.
class JunctionRequestHandler : public BaseHttpRequestHandler
{
public:
    JunctionRequestHandler();
    virtual ~JunctionRequestHandler() override;
protected:
    virtual void doMethod() override;
protected:
    // Defaults to userData<HttpJunction>(); override it to pick the junction up
    // from anywhere else.
    virtual HttpJunction *junction() const;
    // Writes the response: status line, headers, cookies, then the body. A body
    // file is streamed with sendfile(); an unknown size is sent chunked (closed
    // delimited for an HTTP/1.0 peer).
    virtual bool writeResponse(const HttpResponse &response);
private:
    NG_DISABLE_COPY_MOVE(JunctionRequestHandler)
};

QTNG_NAMESPACE_END
#endif  // QTNG_JUNCTION_H
