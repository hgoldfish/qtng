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

inline qtng_core::HttpHeader toCoreHeader(const QTNETWORKNG_NAMESPACE::HttpHeader &h)
{
    return qtng_core::HttpHeader(toStdString(h.name), toStdString(h.value));
}

inline QTNETWORKNG_NAMESPACE::HttpHeader toQtHeader(const qtng_core::HttpHeader &h)
{
    return QTNETWORKNG_NAMESPACE::HttpHeader(toQString(h.name), toQByteArray(h.value));
}

inline std::vector<qtng_core::HttpHeader> toCoreHeaders(const QList<QTNETWORKNG_NAMESPACE::HttpHeader> &headers)
{
    std::vector<qtng_core::HttpHeader> result;
    result.reserve(static_cast<std::size_t>(headers.size()));
    for (const QTNETWORKNG_NAMESPACE::HttpHeader &h : headers) {
        result.push_back(toCoreHeader(h));
    }
    return result;
}

inline QList<QTNETWORKNG_NAMESPACE::HttpHeader> toQtHeaders(const std::vector<qtng_core::HttpHeader> &headers)
{
    QList<QTNETWORKNG_NAMESPACE::HttpHeader> result;
    result.reserve(static_cast<int>(headers.size()));
    for (const qtng_core::HttpHeader &h : headers) {
        result.append(toQtHeader(h));
    }
    return result;
}

qtng_core::HttpProxy *httpProxyCoreOf(QTNETWORKNG_NAMESPACE::HttpProxy *proxy);
const qtng_core::HttpProxy *httpProxyCoreOf(const QTNETWORKNG_NAMESPACE::HttpProxy *proxy);

void bindHttpCookieJarToCore(QTNETWORKNG_NAMESPACE::HttpCookieJar *jar, qtng_core::HttpCookieJar *core);
void bindWebSocketConfiguration(QTNETWORKNG_NAMESPACE::WebSocketConfiguration *config, qtng_core::WebSocketConfiguration *core);

QTNETWORKNG_NAMESPACE::HttpResponse httpResponseFromCore(qtng_core::HttpResponse core);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_HTTP_ACCESS_H
