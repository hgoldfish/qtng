#include <cstring>

#include "bridge/io_bridge.h"
#include "bridge/stream_bridge.h"
#include "bridge/socket_access.h"

using namespace std;

namespace qtng_bridge {

using QTNETWORKNG_NAMESPACE::FileLike;
using QTNETWORKNG_NAMESPACE::HostAddress;
using QTNETWORKNG_NAMESPACE::IPv6Address;
using QTNETWORKNG_NAMESPACE::Socket;
using QTNETWORKNG_NAMESPACE::SocketLike;
using QTNETWORKNG_NAMESPACE::SocketDnsCache;

qtng_core::HostAddress toCoreHostAddress(const HostAddress &addr)
{
    if (addr.isNull()) {
        return qtng_core::HostAddress();
    }
    if (addr.protocol() == HostAddress::IPv4Protocol) {
        return qtng_core::HostAddress(addr.toIPv4Address());
    }
    const IPv6Address v6 = addr.toIPv6Address();
    qtng_core::HostAddress result(v6.c);
    result.setScopeId(toStdString(addr.scopeId()));
    return result;
}

HostAddress toQtHostAddress(const qtng_core::HostAddress &addr)
{
    if (addr.isNull()) {
        return HostAddress();
    }
    if (addr.protocol() == qtng_core::HostAddress::IPv4Protocol) {
        return HostAddress(addr.toIPv4Address());
    }
    const qtng_core::IPv6Address v6 = addr.toIPv6Address();
    IPv6Address qv6;
    memcpy(qv6.c, v6.c, 16);
    HostAddress result(qv6);
    result.setScopeId(toQString(addr.scopeId()));
    return result;
}

QtSocketLikeAdapter::QtSocketLikeAdapter(QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> qt)
    : qt(std::move(qt))
{
}

qtng_core::Socket::SocketError QtSocketLikeAdapter::error() const
{
    return static_cast<qtng_core::Socket::SocketError>(qt->error());
}

string QtSocketLikeAdapter::errorString() const { return toStdString(qt->errorString()); }
bool QtSocketLikeAdapter::isValid() const { return qt->isValid(); }
qtng_core::HostAddress QtSocketLikeAdapter::localAddress() const { return toCoreHostAddress(qt->localAddress()); }
uint16_t QtSocketLikeAdapter::localPort() const { return qt->localPort(); }
qtng_core::HostAddress QtSocketLikeAdapter::peerAddress() const { return toCoreHostAddress(qt->peerAddress()); }
string QtSocketLikeAdapter::peerName() const { return toStdString(qt->peerName()); }
uint16_t QtSocketLikeAdapter::peerPort() const { return qt->peerPort(); }
intptr_t QtSocketLikeAdapter::fileno() const { return static_cast<intptr_t>(qt->fileno()); }
qtng_core::Socket::SocketType QtSocketLikeAdapter::type() const
{
    return static_cast<qtng_core::Socket::SocketType>(qt->type());
}
qtng_core::Socket::SocketState QtSocketLikeAdapter::state() const
{
    return static_cast<qtng_core::Socket::SocketState>(qt->state());
}
qtng_core::HostAddress::NetworkLayerProtocol QtSocketLikeAdapter::protocol() const
{
    return static_cast<qtng_core::HostAddress::NetworkLayerProtocol>(qt->protocol());
}
string QtSocketLikeAdapter::localAddressURI() const { return toStdString(qt->localAddressURI()); }
string QtSocketLikeAdapter::peerAddressURI() const { return toStdString(qt->peerAddressURI()); }

qtng_core::Socket *QtSocketLikeAdapter::acceptRaw()
{
    return nullptr;
}

shared_ptr<qtng_core::SocketLike> QtSocketLikeAdapter::accept()
{
    return toCoreSocketLike(qt->accept());
}

bool QtSocketLikeAdapter::bind(const qtng_core::HostAddress &address, uint16_t port, qtng_core::Socket::BindMode mode)
{
    return qt->bind(toQtHostAddress(address), port, static_cast<Socket::BindMode>(static_cast<int>(mode)));
}

bool QtSocketLikeAdapter::bind(uint16_t port, qtng_core::Socket::BindMode mode)
{
    return qt->bind(port, static_cast<Socket::BindMode>(static_cast<int>(mode)));
}

bool QtSocketLikeAdapter::connect(const qtng_core::HostAddress &addr, uint16_t port)
{
    return qt->connect(toQtHostAddress(addr), port);
}

bool QtSocketLikeAdapter::connect(const string &hostName, uint16_t port, shared_ptr<qtng_core::SocketDnsCache> dnsCache)
{
    Q_UNUSED(dnsCache);
    return qt->connect(toQString(hostName), port);
}

void QtSocketLikeAdapter::close() { qt->close(); }
void QtSocketLikeAdapter::abort() { qt->abort(); }
bool QtSocketLikeAdapter::listen(int backlog) { return qt->listen(backlog); }
bool QtSocketLikeAdapter::setOption(qtng_core::Socket::SocketOption option, int value)
{
    return qt->setOption(static_cast<Socket::SocketOption>(option), value);
}
int QtSocketLikeAdapter::option(qtng_core::Socket::SocketOption option) const
{
    return qt->option(static_cast<Socket::SocketOption>(option)).toInt();
}
int32_t QtSocketLikeAdapter::peek(char *data, int32_t size) { return qt->peek(data, size); }
int32_t QtSocketLikeAdapter::peekRaw(char *data, int32_t size) { return qt->peekRaw(data, size); }
int32_t QtSocketLikeAdapter::recv(char *data, int32_t size) { return qt->recv(data, size); }
int32_t QtSocketLikeAdapter::recvall(char *data, int32_t size) { return qt->recvall(data, size); }
int32_t QtSocketLikeAdapter::send(const char *data, int32_t size) { return qt->send(data, size); }
int32_t QtSocketLikeAdapter::sendall(const char *data, int32_t size) { return qt->sendall(data, size); }
string QtSocketLikeAdapter::recv(int32_t size) { return toStdString(qt->recv(size)); }
string QtSocketLikeAdapter::recvall(int32_t size) { return toStdString(qt->recvall(size)); }
int32_t QtSocketLikeAdapter::send(const string &data) { return qt->send(toQByteArray(data)); }
int32_t QtSocketLikeAdapter::sendall(const string &data) { return qt->sendall(toQByteArray(data)); }

CoreSocketLikeWrapper::CoreSocketLikeWrapper(shared_ptr<qtng_core::SocketLike> core)
    : core(std::move(core))
{
}

Socket::SocketError CoreSocketLikeWrapper::error() const
{
    return static_cast<Socket::SocketError>(core->error());
}
QString CoreSocketLikeWrapper::errorString() const { return toQString(core->errorString()); }
bool CoreSocketLikeWrapper::isValid() const { return core->isValid(); }
HostAddress CoreSocketLikeWrapper::localAddress() const { return toQtHostAddress(core->localAddress()); }
quint16 CoreSocketLikeWrapper::localPort() const { return core->localPort(); }
HostAddress CoreSocketLikeWrapper::peerAddress() const { return toQtHostAddress(core->peerAddress()); }
QString CoreSocketLikeWrapper::peerName() const { return toQString(core->peerName()); }
quint16 CoreSocketLikeWrapper::peerPort() const { return core->peerPort(); }
qintptr CoreSocketLikeWrapper::fileno() const { return static_cast<qintptr>(core->fileno()); }
Socket::SocketType CoreSocketLikeWrapper::type() const { return static_cast<Socket::SocketType>(core->type()); }
Socket::SocketState CoreSocketLikeWrapper::state() const { return static_cast<Socket::SocketState>(core->state()); }
HostAddress::NetworkLayerProtocol CoreSocketLikeWrapper::protocol() const
{
    return static_cast<HostAddress::NetworkLayerProtocol>(core->protocol());
}
QString CoreSocketLikeWrapper::localAddressURI() const { return toQString(core->localAddressURI()); }
QString CoreSocketLikeWrapper::peerAddressURI() const { return toQString(core->peerAddressURI()); }

QSharedPointer<SocketLike> CoreSocketLikeWrapper::accept() { return toQtSocketLike(core->accept()); }

Socket *CoreSocketLikeWrapper::acceptRaw()
{
    qtng_core::Socket *raw = core->acceptRaw();
    if (!raw) {
        return nullptr;
    }
    Socket *wrapper = new Socket(HostAddress::IPv4Protocol, Socket::TcpSocket);
    assignSocketCore(wrapper, std::shared_ptr<qtng_core::Socket>(raw));
    return wrapper;
}

bool CoreSocketLikeWrapper::bind(const HostAddress &address, quint16 port, Socket::BindMode mode)
{
    return core->bind(toCoreHostAddress(address), port, static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
}
bool CoreSocketLikeWrapper::bind(quint16 port, Socket::BindMode mode)
{
    return core->bind(port, static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
}
bool CoreSocketLikeWrapper::connect(const HostAddress &addr, quint16 port)
{
    return core->connect(toCoreHostAddress(addr), port);
}
bool CoreSocketLikeWrapper::connect(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache)
{
    Q_UNUSED(dnsCache);
    return core->connect(toStdString(hostName), port);
}
void CoreSocketLikeWrapper::close() { core->close(); }
void CoreSocketLikeWrapper::abort() { core->abort(); }
bool CoreSocketLikeWrapper::listen(int backlog) { return core->listen(backlog); }
bool CoreSocketLikeWrapper::setOption(Socket::SocketOption option, const QVariant &value)
{
    return core->setOption(static_cast<qtng_core::Socket::SocketOption>(option), value.toInt());
}
QVariant CoreSocketLikeWrapper::option(Socket::SocketOption option) const
{
    return core->option(static_cast<qtng_core::Socket::SocketOption>(option));
}
qint32 CoreSocketLikeWrapper::peek(char *data, qint32 size) { return core->peek(data, size); }
qint32 CoreSocketLikeWrapper::peekRaw(char *data, qint32 size) { return core->peekRaw(data, size); }
qint32 CoreSocketLikeWrapper::recv(char *data, qint32 size) { return core->recv(data, size); }
qint32 CoreSocketLikeWrapper::recvall(char *data, qint32 size) { return core->recvall(data, size); }
qint32 CoreSocketLikeWrapper::send(const char *data, qint32 size) { return core->send(data, size); }
qint32 CoreSocketLikeWrapper::sendall(const char *data, qint32 size) { return core->sendall(data, size); }
QByteArray CoreSocketLikeWrapper::recv(qint32 size) { return toQByteArray(core->recv(size)); }
QByteArray CoreSocketLikeWrapper::recvall(qint32 size) { return toQByteArray(core->recvall(size)); }
qint32 CoreSocketLikeWrapper::send(const QByteArray &data) { return core->send(toStdString(data)); }
qint32 CoreSocketLikeWrapper::sendall(const QByteArray &data) { return core->sendall(toStdString(data)); }

QSharedPointer<SocketLike> asSocketLike(QSharedPointer<Socket> s)
{
    if (!s) {
        return QSharedPointer<SocketLike>();
    }
    return toQtSocketLike(qtng_core::asSocketLike(socketCoreOf(s.data())));
}

QSharedPointer<Socket> convertSocketLikeToSocket(QSharedPointer<SocketLike> socket)
{
    if (!socket) {
        return QSharedPointer<Socket>();
    }
    shared_ptr<qtng_core::SocketLike> coreLike = toCoreSocketLike(socket);
    shared_ptr<qtng_core::Socket> raw = qtng_core::convertSocketLikeToSocket(coreLike);
    if (!raw) {
        return QSharedPointer<Socket>();
    }
    Socket *wrapper = new Socket(HostAddress::IPv4Protocol, Socket::TcpSocket);
    assignSocketCore(wrapper, raw);
    return QSharedPointer<Socket>(wrapper);
}

}  // namespace qtng_bridge
