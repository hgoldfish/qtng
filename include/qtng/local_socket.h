#ifndef QTNG_LOCAL_SOCKET_H
#define QTNG_LOCAL_SOCKET_H

#include <cstdint>
#include <string>

#include "qtng/socket.h"
#include "qtng/utils/platform.h"

namespace qtng {

class LocalSocketPrivate;

class LocalSocket
{
public:
    enum LocalSocketType {
        StreamSocket = 1,
        DatagramSocket = 2,
    };
    enum LocalSocketState {
        UnconnectedState = 1,
        ConnectingState = 2,
        ConnectedState = 3,
        BoundState = 4,
        ListeningState = 5,
        ClosingState = 6
    };
public:
    explicit LocalSocket(LocalSocketType type = StreamSocket);
    explicit LocalSocket(std::intptr_t socketDescriptor);
    virtual ~LocalSocket();
public:
    Socket::SocketError error() const;
    std::string errorString() const;
    bool isValid() const;
    LocalSocketType type() const;
    LocalSocketState state() const;
    std::intptr_t fileno() const;
    std::string localName() const;
    std::string peerName() const;
public:
    LocalSocket *accept();
    bool bind(const std::string &name);
    bool connect(const std::string &name);
    void close();
    void abort();
    bool listen(int backlog);

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::int32_t recvfrom(char *data, std::int32_t size, std::string *addr);
    std::int32_t sendto(const char *data, std::int32_t size, const std::string &addr);

    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);
    std::string recvfrom(std::int32_t size, std::string *addr);
    std::int32_t sendto(const std::string &data, const std::string &addr);
private:
    LocalSocketPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(LocalSocket)
    NG_DISABLE_COPY(LocalSocket)
};

}  // namespace qtng

#endif  // QTNG_LOCAL_SOCKET_H
