#ifndef QTNG_HTTP2_P_H
#define QTNG_HTTP2_P_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/http.h"
#include "qtng/socket_utils.h"

namespace qtng {

class HttpSessionPrivate;
class Http2ClientSessionPrivate;

class Http2ClientSession
{
public:
    explicit Http2ClientSession(std::shared_ptr<SocketLike> connection, int debugLevel = 0);
    ~Http2ClientSession();
    bool start();
    bool isValid() const;
    void close();
    // Perform one HTTP/2 request on a new stream.
    void exchange(HttpSessionPrivate *session, HttpRequest &request, HttpResponse &response);
private:
    std::shared_ptr<Http2ClientSessionPrivate> d;
    friend class Http2ClientSessionPrivate;
    friend class Http2Protocol;
};

}  // namespace qtng

#endif  // QTNG_HTTP2_P_H
