#ifndef JOKER_SERVER_H
#define JOKER_SERVER_H

#include <memory>

#include "qtng/io_utils.h"
#include "qtng/qtng.h"

class JokerServerConfigure
{
public:
    JokerServerConfigure();
public:
    std::shared_ptr<qtng::Cipher> templateCipher;
    float timeout;

    // Applied to the outbound TCP forward socket (SO_SNDBUF / SO_RCVBUF).
    int sendBufferSize;
    int receiveBufferSize;

    qtng::HostAddress kcpAddress;
    std::uint16_t kcpPort;
    qtng::KcpSocket::Mode kcpMode;

    qtng::HostAddress httpAddress;
    std::uint16_t httpPort;
    qtng::PosixPath httpRootDir;
};


class JokerServer
{
public:
    explicit JokerServer(const JokerServerConfigure &configure);
    ~JokerServer();
public:
    bool start();
private:
    struct Private;
    std::unique_ptr<Private> d;
};

#endif
