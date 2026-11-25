#ifndef QTNG_QT_BRIDGE_HTTP_ACCESS_H
#define QTNG_QT_BRIDGE_HTTP_ACCESS_H

#include "bridge/core_access.h"
#include "http_utils.h"

namespace QTNETWORKNG_NAMESPACE {
class HttpCookieJar;
class HttpProxy;
class HttpResponse;
class WebSocketConfiguration;
}

namespace qtng_bridge {

inline QTNG_NAMESPACE::HttpHeader toCoreHeader(const QTNETWORKNG_NAMESPACE::HttpHeader &h)
{
    return QTNG_NAMESPACE::HttpHeader(toStdString(h.name()), toStdString(h.value()));
}

inline QTNETWORKNG_NAMESPACE::HttpHeader toQtHeader(const QTNG_NAMESPACE::HttpHeader &h)
{
    return QTNETWORKNG_NAMESPACE::HttpHeader(toQString(h.name()), toQByteArray(h.value()));
}

inline std::vector<QTNG_NAMESPACE::HttpHeader> toCoreHeaders(const QList<QTNETWORKNG_NAMESPACE::HttpHeader> &headers)
{
    std::vector<QTNG_NAMESPACE::HttpHeader> result;
    result.reserve(static_cast<std::size_t>(headers.size()));
    for (const QTNETWORKNG_NAMESPACE::HttpHeader &h : headers) {
        result.push_back(toCoreHeader(h));
    }
    return result;
}

inline QList<QTNETWORKNG_NAMESPACE::HttpHeader> toQtHeaders(const std::vector<QTNG_NAMESPACE::HttpHeader> &headers)
{
    QList<QTNETWORKNG_NAMESPACE::HttpHeader> result;
    result.reserve(static_cast<int>(headers.size()));
    for (const QTNG_NAMESPACE::HttpHeader &h : headers) {
        result.append(toQtHeader(h));
    }
    return result;
}

QTNG_NAMESPACE::HttpProxy *httpProxyCoreOf(QTNETWORKNG_NAMESPACE::HttpProxy *proxy);
const QTNG_NAMESPACE::HttpProxy *httpProxyCoreOf(const QTNETWORKNG_NAMESPACE::HttpProxy *proxy);

std::shared_ptr<QTNETWORKNG_NAMESPACE::HttpCookieJar>
httpCookieJarFromCore(std::shared_ptr<QTNG_NAMESPACE::HttpCookieJar> core);
std::shared_ptr<QTNETWORKNG_NAMESPACE::WebSocketConfiguration>
webSocketConfigurationFromCore(std::shared_ptr<QTNG_NAMESPACE::WebSocketConfiguration> core);
std::shared_ptr<QTNG_NAMESPACE::WebSocketConfiguration>
webSocketConfigurationToCore(const std::shared_ptr<QTNETWORKNG_NAMESPACE::WebSocketConfiguration> &config);

QTNETWORKNG_NAMESPACE::HttpResponse httpResponseFromCore(QTNG_NAMESPACE::HttpResponse core);
// Deep copy from the Qt wrapper back to the core type: the core copy constructors
// share private state, and a handler may still hold the wrapper afterwards.
QTNG_NAMESPACE::HttpResponse httpResponseToCore(const QTNETWORKNG_NAMESPACE::HttpResponse &response);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_HTTP_ACCESS_H
