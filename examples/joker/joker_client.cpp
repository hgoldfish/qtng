#include <algorithm>
#include <cstdio>

#include "joker_client.h"

using namespace std;

using namespace qtng;
using namespace qtng::utils;

class JokerSocks5ProxyRequestHandler : public Socks5RequestHandler
{
public:
    shared_ptr<SocketLike> makeConnection(const string &hostName, const HostAddress &addr, uint16_t port,
                                          HostAddress *forwardAddress) override;
    void exchange(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward) override;
    void logProxy(const string &hostName, const HostAddress &hostAddress, uint16_t port,
                  const HostAddress &forwardAddress, bool success) override;
};

class JokerHttpProxyRequestHandler : public BaseHttpProxyRequestHandler
{
protected:
    string serverName() override;
    void logProxy(const string &remoteHostName, uint16_t remotePort, const HostAddress &forwardAddress,
                  bool success) override;
    shared_ptr<SocketLike> makeConnection(const string &remoteHostName, uint16_t remotePort,
                                          HostAddress *forwardAddress) override;
    shared_ptr<HttpResponse> sendRequest(HttpRequest &request) override;
    void exchangeAsync(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward) override;
};

class MakeChannelCoroutine : public Coroutine
{
public:
    explicit MakeChannelCoroutine(JokerClient *parent)
        : parent(parent)
    {
    }
    void run() override;
public:
    MakeChannelResult result;
    JokerClient *parent;
};

JokerClientConfigure::JokerClientConfigure()
    : timeout(5.0f)
    , maxWeight(5)
    , localSocks5Address(HostAddress::Any)
    , localSocks5Port(8085)
    , localHttpAddress(HostAddress::Any)
    , localHttpPort(8118)
{
    templateCipher.reset(new Cipher(Cipher::Chacha20, Cipher::CBC, Cipher::Encrypt));
    const string salt("3.14159265358979323846");
    templateCipher->setPassword("123456", salt, MessageDigest::Sha256, 100000);
}

JokerServerConnection::JokerServerConnection()
    : type(Kcp)
    , remoteAddress("127.0.0.1")
    , remotePort(8000)
    , mtu(1400)
    , mode(KcpSocket::Internet)
    , weight(3)
{
}

bool JokerServerConnection::connect(shared_ptr<Cipher> templateCipher)
{
    if (remoteAddress.empty() || remotePort == 0) {
        return false;
    }
    if (channel && !channel->isBroken()) {
        return true;
    }
    bool ok;
    if (type == Kcp) {
        ok = connectKcp(templateCipher);
    } else {
        ok = connectHttp(templateCipher);
    }
    if (!ok) {
        return false;
    }
    channel->setKeepaliveTimeout(30);

    string profile;
    MsgPackStream mps(&profile, true);
    const uint16_t version = 1;
    mps << version << mtu;
    if (!channel->sendPacket(profile)) {
        channel.reset();
        return false;
    }
    const string &reply = channel->recvPacket();
    if (reply.empty()) {
        channel.reset();
        return false;
    }
    return true;
}

void JokerServerConnection::increaseWeight(int maxWeight)
{
    if (weight < maxWeight) {
        ++weight;
    }
}

void JokerServerConnection::decreaseWeight()
{
    if (weight > 1) {
        --weight;
    }
}

bool JokerServerConnection::connectKcp(shared_ptr<Cipher> templateCipher)
{
    shared_ptr<KcpSocket> kcp(KcpSocket::createConnection(remoteAddress, remotePort));
    if (!kcp) {
        return false;
    }
    kcp->setMode(mode);
    kcp->setUdpPacketSize(mtu);
    shared_ptr<SocketLike> encryptedConnection = encrypted(templateCipher, asSocketLike(kcp));
    channel.reset(new SocketChannel(encryptedConnection, DataChannelPole::PositivePole));
    channel->setPayloadSizeHint(kcp->payloadSizeHint());
    return true;
}

bool JokerServerConnection::connectHttp(shared_ptr<Cipher> templateCipher)
{
    HttpSession session;
    HttpRequest request;
    request.setUrl("http://" + remoteAddress + ":" + number(remotePort) + "/joker/");
    request.setMethod("POST");
    request.setStreamResponse(true);
    HttpResponse response = session.send(request);
    if (!response.isOk()) {
        return false;
    }
    string buf;
    shared_ptr<SocketLike> tcp = response.takeStream(&buf);
    const int64_t leftBytes = response.getContentLength() - static_cast<int64_t>(buf.size());
    if (leftBytes > 0) {
        const string &t = tcp->recvall(static_cast<int32_t>(leftBytes));
        if (static_cast<int64_t>(t.size()) != leftBytes) {
            return false;
        }
    }
    shared_ptr<SocketLike> encryptedConnection = encrypted(templateCipher, tcp);
    channel.reset(new SocketChannel(encryptedConnection, DataChannelPole::PositivePole));
    channel->setPayloadSizeHint(mtu);
    return true;
}

shared_ptr<SocketLike> JokerSocks5ProxyRequestHandler::makeConnection(const string &hostName, const HostAddress &addr,
                                                                      uint16_t port, HostAddress *forwardAddress)
{
    return userData<JokerClient>()->connectToRemoteHost(hostName, addr, port, forwardAddress);
}

void JokerSocks5ProxyRequestHandler::exchange(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward)
{
    userData<JokerClient>()->exchangeSync(request, forward);
}

void JokerSocks5ProxyRequestHandler::logProxy(const string &hostName, const HostAddress &hostAddress, uint16_t port,
                                              const HostAddress &forwardAddress, bool success)
{
    string host;
    if (hostName.empty()) {
        host = hostAddress.toString();
    } else {
        host = hostName;
    }
    userData<JokerClient>()->logRequest("SOCK", host, port, forwardAddress, success);
}

string JokerHttpProxyRequestHandler::serverName()
{
    return "JokerHttpProxy";
}

void JokerHttpProxyRequestHandler::logProxy(const string &remoteHostName, uint16_t remotePort,
                                            const HostAddress &forwardAddress, bool success)
{
    userData<JokerClient>()->logRequest("HTTP", remoteHostName, remotePort, forwardAddress, success);
}

shared_ptr<SocketLike> JokerHttpProxyRequestHandler::makeConnection(const string &remoteHostName, uint16_t remotePort,
                                                                    HostAddress *forwardAddress)
{
    return userData<JokerClient>()->connectToRemoteHost(remoteHostName, HostAddress(), remotePort, forwardAddress);
}

shared_ptr<HttpResponse> JokerHttpProxyRequestHandler::sendRequest(HttpRequest &request)
{
    shared_ptr<HttpResponse> response(new HttpResponse());
    *response = userData<JokerClient>()->session->send(request);
    return response;
}

void JokerHttpProxyRequestHandler::exchangeAsync(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward)
{
    userData<JokerClient>()->exchangeAsync(request, forward);
}

JokerClient::JokerClient(const JokerClientConfigure &configure, const vector<shared_ptr<JokerServerConnection>> &servers)
    : operations(new CoroutineGroup())
    , configure(configure)
    , servers(servers)
    , session(make_shared<HttpSession>())
{
    session->setManagingCookies(false);
}

JokerClient::~JokerClient()
{
    delete operations;
}

bool JokerClient::start()
{
    typedef TcpServer<JokerSocks5ProxyRequestHandler> JokerSocks5Proxy;
    typedef TcpServer<JokerHttpProxyRequestHandler> JokerHttpProxy;

    shared_ptr<JokerSocks5Proxy> socks5Proxy;
    shared_ptr<JokerHttpProxy> httpProxy;
    if (!configure.localSocks5Address.isNull() && configure.localSocks5Port != 0) {
        socks5Proxy.reset(new JokerSocks5Proxy(configure.localSocks5Address, configure.localSocks5Port));
        socks5Proxy->setUserData(this);
        if (!socks5Proxy->start()) {
            return false;
        }
        if (!socks5Proxy->started()->tryWait()) {
            return false;
        }
    }

    if (!configure.localHttpAddress.isNull() && configure.localHttpPort != 0) {
        httpProxy.reset(new JokerHttpProxy(configure.localHttpAddress, configure.localHttpPort));
        httpProxy->setUserData(this);
        if (!httpProxy->start()) {
            return false;
        }
        if (!httpProxy->started()->tryWait()) {
            return false;
        }
    }

    if (socks5Proxy) {
        if (!socks5Proxy->stopped()->tryWait()) {
            return false;
        }
    }

    if (httpProxy) {
        if (!httpProxy->stopped()->tryWait()) {
            return false;
        }
    }

    return true;
}

static bool compareConnection(const shared_ptr<JokerServerConnection> &x, const shared_ptr<JokerServerConnection> &y)
{
    return x->weight > y->weight;
}

MakeChannelResult JokerClient::_makeChannel()
{
    vector<shared_ptr<JokerServerConnection>> aliveServers = servers;
    sort(aliveServers.begin(), aliveServers.end(), compareConnection);

    for (shared_ptr<JokerServerConnection> server : aliveServers) {
        if (!server->isAlive()) {
            bool success = false;
            try {
                Timeout timeout(configure.timeout);
                success = server->connect(configure.templateCipher);
            } catch (TimeoutException &) {
            }
            if (!success) {
                continue;
            }
        }

        shared_ptr<VirtualChannel> forward;
        try {
            Timeout timeout(configure.timeout);
            forward = server->channel->makeChannel();
        } catch (TimeoutException &) {
        }
        if (forward && !forward->isBroken()) {
            MakeChannelResult result;
            result.forward = forward;
            result.selectedServer = server;
            if (lastSelectedServer != server) {
                lastSelectedServer = server;
                printf("Selected server connection `%s`.\n", server->name.c_str());
            }
            return result;
        }
    }

    return MakeChannelResult();
}

void MakeChannelCoroutine::run()
{
    result = parent->_makeChannel();
}

MakeChannelResult JokerClient::makeChannel()
{
    ScopedLock<Lock> l(lock);
    if (!l.isSuccess()) {
        return MakeChannelResult();
    }
    if (!makeChannelCoroutine) {
        makeChannelCoroutine = make_shared<MakeChannelCoroutine>(this);
        makeChannelCoroutine->start();
    }
    makeChannelCoroutine->join();
    MakeChannelResult result = makeChannelCoroutine->result;
    makeChannelCoroutine.reset();
    return result;
}

shared_ptr<SocketLike> JokerClient::connectToRemoteHost(const string &hostName, const HostAddress &addr, uint16_t port,
                                                        HostAddress *forwardAddress)
{
    string command;
    MsgPackStream mps(&command, true);

    if (!hostName.empty()) {
        mps << static_cast<uint8_t>(0x01) << hostName << port;
    } else if (!addr.isNull()) {
        bool ok = false;
        const uint32_t ipv4 = addr.toIPv4Address(&ok);
        if (!ok) {
            printf("only support ipv4.\n");
            *forwardAddress = HostAddress();
            return shared_ptr<SocketLike>();
        }
        mps << static_cast<uint8_t>(0x02) << ipv4 << port;
    } else {
        *forwardAddress = HostAddress();
        return shared_ptr<SocketLike>();
    }

    MakeChannelResult result;
    try {
        Timeout handshakeTimeout(configure.timeout);
        (void) handshakeTimeout;

        result = makeChannel();
        if (!result.forward) {
            *forwardAddress = HostAddress();
            return shared_ptr<SocketLike>();
        }
        if (!result.forward->sendPacket(command)) {
            result.selectedServer->decreaseWeight();
            *forwardAddress = HostAddress();
            return shared_ptr<SocketLike>();
        }
    } catch (TimeoutException &) {
        *forwardAddress = HostAddress();
        if (result.selectedServer) {
            result.selectedServer->decreaseWeight();
        }
        return shared_ptr<SocketLike>();
    }

    try {
        Timeout replyTimeout(configure.timeout);
        (void) replyTimeout;
        const string &reply = result.forward->recvPacket();
        MsgPackStream replyStream(reply);
        string s;
        replyStream >> s;
        *forwardAddress = HostAddress(s);
        result.selectedServer->increaseWeight(configure.maxWeight);
    } catch (TimeoutException &) {
        *forwardAddress = HostAddress();
        result.selectedServer->decreaseWeight();
        return shared_ptr<SocketLike>();
    }

    return asSocketLike(result.forward);
}

void JokerClient::exchangeAsync(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward)
{
    operations->spawn([request, forward] {
        Exchanger exchanger(request, forward, 1024 * 1024 * 8);
        exchanger.exchange();
    });
}

void JokerClient::exchangeSync(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward)
{
    Exchanger exchanger(request, forward, 1024 * 1024 * 8);
    exchanger.exchange();
}

void JokerClient::logRequest(const string &type, const string &hostName, uint16_t port, const HostAddress &realIP,
                             bool success)
{
    string serverName;
    if (lastSelectedServer) {
        serverName = lastSelectedServer->name;
    }
    const string successStr = success ? "SUCC" : "FAIL";
    const string msg = formatMessage("[%1,%2,%3] %4 -- %5:%6 -> %7",
                                     {type,
                                      successStr,
                                      serverName,
                                      DateTime::currentDateTimeUtc().toString(),
                                      hostName,
                                      number(port),
                                      realIP.toString()});
    printf("%s\n", msg.c_str());
}
