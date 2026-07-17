#ifndef QTNG_SOCKET_UTILS_H
#define QTNG_SOCKET_UTILS_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtng/socket.h"
#include "qtng/io_utils.h"
#include "qtng/utils/platform.h"

namespace qtng {

#ifndef QTNG_NO_CRYPTO
class SslSocket;
#endif
class KcpSocket;
class SocketLike : public FileLike
{
public:
    SocketLike();
    virtual ~SocketLike() override;
public:
    virtual Socket::SocketError error() const = 0;
    virtual std::string errorString() const = 0;
    virtual bool isValid() const = 0;
    virtual HostAddress localAddress() const = 0;
    virtual std::uint16_t localPort() const = 0;
    virtual HostAddress peerAddress() const = 0;
    virtual std::string peerName() const = 0;
    virtual std::uint16_t peerPort() const = 0;
    virtual std::intptr_t fileno() const = 0;
    virtual Socket::SocketType type() const = 0;
    virtual Socket::SocketState state() const = 0;
    virtual HostAddress::NetworkLayerProtocol protocol() const = 0;
    virtual std::string localAddressURI() const = 0;
    virtual std::string peerAddressURI() const = 0;

    virtual std::shared_ptr<SocketLike> accept() = 0;
    virtual Socket *acceptRaw() = 0;
    virtual bool bind(const HostAddress &address, std::uint16_t port = 0,
                      Socket::BindMode mode = Socket::DefaultForPlatform) = 0;
    virtual bool bind(std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform) = 0;
    virtual bool connect(const HostAddress &addr, std::uint16_t port) = 0;
    virtual bool connect(const std::string &hostName, std::uint16_t port,
                         std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>()) = 0;
    //    virtual void close() override = 0;  // from FileLike
    virtual void abort() = 0;
    virtual bool listen(int backlog) = 0;
    virtual bool setOption(Socket::SocketOption option, int value) = 0;
    virtual int option(Socket::SocketOption option) const = 0;

    virtual std::int32_t peek(char *data, std::int32_t size) = 0;
    virtual std::int32_t peekRaw(char *data, std::int32_t size) = 0;
    virtual std::int32_t recv(char *data, std::int32_t size) = 0;
    virtual std::int32_t recvall(char *data, std::int32_t size) = 0;
    virtual std::int32_t send(const char *data, std::int32_t size) = 0;
    virtual std::int32_t sendall(const char *data, std::int32_t size) = 0;
    virtual std::string recv(std::int32_t size) = 0;
    virtual std::string recvall(std::int32_t size) = 0;
    virtual std::int32_t send(const std::string &data) = 0;
    virtual std::int32_t sendall(const std::string &data) = 0;
public:
    virtual std::int32_t read(char *data, std::int32_t size) override;
    virtual std::int32_t write(const char *data, std::int32_t size) override;
    virtual std::int64_t size() override;
};

class ExchangerPrivate;
class Exchanger
{
public:
    Exchanger(std::shared_ptr<SocketLike> request, std::shared_ptr<SocketLike> forward, std::uint32_t maxBufferSize = 1024 * 8);
    ~Exchanger();
public:
    void exchange();
private:
    ExchangerPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(Exchanger)
};

class SocketProxy
{
public:
    virtual std::shared_ptr<SocketLike> connect(const HostAddress &addr, std::uint16_t port) = 0;
    virtual std::shared_ptr<SocketLike> connect(const std::string &addr, std::uint16_t port) = 0;
};

std::shared_ptr<SocketLike> asSocketLike(std::shared_ptr<Socket> s);

inline std::shared_ptr<SocketLike> asSocketLike(Socket *s)
{
    return asSocketLike(std::shared_ptr<Socket>(s));
}

std::shared_ptr<Socket> convertSocketLikeToSocket(std::shared_ptr<SocketLike> socket);

}  // namespace qtng

#endif  // QTNG_SOCKET_UTILS_H
