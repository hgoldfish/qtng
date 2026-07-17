#ifndef JOKER_CLIENT_H
#define JOKER_CLIENT_H

#include <memory>
#include <string>
#include <vector>

#include "qtng/qtng.h"

class JokerClientConfigure
{
public:
    JokerClientConfigure();
public:
    std::shared_ptr<qtng::Cipher> templateCipher;
    float timeout;
    int maxWeight;

    qtng::HostAddress localSocks5Address;
    std::uint16_t localSocks5Port;

    qtng::HostAddress localHttpAddress;
    std::uint16_t localHttpPort;
};


class JokerServerConnection
{
public:
    enum Type
    {
        Kcp,
        Http,
    };
public:
    JokerServerConnection();
    bool connect(std::shared_ptr<qtng::Cipher> templateCipher);
    void increaseWeight(int maxWeight);
    void decreaseWeight();
    bool isAlive() const { return channel && !channel->isBroken(); }
    void close() { channel.reset(); }
public:
    Type type;
    std::string name;
    std::string remoteAddress;
    std::uint16_t remotePort;
    std::uint16_t mtu;
    qtng::KcpSocket::Mode mode;
public:
    std::shared_ptr<qtng::SocketChannel> channel;
    int weight;
private:
    bool connectKcp(std::shared_ptr<qtng::Cipher> templateCipher);
    bool connectHttp(std::shared_ptr<qtng::Cipher> templateCipher);
};


struct MakeChannelResult
{
    std::shared_ptr<qtng::VirtualChannel> forward;
    std::shared_ptr<JokerServerConnection> selectedServer;
};

class JokerClient
{
public:
    JokerClient(const JokerClientConfigure &configure, const std::vector<std::shared_ptr<JokerServerConnection>> &servers);
    ~JokerClient();
public:
    bool start();
private:
    std::shared_ptr<qtng::SocketLike> connectToRemoteHost(const std::string &hostName, const qtng::HostAddress &addr,
                                                          std::uint16_t port, qtng::HostAddress *forwardAddress);
    void exchangeAsync(std::shared_ptr<qtng::SocketLike> request, std::shared_ptr<qtng::SocketLike> forward);
    void exchangeSync(std::shared_ptr<qtng::SocketLike> request, std::shared_ptr<qtng::SocketLike> forward);
    void logRequest(const std::string &type, const std::string &hostName, std::uint16_t port,
                    const qtng::HostAddress &realIP, bool success);
    MakeChannelResult makeChannel();
    MakeChannelResult _makeChannel();
private:
    qtng::CoroutineGroup *operations;
    qtng::Lock lock;
    JokerClientConfigure configure;
    std::vector<std::shared_ptr<JokerServerConnection>> servers;
    std::shared_ptr<JokerServerConnection> lastSelectedServer;
    std::shared_ptr<class MakeChannelCoroutine> makeChannelCoroutine;
    std::shared_ptr<qtng::HttpSession> session;
    friend class JokerSocks5ProxyRequestHandler;
    friend class JokerHttpProxyRequestHandler;
    friend class MakeChannelCoroutine;
};

#endif
