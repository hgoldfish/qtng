#include "bridge/core_access.h"
#include "bridge/stream_bridge.h"
#include "socks5_proxy.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class Socks5ProxyPrivate
{
public:
    qtng_core::Socks5Proxy core;
};

Socks5Exception::Error Socks5Exception::error() const
{
    return static_cast<Error>(err);
}

QString Socks5Exception::errorString() const
{
    return toQString(qtng_core::Socks5Exception(static_cast<qtng_core::Socks5Exception::Error>(err)).errorString());
}

Socks5Proxy::Socks5Proxy()
    : d_ptr(new Socks5ProxyPrivate)
{
}

Socks5Proxy::Socks5Proxy(const QString &hostName, quint16 port, const QString &user, const QString &password)
    : d_ptr(new Socks5ProxyPrivate)
{
    d_ptr->core = qtng_core::Socks5Proxy(toStdString(hostName), port, toStdString(user), toStdString(password));
}

Socks5Proxy::Socks5Proxy(const Socks5Proxy &other)
    : d_ptr(new Socks5ProxyPrivate)
{
    d_ptr->core = other.d_ptr->core;
}

Socks5Proxy::~Socks5Proxy()
{
    delete d_ptr;
}

QSharedPointer<SocketLike> Socks5Proxy::connect(const QString &remoteHost, quint16 port)
{
    return toQtSocketLike(d_ptr->core.connect(toStdString(remoteHost), port));
}

QSharedPointer<SocketLike> Socks5Proxy::connect(const HostAddress &remoteHost, quint16 port)
{
    return toQtSocketLike(d_ptr->core.connect(toCoreAddress(remoteHost), port));
}

QSharedPointer<SocketLike> Socks5Proxy::listen(quint16 port)
{
    return toQtSocketLike(d_ptr->core.listen(port));
}

bool Socks5Proxy::isNull() const
{
    return d_ptr->core.isNull();
}

Socks5Proxy::Capabilities Socks5Proxy::capabilities() const
{
    return static_cast<Capabilities>(d_ptr->core.capabilities());
}

QString Socks5Proxy::hostName() const
{
    return toQString(d_ptr->core.hostName());
}

quint16 Socks5Proxy::port() const
{
    return d_ptr->core.port();
}

QString Socks5Proxy::user() const
{
    return toQString(d_ptr->core.user());
}

QString Socks5Proxy::password() const
{
    return toQString(d_ptr->core.password());
}

void Socks5Proxy::setCapabilities(Capabilities capabilities)
{
    d_ptr->core.setCapabilities(capabilities);
}

void Socks5Proxy::setHostName(const QString &hostName)
{
    d_ptr->core.setHostName(toStdString(hostName));
}

void Socks5Proxy::setPort(quint16 port)
{
    d_ptr->core.setPort(port);
}

void Socks5Proxy::setUser(const QString &user)
{
    d_ptr->core.setUser(toStdString(user));
}

void Socks5Proxy::setPassword(const QString &password)
{
    d_ptr->core.setPassword(toStdString(password));
}

Socks5Proxy &Socks5Proxy::operator=(const Socks5Proxy &other)
{
    if (this != &other) {
        d_ptr->core = other.d_ptr->core;
    }
    return *this;
}

Socks5Proxy &Socks5Proxy::operator=(Socks5Proxy &&other)
{
    qSwap(d_ptr, other.d_ptr);
    return *this;
}

bool Socks5Proxy::operator==(const Socks5Proxy &other) const
{
    return d_ptr->core == other.d_ptr->core;
}

}  // namespace QTNETWORKNG_NAMESPACE
