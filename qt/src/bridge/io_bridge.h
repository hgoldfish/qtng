#ifndef QTNG_QT_BRIDGE_IO_BRIDGE_H
#define QTNG_QT_BRIDGE_IO_BRIDGE_H

#include <memory>

#include "bridge/core_access.h"
#include "io_utils.h"
#include "socket.h"
#include "socket_utils.h"
#include "bridge/stream_bridge.h"

namespace qtng_bridge {

class QtFileLikeAdapter : public qtng_core::FileLike
{
public:
    explicit QtFileLikeAdapter(QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> qt)
        : qt(std::move(qt))
    {
    }
    std::int32_t read(char *data, std::int32_t size) override { return qt ? qt->read(data, size) : -1; }
    std::int32_t write(const char *data, std::int32_t size) override { return qt ? qt->write(data, size) : -1; }
    void close() override
    {
        if (qt) {
            qt->close();
        }
    }
    std::int64_t size() override { return qt ? qt->size() : -1; }

    QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> qt;
};

class CoreFileLikeWrapper : public QTNETWORKNG_NAMESPACE::FileLike
{
public:
    explicit CoreFileLikeWrapper(std::shared_ptr<qtng_core::FileLike> core)
        : core(std::move(core))
    {
    }
    qint32 read(char *data, qint32 size) override
    {
        return core ? core->read(data, size) : -1;
    }
    qint32 write(const char *data, qint32 size) override
    {
        return core ? core->write(data, size) : -1;
    }
    void close() override
    {
        if (core) {
            core->close();
        }
    }
    qint64 size() override { return core ? core->size() : -1; }

    std::shared_ptr<qtng_core::FileLike> core;
};

class QtSocketLikeAdapter : public qtng_core::SocketLike
{
public:
    explicit QtSocketLikeAdapter(QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> qt);
    qtng_core::Socket::SocketError error() const override;
    std::string errorString() const override;
    bool isValid() const override;
    qtng_core::HostAddress localAddress() const override;
    std::uint16_t localPort() const override;
    qtng_core::HostAddress peerAddress() const override;
    std::string peerName() const override;
    std::uint16_t peerPort() const override;
    std::intptr_t fileno() const override;
    qtng_core::Socket::SocketType type() const override;
    qtng_core::Socket::SocketState state() const override;
    qtng_core::HostAddress::NetworkLayerProtocol protocol() const override;
    std::string localAddressURI() const override;
    std::string peerAddressURI() const override;
    qtng_core::Socket *acceptRaw() override;
    std::shared_ptr<qtng_core::SocketLike> accept() override;
    bool bind(const qtng_core::HostAddress &address, std::uint16_t port, qtng_core::Socket::BindMode mode) override;
    bool bind(std::uint16_t port, qtng_core::Socket::BindMode mode) override;
    bool connect(const qtng_core::HostAddress &addr, std::uint16_t port) override;
    bool connect(const std::string &hostName, std::uint16_t port,
                 std::shared_ptr<qtng_core::SocketDnsCache> dnsCache) override;
    void close() override;
    void abort() override;
    bool listen(int backlog) override;
    bool setOption(qtng_core::Socket::SocketOption option, int value) override;
    int option(qtng_core::Socket::SocketOption option) const override;
    std::int32_t peek(char *data, std::int32_t size) override;
    std::int32_t peekRaw(char *data, std::int32_t size) override;
    std::int32_t recv(char *data, std::int32_t size) override;
    std::int32_t recvall(char *data, std::int32_t size) override;
    std::int32_t send(const char *data, std::int32_t size) override;
    std::int32_t sendall(const char *data, std::int32_t size) override;
    std::string recv(std::int32_t size) override;
    std::string recvall(std::int32_t size) override;
    std::int32_t send(const std::string &data) override;
    std::int32_t sendall(const std::string &data) override;

    QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> qt;
};

class CoreSocketLikeWrapper : public QTNETWORKNG_NAMESPACE::SocketLike
{
public:
    explicit CoreSocketLikeWrapper(std::shared_ptr<qtng_core::SocketLike> core);
    QTNETWORKNG_NAMESPACE::Socket::SocketError error() const override;
    QString errorString() const override;
    bool isValid() const override;
    QTNETWORKNG_NAMESPACE::HostAddress localAddress() const override;
    quint16 localPort() const override;
    QTNETWORKNG_NAMESPACE::HostAddress peerAddress() const override;
    QString peerName() const override;
    quint16 peerPort() const override;
    qintptr fileno() const override;
    QTNETWORKNG_NAMESPACE::Socket::SocketType type() const override;
    QTNETWORKNG_NAMESPACE::Socket::SocketState state() const override;
    QTNETWORKNG_NAMESPACE::HostAddress::NetworkLayerProtocol protocol() const override;
    QString localAddressURI() const override;
    QString peerAddressURI() const override;
    QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> accept() override;
    QTNETWORKNG_NAMESPACE::Socket *acceptRaw() override;
    bool bind(const QTNETWORKNG_NAMESPACE::HostAddress &address, quint16 port,
              QTNETWORKNG_NAMESPACE::Socket::BindMode mode) override;
    bool bind(quint16 port, QTNETWORKNG_NAMESPACE::Socket::BindMode mode) override;
    bool connect(const QTNETWORKNG_NAMESPACE::HostAddress &addr, quint16 port) override;
    bool connect(const QString &hostName, quint16 port,
                 QSharedPointer<QTNETWORKNG_NAMESPACE::SocketDnsCache> dnsCache) override;
    void close() override;
    void abort() override;
    bool listen(int backlog) override;
    bool setOption(QTNETWORKNG_NAMESPACE::Socket::SocketOption option, const QVariant &value) override;
    QVariant option(QTNETWORKNG_NAMESPACE::Socket::SocketOption option) const override;
    qint32 peek(char *data, qint32 size) override;
    qint32 peekRaw(char *data, qint32 size) override;
    qint32 recv(char *data, qint32 size) override;
    qint32 recvall(char *data, qint32 size) override;
    qint32 send(const char *data, qint32 size) override;
    qint32 sendall(const char *data, qint32 size) override;
    QByteArray recv(qint32 size) override;
    QByteArray recvall(qint32 size) override;
    qint32 send(const QByteArray &data) override;
    qint32 sendall(const QByteArray &data) override;

    std::shared_ptr<qtng_core::SocketLike> core;
};

QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> asSocketLike(QSharedPointer<QTNETWORKNG_NAMESPACE::Socket> s);
QSharedPointer<QTNETWORKNG_NAMESPACE::Socket> convertSocketLikeToSocket(QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> socket);

qtng_core::HostAddress toCoreHostAddress(const QTNETWORKNG_NAMESPACE::HostAddress &addr);
QTNETWORKNG_NAMESPACE::HostAddress toQtHostAddress(const qtng_core::HostAddress &addr);

}  // namespace qtng_bridge

#endif  // QTNG_QT_BRIDGE_IO_BRIDGE_H
