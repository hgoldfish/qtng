#include "bridge/core_access.h"
#include "bridge/stream_bridge.h"
#include "socket_server.h"
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
    try {
        return toQtSocketLike(d_ptr->core.connect(toStdString(remoteHost), port));
    } catch (const qtng_core::Socks5Exception &e) {
        throw Socks5Exception(static_cast<Socks5Exception::Error>(e.error()));
    }
}

QSharedPointer<SocketLike> Socks5Proxy::connect(const HostAddress &remoteHost, quint16 port)
{
    try {
        return toQtSocketLike(d_ptr->core.connect(toCoreAddress(remoteHost), port));
    } catch (const qtng_core::Socks5Exception &e) {
        throw Socks5Exception(static_cast<Socks5Exception::Error>(e.error()));
    }
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

class QtSocks5RequestHandlerCoreBridge : public qtng_core::Socks5RequestHandler
{
public:
    explicit QtSocks5RequestHandlerCoreBridge(QTNETWORKNG_NAMESPACE::Socks5RequestHandler *q)
        : q(q)
    {
    }

    // non-virtual core helpers, called from the Qt default implementations.
    void handleCore() { qtng_core::Socks5RequestHandler::handle(); }
    bool sendConnectReplyCore(const qtng_core::HostAddress &hostAddress, quint16 port)
    {
        return qtng_core::Socks5RequestHandler::sendConnectReply(hostAddress, port);
    }
    bool sendFailedReplyCore() { return qtng_core::Socks5RequestHandler::sendFailedReply(); }
    void doConnectCore(const string &hostName, const qtng_core::HostAddress &hostAddress, uint16_t port)
    {
        qtng_core::Socks5RequestHandler::doConnect(hostName, hostAddress, port);
    }
    void doFailedCore(const string &hostName, const qtng_core::HostAddress &hostAddress, uint16_t port)
    {
        qtng_core::Socks5RequestHandler::doFailed(hostName, hostAddress, port);
    }
    shared_ptr<qtng_core::SocketLike> makeConnectionCore(const string &hostName,
                                                         const qtng_core::HostAddress &hostAddress, uint16_t port,
                                                         qtng_core::HostAddress *forwardAddress)
    {
        return qtng_core::Socks5RequestHandler::makeConnection(hostName, hostAddress, port, forwardAddress);
    }
    void logProxyCore(const string &hostName, const qtng_core::HostAddress &hostAddress, uint16_t port,
                      const qtng_core::HostAddress &forwardAddress, bool success)
    {
        qtng_core::Socks5RequestHandler::logProxy(hostName, hostAddress, port, forwardAddress, success);
    }
    void exchangeCore(shared_ptr<qtng_core::SocketLike> request, shared_ptr<qtng_core::SocketLike> forward)
    {
        qtng_core::Socks5RequestHandler::exchange(request, forward);
    }

    // virtual methods: forward to the Qt side so that user overrides are honored.
    void handle() override { q->handle(); }
    void doConnect(const string &hostName, const qtng_core::HostAddress &hostAddress, uint16_t port) override
    {
        q->doConnect(toQString(hostName), toQtAddress(hostAddress), port);
    }
    void doFailed(const string &hostName, const qtng_core::HostAddress &hostAddress, uint16_t port) override
    {
        q->doFailed(toQString(hostName), toQtAddress(hostAddress), port);
    }
    shared_ptr<qtng_core::SocketLike> makeConnection(const string &hostName, const qtng_core::HostAddress &hostAddress,
                                                     uint16_t port, qtng_core::HostAddress *forwardAddress) override
    {
        HostAddress qtForwardAddress;
        const QSharedPointer<SocketLike> qtResult =
                q->makeConnection(toQString(hostName), toQtAddress(hostAddress), port,
                                  forwardAddress ? &qtForwardAddress : nullptr);
        if (forwardAddress) {
            *forwardAddress = toCoreAddress(qtForwardAddress);
        }
        return toCoreSocketLike(qtResult);
    }
    void logProxy(const string &hostName, const qtng_core::HostAddress &hostAddress, uint16_t port,
                  const qtng_core::HostAddress &forwardAddress, bool success) override
    {
        q->logProxy(toQString(hostName), toQtAddress(hostAddress), port, toQtAddress(forwardAddress), success);
    }
    void exchange(shared_ptr<qtng_core::SocketLike> request, shared_ptr<qtng_core::SocketLike> forward) override
    {
        q->exchange(toQtSocketLike(request), toQtSocketLike(forward));
    }

private:
    QTNETWORKNG_NAMESPACE::Socks5RequestHandler * const q;
};

Socks5RequestHandler::Socks5RequestHandler()
    : coreBridge(new QtSocks5RequestHandlerCoreBridge(this))
{
}

Socks5RequestHandler::~Socks5RequestHandler()
{
    delete coreBridge;
}

void Socks5RequestHandler::handle()
{
    if (!request) {
        return;
    }
    coreBridge->request = toCoreSocketLike(request);
    coreBridge->handleCore();
}

void Socks5RequestHandler::doConnect(const QString &hostName, const HostAddress &hostAddress, quint16 port)
{
    coreBridge->doConnectCore(toStdString(hostName), toCoreAddress(hostAddress), port);
}

void Socks5RequestHandler::doFailed(const QString &hostName, const HostAddress &hostAddress, quint16 port)
{
    coreBridge->doFailedCore(toStdString(hostName), toCoreAddress(hostAddress), port);
}

QSharedPointer<SocketLike> Socks5RequestHandler::makeConnection(const QString &hostName, const HostAddress &hostAddress,
                                                                quint16 port, HostAddress *forwardAddress)
{
    qtng_core::HostAddress coreForwardAddress;
    const shared_ptr<qtng_core::SocketLike> core = coreBridge->makeConnectionCore(
            toStdString(hostName), toCoreAddress(hostAddress), port, forwardAddress ? &coreForwardAddress : nullptr);
    if (forwardAddress) {
        *forwardAddress = toQtAddress(coreForwardAddress);
    }
    return toQtSocketLike(core);
}

void Socks5RequestHandler::logProxy(const QString &hostName, const HostAddress &hostAddress, quint16 port,
                                    const HostAddress &forwardAddress, bool success)
{
    coreBridge->logProxyCore(toStdString(hostName), toCoreAddress(hostAddress), port, toCoreAddress(forwardAddress),
                             success);
}

void Socks5RequestHandler::exchange(QSharedPointer<SocketLike> request, QSharedPointer<SocketLike> forward)
{
    coreBridge->exchangeCore(toCoreSocketLike(request), toCoreSocketLike(forward));
}

bool Socks5RequestHandler::sendConnectReply(const HostAddress &hostAddress, quint16 port)
{
    return coreBridge->sendConnectReplyCore(toCoreAddress(hostAddress), port);
}

bool Socks5RequestHandler::sendFailedReply()
{
    return coreBridge->sendFailedReplyCore();
}

}  // namespace QTNETWORKNG_NAMESPACE
