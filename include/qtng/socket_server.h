#ifndef QTNG_SOCKET_SERVER_H
#define QTNG_SOCKET_SERVER_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/udp.h"
#include "qtng/socket_utils.h"
#include "qtng/coroutine_utils.h"
#ifndef QTNG_NO_CRYPTO
#include "qtng/ssl.h"
#include "qtng/utils/platform.h"
#endif

namespace qtng {

class BaseStreamServerPrivate;
class BaseStreamServer
{
public:
    BaseStreamServer(const HostAddress &serverAddress, std::uint16_t serverPort);
    BaseStreamServer(std::uint16_t serverPort)
        : BaseStreamServer(HostAddress::Any, serverPort)
    {
    }
    virtual ~BaseStreamServer();
protected:
    // these two virtual functions should be overrided by subclass.
    virtual std::shared_ptr<SocketLike> serverCreate() = 0;
    virtual void processRequest(std::shared_ptr<SocketLike> request) = 0;
public:
    bool allowReuseAddress() const;  // default to true,
    void setAllowReuseAddress(bool b);
    int requestQueueSize() const;  // default to 100
    void setRequestQueueSize(int requestQueueSize);
    bool serveForever();  // serve blocking
    bool start();  // serve in background
    void stop();  // stop serving
    bool wait();  // wait for server stopped
    virtual bool isSecure() const;  // is this ssl?
    std::shared_ptr<SocketLike> createServer() { return serverSocket(); }
public:
    void setUserData(void *data);  // the owner of data is not changed.
    void *userData() const;
public:
    std::uint16_t serverPort() const;
    HostAddress serverAddress() const;
    std::shared_ptr<SocketLike> serverSocket();
public:
    std::shared_ptr<Event> started();
    std::shared_ptr<Event> stopped();
protected:
    virtual bool serverBind();  // bind()
    virtual bool serverActivate();  // listen()
    virtual void serverClose();  // close()
    virtual bool serviceActions();  // default to nothing, called before accept next request.
    virtual std::shared_ptr<SocketLike> getRequest();  // accept();
    virtual std::shared_ptr<SocketLike>
    prepareRequest(std::shared_ptr<SocketLike> request);  // ssl handshake, default to nothing for tcp
    virtual bool verifyRequest(std::shared_ptr<SocketLike> request);
    virtual void handleError(std::shared_ptr<SocketLike> request);
    virtual void closeRequest(std::shared_ptr<SocketLike> request);
protected:
    BaseStreamServerPrivate * const d_ptr;
private:
    NG_DECLARE_PRIVATE(BaseStreamServer)
};

template<typename RequestHandler>
class TcpServer : public BaseStreamServer
{
public:
    TcpServer(const HostAddress &serverAddress, std::uint16_t serverPort)
        : BaseStreamServer(serverAddress, serverPort)
    {
    }
    TcpServer(std::uint16_t serverPort)
        : BaseStreamServer(HostAddress::Any, serverPort)
    {
    }
protected:
    virtual std::shared_ptr<SocketLike> serverCreate() override;
    virtual void processRequest(std::shared_ptr<SocketLike> request) override;
};

template<typename RequestHandler>
std::shared_ptr<SocketLike> TcpServer<RequestHandler>::serverCreate()
{
    return asSocketLike(Socket::createServer(serverAddress(), serverPort(), 0));
}

template<typename RequestHandler>
void TcpServer<RequestHandler>::processRequest(std::shared_ptr<SocketLike> request)
{
    RequestHandler handler;
    handler.request = request;
    handler.server = this;
    handler.run();
}

template<typename RequestHandler>
class KcpServer : public BaseStreamServer
{
public:
    KcpServer(const HostAddress &serverAddress, std::uint16_t serverPort)
        : BaseStreamServer(serverAddress, serverPort)
    {
    }
    KcpServer(std::uint16_t serverPort)
        : BaseStreamServer(HostAddress::Any, serverPort)
    {
    }
protected:
    virtual std::shared_ptr<SocketLike> serverCreate() override;
    virtual void processRequest(std::shared_ptr<SocketLike> request) override;
};

template<typename RequestHandler>
std::shared_ptr<SocketLike> KcpServer<RequestHandler>::serverCreate()
{
    return asSocketLike(KcpSocket::createServer(serverAddress(), serverPort(), 0));
}

template<typename RequestHandler>
void KcpServer<RequestHandler>::processRequest(std::shared_ptr<SocketLike> request)
{
    RequestHandler handler;
    handler.request = request;
    handler.server = this;
    handler.run();
}

#ifndef QTNG_NO_CRYPTO

template<typename ServerType>
class WithSsl : public ServerType
{
public:
    WithSsl(const HostAddress &serverAddress, std::uint16_t serverPort);
    WithSsl(const HostAddress &serverAddress, std::uint16_t serverPort, const SslConfiguration &configuration);
    WithSsl(std::uint16_t serverPort);
    WithSsl(std::uint16_t serverPort, const SslConfiguration &configuration);
public:
    void setSslConfiguration(const SslConfiguration &configuration);
    SslConfiguration sslConfiguration() const;
    void setSslHandshakeTimeout(float sslHandshakeTimeout);
    float sslHandshakeTimeout() const;
    virtual bool isSecure() const override;
protected:
    virtual std::shared_ptr<SocketLike> prepareRequest(std::shared_ptr<SocketLike> request) override;
private:
    SslConfiguration _configuration;
    float _sslHandshakeTimeout;
};

template<typename ServerType>
WithSsl<ServerType>::WithSsl(const HostAddress &serverAddress, std::uint16_t serverPort)
    : ServerType(serverAddress, serverPort)
    , _sslHandshakeTimeout(5.0)
{
    _configuration = SslConfiguration::testPurpose("SslServer", "CN",
                                                   "qtng");
}

template<typename ServerType>
WithSsl<ServerType>::WithSsl(const HostAddress &serverAddress, std::uint16_t serverPort,
                             const SslConfiguration &configuration)
    : ServerType(serverAddress, serverPort)
    , _configuration(configuration)
    , _sslHandshakeTimeout(5.0)
{
}

template<typename ServerType>
WithSsl<ServerType>::WithSsl(std::uint16_t serverPort)
    : ServerType(HostAddress::Any, serverPort)
    , _sslHandshakeTimeout(5.0)
{
    _configuration = SslConfiguration::testPurpose("SslServer", "CN",
                                                   "qtng");
}

template<typename ServerType>
WithSsl<ServerType>::WithSsl(std::uint16_t serverPort, const SslConfiguration &configuration)
    : ServerType(HostAddress::Any, serverPort)
    , _configuration(configuration)
    , _sslHandshakeTimeout(5.0)
{
}

template<typename ServerType>
void WithSsl<ServerType>::setSslConfiguration(const SslConfiguration &configuration)
{
    this->_configuration = configuration;
}

template<typename ServerType>
SslConfiguration WithSsl<ServerType>::sslConfiguration() const
{
    return this->_configuration;
}

template<typename ServerType>
void WithSsl<ServerType>::setSslHandshakeTimeout(float sslHandshakeTimeout)
{
    this->_sslHandshakeTimeout = sslHandshakeTimeout;
}

template<typename ServerType>
float WithSsl<ServerType>::sslHandshakeTimeout() const
{
    return this->_sslHandshakeTimeout;
}

template<typename ServerType>
bool WithSsl<ServerType>::isSecure() const
{
    return true;
}

template<typename ServerType>
std::shared_ptr<SocketLike> WithSsl<ServerType>::prepareRequest(std::shared_ptr<SocketLike> request)
{
    try {
        Timeout timeout(_sslHandshakeTimeout);
        std::shared_ptr<SslSocket> s = std::make_shared<SslSocket>(request, _configuration);
        if (s->handshake(true, std::string())) {
            return asSocketLike(s);
        }
    } catch (TimeoutException &) {
        //
    }
    return std::shared_ptr<SocketLike>();
}

template<typename RequestHandler>
class SslServer : public WithSsl<TcpServer<RequestHandler>>
{
public:
    SslServer(const HostAddress &serverAddress, std::uint16_t serverPort);
    SslServer(const HostAddress &serverAddress, std::uint16_t serverPort, const SslConfiguration &configuration);
    SslServer(std::uint16_t serverPort);
    SslServer(std::uint16_t serverPort, const SslConfiguration &configuration);
};

template<typename RequestHandler>
SslServer<RequestHandler>::SslServer(const HostAddress &serverAddress, std::uint16_t serverPort)
    : WithSsl<TcpServer<RequestHandler>>(serverAddress, serverPort)
{
}

template<typename RequestHandler>
SslServer<RequestHandler>::SslServer(const HostAddress &serverAddress, std::uint16_t serverPort,
                                     const SslConfiguration &configuration)
    : WithSsl<TcpServer<RequestHandler>>(serverAddress, serverPort, configuration)
{
}

template<typename RequestHandler>
SslServer<RequestHandler>::SslServer(std::uint16_t serverPort)
    : WithSsl<TcpServer<RequestHandler>>(serverPort)
{
}

template<typename RequestHandler>
SslServer<RequestHandler>::SslServer(std::uint16_t serverPort, const SslConfiguration &configuration)
    : WithSsl<TcpServer<RequestHandler>>(serverPort, configuration)
{
}

#endif

class BaseRequestHandler
{
public:
    BaseRequestHandler();
    virtual ~BaseRequestHandler();
public:
    void run();
protected:
    virtual bool setup();
    virtual void handle();
    virtual void finish();
    template<typename UserDataType>
    UserDataType *userData() const;
public:
    std::shared_ptr<SocketLike> request;
    BaseStreamServer *server;
};

template<typename UserDataType>
UserDataType *BaseRequestHandler::userData() const
{
    return static_cast<UserDataType *>(server->userData());
}

class Socks5RequestHandlerPrivate;
class Socks5RequestHandler : public BaseRequestHandler
{
protected:
    virtual void handle() override;
protected:
    virtual void doConnect(const std::string &hostName, const HostAddress &hostAddress, std::uint16_t port);
    virtual void doFailed(const std::string &hostName, const HostAddress &hostAddress, std::uint16_t port);
    virtual std::shared_ptr<SocketLike> makeConnection(const std::string &hostName, const HostAddress &hostAddress,
                                                      std::uint16_t port, HostAddress *forwardAddress);
    virtual void logProxy(const std::string &hostName, const HostAddress &hostAddress, std::uint16_t port,
                          const HostAddress &forwardAddress, bool success);
    virtual void exchange(std::shared_ptr<SocketLike> request, std::shared_ptr<SocketLike> forward);
protected:
    bool sendConnectReply(const HostAddress &hostAddress, std::uint16_t port);
    bool sendFailedReply();
private:
    bool handshake();
    bool parseAddress(std::string *hostName, HostAddress *addr, std::uint16_t *port);
};

}  // namespace qtng

#endif
