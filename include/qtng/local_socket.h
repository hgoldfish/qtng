#ifndef QTNG_LOCAL_SOCKET_H
#define QTNG_LOCAL_SOCKET_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/socket.h"
#include "qtng/utils/platform.h"

namespace qtng {

class LocalSocketPrivate;
class LocalSocket
{
public:
    LocalSocket();
    explicit LocalSocket(std::intptr_t socketDescriptor);
    virtual ~LocalSocket();
public:
    Socket::SocketError error() const;
    std::string errorString() const;
    bool isValid() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    HostAddress peerAddress() const;
    std::string peerName() const;
    std::uint16_t peerPort() const;
    std::intptr_t fileno() const;
    Socket::SocketType type() const;
    Socket::SocketState state() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    std::string localAddressURI() const;
    std::string peerAddressURI() const;

    std::string serverName() const;
    std::string fullServerName() const;

    LocalSocket *accept();
    // By default a socket file that nothing serves anymore - the leftover of a
    // crashed server - is reclaimed, so restarting a server on the same name
    // just works. DontShareAddress opts out: the leftover is left alone and
    // bind() fails with AddressInUseError. Only a socket file is ever
    // reclaimed; anything else at that path is somebody else's data and stays
    // put, so bind() fails with AddressInUseError there too. Windows has
    // nothing to reclaim, and a named pipe never shares its name in the first
    // place, so mode is ignored there.
    bool bind(const std::string &name, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool connect(const std::string &name);
    void close();
    void abort();
    // backlog reaches ::listen() on Unix. Windows named pipes have no accept
    // queue, so the value has no effect there.
    bool listen(int backlog = 50);
    bool setOption(Socket::SocketOption option, int value);
    int option(Socket::SocketOption option) const;

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t peekRaw(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);

    static LocalSocket *createConnection(const std::string &name, Socket::SocketError *error = nullptr);
    // if backlog == 0, only create and bind (do not listen).
    static LocalSocket *createServer(const std::string &name, int backlog = 50);
private:
    LocalSocket(LocalSocketPrivate *d);
private:
    LocalSocketPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(LocalSocket)
    NG_DISABLE_COPY_MOVE(LocalSocket)
};

std::shared_ptr<class SocketLike> asSocketLike(std::shared_ptr<LocalSocket> s);

inline std::shared_ptr<class SocketLike> asSocketLike(LocalSocket *s)
{
    return asSocketLike(std::shared_ptr<LocalSocket>(s));
}

std::shared_ptr<LocalSocket> convertSocketLikeToLocalSocket(std::shared_ptr<class SocketLike> socket);

}  // namespace qtng

#endif  // QTNG_LOCAL_SOCKET_H
