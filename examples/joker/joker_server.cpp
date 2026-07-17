using namespace std;

#include <cstdio>

#include "joker_server.h"

using namespace qtng;
using namespace qtng::utils;

struct JokerServerPrivate
{
    explicit JokerServerPrivate(const JokerServerConfigure &configure);
    ~JokerServerPrivate();
    bool start();
    void handleRequest(shared_ptr<SocketLike> request);
    void handleChannel(shared_ptr<VirtualChannel> channel);
    bool sendReply(shared_ptr<VirtualChannel> request, const HostAddress &addr);
    void exchange(shared_ptr<VirtualChannel> request, shared_ptr<Socket> forward);
public:
    JokerServerConfigure configure;
    shared_ptr<SocketDnsCache> dnsCache;
    CoroutineGroup *operations;
};

class JokerKcpRequestHandler : public BaseRequestHandler
{
protected:
    void handle() override;
};

class JokerHttpRequestHandler : public SimpleHttpRequestHandler
{
protected:
    bool setup() override;
    void doPOST() override;
    string serverName() override;
};

JokerServerConfigure::JokerServerConfigure()
    : timeout(8.0f)
    , kcpAddress(HostAddress::Any)
    , kcpPort(8000)
    , kcpMode(KcpSocket::Mode::Internet)
    , httpAddress(HostAddress::Any)
    , httpPort(8000)
    , httpRootDir(PosixPath("./htdocs/"))
{
    templateCipher.reset(new Cipher(Cipher::Chacha20, Cipher::CBC, Cipher::Encrypt));
    const string salt("3.14159265358979323846");
    templateCipher->setPassword("123456", salt, MessageDigest::Sha256, 100000);
}

void JokerKcpRequestHandler::handle()
{
    userData<JokerServerPrivate>()->handleRequest(request);
}

bool JokerHttpRequestHandler::setup()
{
    setRootDir(userData<JokerServerPrivate>()->configure.httpRootDir);
    return true;
}

void JokerHttpRequestHandler::doPOST()
{
    if (path == "/joker/") {
        const string body("Joker is here.");
        sendResponse(HttpStatus::OK);
        sendHeader("Content-Type", "text/plain");
        sendHeader("Content-Length", number(static_cast<int>(body.size())));
        endHeader();
        request->sendall(body);
        userData<JokerServerPrivate>()->handleRequest(request);
    } else {
        sendError(HttpStatus::BadRequest);
    }
}

string JokerHttpRequestHandler::serverName()
{
    return "Tengine";
}

JokerServerPrivate::JokerServerPrivate(const JokerServerConfigure &configure)
    : configure(configure)
    , dnsCache(make_shared<SocketDnsCache>())
    , operations(new CoroutineGroup())
{
}

JokerServerPrivate::~JokerServerPrivate()
{
    delete operations;
}

class JokerKcpServer : public KcpServer<JokerKcpRequestHandler>
{
public:
    JokerKcpServer(const HostAddress &serverAddress, uint16_t serverPort)
        : KcpServer(serverAddress, serverPort)
    {
    }
};

bool JokerServerPrivate::start()
{
    typedef TcpServer<JokerHttpRequestHandler> JokerHttpServer;

    shared_ptr<JokerKcpServer> kcpServer;
    shared_ptr<JokerHttpServer> httpServer;
    if (!configure.kcpAddress.isNull() && configure.kcpPort != 0) {
        kcpServer.reset(new JokerKcpServer(configure.kcpAddress, configure.kcpPort));
        kcpServer->setUserData(this);
        if (!kcpServer->start()) {
            return false;
        }
        if (!kcpServer->started()->tryWait()) {
            return false;
        }
    }

    if (!configure.httpAddress.isNull() && configure.httpPort != 0 && configure.httpRootDir.isReadable()) {
        httpServer.reset(new JokerHttpServer(configure.httpAddress, configure.httpPort));
        httpServer->setUserData(this);
        if (!httpServer->start()) {
            return false;
        }
        if (!httpServer->started()->tryWait()) {
            return false;
        }
    }

    if (kcpServer) {
        if (!kcpServer->stopped()->tryWait()) {
            return false;
        }
    }

    if (httpServer) {
        if (!httpServer->stopped()->tryWait()) {
            return false;
        }
    }

    return true;
}

void JokerServerPrivate::handleRequest(shared_ptr<SocketLike> request)
{
    shared_ptr<KcpSocket> kcpRequest = convertSocketLikeToKcpSocket(request);
    if (kcpRequest) {
        kcpRequest->setMode(configure.kcpMode);
    }

    shared_ptr<SocketLike> encryptedConnection = encrypted(configure.templateCipher, request);
    SocketChannel channel(encryptedConnection, DataChannelPole::NegativePole);
    channel.setKeepaliveTimeout(30);

    const string &clientProfile = channel.recvPacket();
    if (clientProfile.empty()) {
        return;
    }
    MsgPackStream mps(clientProfile);
    uint16_t version;
    uint16_t udpPacketSize;
    mps >> version >> udpPacketSize;
    if (mps.status() != MsgPackStream::Ok || version != 1) {
        return;
    }
    if (kcpRequest) {
        kcpRequest->setUdpPacketSize(udpPacketSize);
        channel.setPayloadSizeHint(kcpRequest->payloadSizeHint());
    } else {
        channel.setPayloadSizeHint(udpPacketSize);
    }
    channel.setCapacity(1024 * 64);
    const bool ok = channel.sendPacket(randomBytes(static_cast<int>(channel.payloadSizeHint())));
    if (!ok) {
        return;
    }

    while (true) {
        shared_ptr<VirtualChannel> subChannel = channel.takeChannel();
        if (!subChannel) {
            return;
        }
        operations->spawn([this, subChannel] {
            handleChannel(subChannel);
        });
    }
}

void JokerServerPrivate::handleChannel(shared_ptr<VirtualChannel> channel)
{
    string command;
    try {
        Timeout timeout(5.0f);
        (void) timeout;
        command = channel->recvPacket();
    } catch (TimeoutException &) {
        return;
    }
    if (command.empty()) {
        return;
    }

    MsgPackStream mps(command);
    uint8_t addressType = 0;
    mps >> addressType;

    shared_ptr<Socket> forward;
    string hostPart = "unknown";
    string portPart = "-1";
    try {
        Timeout connectTimeout(configure.timeout);
        if (addressType == 0x01) {
            string hostName;
            uint16_t port;
            mps >> hostName >> port;
            if (mps.status() != MsgPackStream::Ok) {
                return;
            }
            hostPart = hostName;
            portPart = number(port);
            forward.reset(Socket::createConnection(hostName, port, nullptr, dnsCache));
        } else if (addressType == 0x02) {
            uint32_t ipv4;
            uint16_t port;
            mps >> ipv4 >> port;
            if (mps.status() != MsgPackStream::Ok) {
                return;
            }
            HostAddress addr(ipv4);
            hostPart = addr.toString();
            portPart = number(port);
            forward.reset(new Socket(HostAddress::IPv4Protocol));
            if (!forward->connect(addr, port)) {
                forward.reset();
            }
        }
    } catch (TimeoutException &) {
        forward.reset();
    }

    const string message = formatMessage("%1 - %2:%3 %4",
                                       {DateTime::currentDateTimeUtc().toString(),
                                        hostPart,
                                        portPart,
                                        forward ? "OK" : "FAIL"});
    printf("%s\n", message.c_str());

    if (!forward) {
        sendReply(channel, HostAddress());
        return;
    }

    forward->setOption(Socket::ReceiveBufferSizeSocketOption, 1024 * 1024 * 32);
    forward->setOption(Socket::SendBufferSizeSocketOption, 1024 * 1024 * 32);
    if (!sendReply(channel, forward->peerAddress())) {
        return;
    }
    exchange(channel, forward);
}

bool JokerServerPrivate::sendReply(shared_ptr<VirtualChannel> channel, const HostAddress &addr)
{
    string reply;
    MsgPackStream mps(&reply, true);
    mps << addr.toString();
    return channel->sendPacket(reply);
}

void JokerServerPrivate::exchange(shared_ptr<VirtualChannel> request, shared_ptr<Socket> forward)
{
    Exchanger exchanger(asSocketLike(request), asSocketLike(forward), 1024 * 1024 * 8);
    exchanger.exchange();
}

struct JokerServer::Private : public JokerServerPrivate
{
    using JokerServerPrivate::JokerServerPrivate;
};

JokerServer::JokerServer(const JokerServerConfigure &configure)
    : d(new Private(configure))
{
}

JokerServer::~JokerServer()
{
}

bool JokerServer::start()
{
    return d->start();
}
