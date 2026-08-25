#include "bridge/core_access.h"
#include "bridge/http_access.h"
#include "bridge/stream_bridge.h"
#include "http_proxy.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class HttpProxyPrivate
{
public:
    qtng_core::HttpProxy core;

    static qtng_core::HttpProxy *coreOf(HttpProxy *proxy) { return proxy ? &proxy->d_ptr->core : nullptr; }
    static const qtng_core::HttpProxy *coreOf(const HttpProxy *proxy) { return proxy ? &proxy->d_ptr->core : nullptr; }
};

HttpProxy::HttpProxy()
    : d_ptr(new HttpProxyPrivate)
{
}

HttpProxy::HttpProxy(const QString &hostName, quint16 port, const QString &user, const QString &password)
    : d_ptr(new HttpProxyPrivate)
{
    d_ptr->core = qtng_core::HttpProxy(toStdString(hostName), port, toStdString(user), toStdString(password));
}

HttpProxy::HttpProxy(const HttpProxy &other)
    : d_ptr(new HttpProxyPrivate)
{
    d_ptr->core = other.d_ptr->core;
    headers = other.headers;
}

HttpProxy::~HttpProxy()
{
    delete d_ptr;
}

QSharedPointer<SocketLike> HttpProxy::connect(const QString &remoteHost, quint16 port)
{
    return toQtSocketLike(d_ptr->core.connect(toStdString(remoteHost), port));
}

QSharedPointer<SocketLike> HttpProxy::connect(const HostAddress &remoteHost, quint16 port)
{
    return toQtSocketLike(d_ptr->core.connect(toCoreAddress(remoteHost), port));
}

QString HttpProxy::hostName() const
{
    return toQString(d_ptr->core.hostName());
}

quint16 HttpProxy::port() const
{
    return d_ptr->core.port();
}

QString HttpProxy::user() const
{
    return toQString(d_ptr->core.user());
}

QString HttpProxy::password() const
{
    return toQString(d_ptr->core.password());
}

void HttpProxy::setHostName(const QString &hostName)
{
    d_ptr->core.setHostName(toStdString(hostName));
}

void HttpProxy::setPort(quint16 port)
{
    d_ptr->core.setPort(port);
}

void HttpProxy::setUser(const QString &user)
{
    d_ptr->core.setUser(toStdString(user));
}

void HttpProxy::setPassword(const QString &password)
{
    d_ptr->core.setPassword(toStdString(password));
}

HttpProxy &HttpProxy::operator=(const HttpProxy &other)
{
    if (this != &other) {
        d_ptr->core = other.d_ptr->core;
        headers = other.headers;
    }
    return *this;
}

HttpProxy &HttpProxy::operator=(HttpProxy &&other)
{
    qSwap(d_ptr, other.d_ptr);
    headers = std::move(other.headers);
    return *this;
}

bool HttpProxy::operator==(const HttpProxy &other) const
{
    return d_ptr->core == other.d_ptr->core && headers == other.headers;
}

BaseProxySwitcher::BaseProxySwitcher() = default;
BaseProxySwitcher::~BaseProxySwitcher() = default;

QSharedPointer<SocketProxy> SimpleProxySwitcher::selectSocketProxy(const QUrl &url)
{
    (void)url;
    if (!socketProxies.isEmpty()) {
        return socketProxies.front();
    }
    if (!httpProxies.isEmpty()) {
        return httpProxies.front();
    }
    return QSharedPointer<SocketProxy>();
}

QSharedPointer<HttpProxy> SimpleProxySwitcher::selectHttpProxy(const QUrl &url)
{
    (void)url;
    if (!httpProxies.isEmpty()) {
        return httpProxies.front();
    }
    return QSharedPointer<HttpProxy>();
}

}  // namespace QTNETWORKNG_NAMESPACE

namespace qtng_bridge {

qtng_core::HttpProxy *httpProxyCoreOf(QTNETWORKNG_NAMESPACE::HttpProxy *proxy)
{
    return QTNETWORKNG_NAMESPACE::HttpProxyPrivate::coreOf(proxy);
}

const qtng_core::HttpProxy *httpProxyCoreOf(const QTNETWORKNG_NAMESPACE::HttpProxy *proxy)
{
    return QTNETWORKNG_NAMESPACE::HttpProxyPrivate::coreOf(proxy);
}

}  // namespace qtng_bridge
