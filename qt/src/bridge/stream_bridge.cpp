#include "bridge/stream_bridge.h"
#include "bridge/io_bridge.h"
#include "bridge/socket_access.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace qtng_bridge {

class QtBackedCoreSocketLike : public qtng_core::SocketLike
{
public:
    explicit QtBackedCoreSocketLike(QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> qt)
        : qt(std::move(qt))
    {
    }

    qtng_core::Socket::SocketError error() const override
    {
        return static_cast<qtng_core::Socket::SocketError>(qt->error());
    }

    string errorString() const override { return toStdString(qt->errorString()); }
    bool isValid() const override { return qt->isValid(); }
    qtng_core::HostAddress localAddress() const override { return toCoreAddress(qt->localAddress()); }
    uint16_t localPort() const override { return qt->localPort(); }
    qtng_core::HostAddress peerAddress() const override { return toCoreAddress(qt->peerAddress()); }
    string peerName() const override { return toStdString(qt->peerName()); }
    uint16_t peerPort() const override { return qt->peerPort(); }
    intptr_t fileno() const override { return static_cast<intptr_t>(qt->fileno()); }
    qtng_core::Socket::SocketType type() const override
    {
        return static_cast<qtng_core::Socket::SocketType>(qt->type());
    }
    qtng_core::Socket::SocketState state() const override
    {
        return static_cast<qtng_core::Socket::SocketState>(qt->state());
    }
    qtng_core::HostAddress::NetworkLayerProtocol protocol() const override
    {
        return static_cast<qtng_core::HostAddress::NetworkLayerProtocol>(qt->protocol());
    }
    string localAddressURI() const override { return toStdString(qt->localAddressURI()); }
    string peerAddressURI() const override { return toStdString(qt->peerAddressURI()); }

    shared_ptr<qtng_core::SocketLike> accept() override
    {
        QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> accepted = qt->accept();
        if (accepted.isNull()) {
            return shared_ptr<qtng_core::SocketLike>();
        }
        return make_shared<QtBackedCoreSocketLike>(accepted);
    }

    qtng_core::Socket *acceptRaw() override { return nullptr; }

    bool bind(const qtng_core::HostAddress &address, uint16_t port, qtng_core::Socket::BindMode mode) override
    {
        return qt->bind(toQtAddress(address), port, static_cast<Socket::BindMode>(static_cast<int>(mode)));
    }

    bool bind(uint16_t port, qtng_core::Socket::BindMode mode) override
    {
        return qt->bind(port, static_cast<Socket::BindMode>(static_cast<int>(mode)));
    }

    bool connect(const qtng_core::HostAddress &addr, uint16_t port) override
    {
        return qt->connect(toQtAddress(addr), port);
    }

    bool connect(const string &hostName, uint16_t port, shared_ptr<qtng_core::SocketDnsCache> dnsCache) override
    {
        (void)dnsCache;
        return qt->connect(toQString(hostName), port);
    }

    void close() override { qt->close(); }
    void abort() override { qt->abort(); }
    bool listen(int backlog) override { return qt->listen(backlog); }

    bool setOption(qtng_core::Socket::SocketOption option, int value) override
    {
        return qt->setOption(static_cast<QTNETWORKNG_NAMESPACE::Socket::SocketOption>(option), QVariant(value));
    }

    int option(qtng_core::Socket::SocketOption option) const override
    {
        return qt->option(static_cast<QTNETWORKNG_NAMESPACE::Socket::SocketOption>(option)).toInt();
    }

    int32_t peek(char *data, int32_t size) override { return qt->peek(data, size); }
    int32_t peekRaw(char *data, int32_t size) override { return qt->peekRaw(data, size); }
    int32_t recv(char *data, int32_t size) override { return qt->recv(data, size); }
    int32_t recvall(char *data, int32_t size) override { return qt->recvall(data, size); }
    int32_t send(const char *data, int32_t size) override { return qt->send(data, size); }
    int32_t sendall(const char *data, int32_t size) override { return qt->sendall(data, size); }
    string recv(int32_t size) override { return toStdString(qt->recv(size)); }
    string recvall(int32_t size) override { return toStdString(qt->recvall(size)); }
    int32_t send(const string &data) override { return qt->send(toQByteArray(data)); }
    int32_t sendall(const string &data) override { return qt->sendall(toQByteArray(data)); }

    QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> qtSocket() const { return qt; }

    QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> qt;
};

class CoreSocketLikeAdapter : public SocketLike
{
public:
    explicit CoreSocketLikeAdapter(shared_ptr<qtng_core::SocketLike> core)
        : core(std::move(core))
    {
    }

    Socket::SocketError error() const override
    {
        return static_cast<Socket::SocketError>(core->error());
    }

    QString errorString() const override { return toQString(core->errorString()); }
    bool isValid() const override { return core->isValid(); }
    HostAddress localAddress() const override { return toQtAddress(core->localAddress()); }
    quint16 localPort() const override { return core->localPort(); }
    HostAddress peerAddress() const override { return toQtAddress(core->peerAddress()); }
    QString peerName() const override { return toQString(core->peerName()); }
    quint16 peerPort() const override { return core->peerPort(); }
    qintptr fileno() const override { return static_cast<qintptr>(core->fileno()); }
    Socket::SocketType type() const override { return static_cast<Socket::SocketType>(core->type()); }
    Socket::SocketState state() const override { return static_cast<Socket::SocketState>(core->state()); }
    HostAddress::NetworkLayerProtocol protocol() const override
    {
        return static_cast<HostAddress::NetworkLayerProtocol>(core->protocol());
    }
    QString localAddressURI() const override { return toQString(core->localAddressURI()); }
    QString peerAddressURI() const override { return toQString(core->peerAddressURI()); }

    QSharedPointer<SocketLike> accept() override
    {
        shared_ptr<qtng_core::SocketLike> accepted = core->accept();
        if (!accepted) {
            return QSharedPointer<SocketLike>();
        }
        return QSharedPointer<SocketLike>(new CoreSocketLikeAdapter(accepted));
    }

    Socket *acceptRaw() override { return nullptr; }

    bool bind(const HostAddress &address, quint16 port, Socket::BindMode mode) override
    {
        return core->bind(toCoreAddress(address), port, static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
    }

    bool bind(quint16 port, Socket::BindMode mode) override
    {
        return core->bind(port, static_cast<qtng_core::Socket::BindMode>(static_cast<int>(mode)));
    }

    bool connect(const HostAddress &addr, quint16 port) override
    {
        return core->connect(toCoreAddress(addr), port);
    }

    bool connect(const QString &hostName, quint16 port, QSharedPointer<SocketDnsCache> dnsCache) override
    {
        (void)dnsCache;
        return core->connect(toStdString(hostName), port);
    }

    void close() override { core->close(); }
    void abort() override { core->abort(); }
    bool listen(int backlog) override { return core->listen(backlog); }

    bool setOption(Socket::SocketOption option, const QVariant &value) override
    {
        return core->setOption(static_cast<qtng_core::Socket::SocketOption>(option), value.toInt());
    }

    QVariant option(Socket::SocketOption option) const override
    {
        return core->option(static_cast<qtng_core::Socket::SocketOption>(option));
    }

    qint32 peek(char *data, qint32 size) override { return core->peek(data, size); }
    qint32 peekRaw(char *data, qint32 size) override { return core->peekRaw(data, size); }
    qint32 recv(char *data, qint32 size) override { return core->recv(data, size); }
    qint32 recvall(char *data, qint32 size) override { return core->recvall(data, size); }
    qint32 send(const char *data, qint32 size) override { return core->send(data, size); }
    qint32 sendall(const char *data, qint32 size) override { return core->sendall(data, size); }
    QByteArray recv(qint32 size) override { return toQByteArray(core->recv(size)); }
    QByteArray recvall(qint32 size) override { return toQByteArray(core->recvall(size)); }
    qint32 send(const QByteArray &data) override { return core->send(toStdString(data)); }
    qint32 sendall(const QByteArray &data) override { return core->sendall(toStdString(data)); }

    shared_ptr<qtng_core::SocketLike> coreSocket() const { return core; }

    shared_ptr<qtng_core::SocketLike> core;
};

std::shared_ptr<qtng_core::SocketLike> toCoreSocketLike(const QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> &socket)
{
    if (socket.isNull()) {
        return std::shared_ptr<qtng_core::SocketLike>();
    }
    if (QSharedPointer<CoreSocketLikeAdapter> adapter = socket.dynamicCast<CoreSocketLikeAdapter>()) {
        return adapter->coreSocket();
    }
    if (std::shared_ptr<qtng_core::SocketLike> special = kcpOrUtpToCoreSocketLike(socket)) {
        return special;
    }
    if (QSharedPointer<QTNETWORKNG_NAMESPACE::Socket> asSocket =
                qSharedPointerDynamicCast<QTNETWORKNG_NAMESPACE::Socket>(socket)) {
        std::shared_ptr<qtng_core::Socket> coreSocket = ::qtng_bridge::socketCoreOf(asSocket.data());
        if (coreSocket) {
            return qtng_core::asSocketLike(coreSocket);
        }
    }
    return std::make_shared<QtBackedCoreSocketLike>(socket);
}

QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike> toQtSocketLike(const shared_ptr<qtng_core::SocketLike> &core)
{
    if (!core) {
        return QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike>();
    }
    if (shared_ptr<QtBackedCoreSocketLike> backed = dynamic_pointer_cast<QtBackedCoreSocketLike>(core)) {
        return backed->qtSocket();
    }
    return QSharedPointer<QTNETWORKNG_NAMESPACE::SocketLike>(new CoreSocketLikeAdapter(core));
}

class QtBackedCoreFileLike : public qtng_core::FileLike
{
public:
    explicit QtBackedCoreFileLike(QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> qt)
        : qt(std::move(qt))
    {
    }

    int32_t read(char *data, int32_t size) override { return qt->read(data, size); }
    int32_t write(const char *data, int32_t size) override { return qt->write(data, size); }
    void close() override { qt->close(); }
    int64_t size() override { return qt->size(); }

    QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> qt;
};

class CoreFileLikeAdapter : public FileLike
{
public:
    explicit CoreFileLikeAdapter(shared_ptr<qtng_core::FileLike> core)
        : core(std::move(core))
    {
    }

    qint32 read(char *data, qint32 size) override { return core->read(data, size); }
    qint32 write(const char *data, qint32 size) override { return core->write(data, size); }
    void close() override { core->close(); }
    qint64 size() override { return core->size(); }
    QByteArray readall(bool *ok) override
    {
        bool coreOk = false;
        const string data = core->readall(&coreOk);
        if (ok) {
            *ok = coreOk;
        }
        return toQByteArray(data);
    }

    shared_ptr<qtng_core::FileLike> coreFile() const { return core; }

    shared_ptr<qtng_core::FileLike> core;
};

shared_ptr<qtng_core::FileLike> toCoreFileLike(const QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> &file)
{
    if (file.isNull()) {
        return shared_ptr<qtng_core::FileLike>();
    }
    if (QSharedPointer<CoreFileLikeAdapter> adapter = file.dynamicCast<CoreFileLikeAdapter>()) {
        return adapter->coreFile();
    }
    return make_shared<QtBackedCoreFileLike>(file);
}

QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike> toQtFileLike(const shared_ptr<qtng_core::FileLike> &core)
{
    if (!core) {
        return QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike>();
    }
    if (shared_ptr<QtBackedCoreFileLike> backed = dynamic_pointer_cast<QtBackedCoreFileLike>(core)) {
        return backed->qt;
    }
    return QSharedPointer<QTNETWORKNG_NAMESPACE::FileLike>(new CoreFileLikeAdapter(core));
}

}  // namespace qtng_bridge
