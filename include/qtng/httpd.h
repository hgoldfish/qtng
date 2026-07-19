#ifndef QTNG_HTTPD_H
#define QTNG_HTTPD_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qtng/socket_server.h"
#include "qtng/http_utils.h"

namespace qtng {

class BaseHttpRequestHandler : public WithHttpHeaders<BaseRequestHandler>
{
public:
    BaseHttpRequestHandler();
protected:  // most common methods to override
    virtual void doMethod();
    virtual void doGET();
    virtual void doPOST();
    virtual void doPUT();
    virtual void doDELETE();
    virtual void doPATCH();
    virtual void doHEAD();
    virtual void doOPTIONS();
    virtual void doTRACE();
    virtual void doCONNECT();
protected:  // many people also override these
    virtual std::string serverName();
    virtual void logRequest(HttpStatus status, int bodySize);
    virtual void logError(HttpStatus status, const std::string &shortMessage, const std::string &longMessage);
protected:  // http protocol, parsing request and making response.
    virtual void handle();
    virtual void handleOneRequest();
    virtual bool parseRequest();
    virtual bool sendError(HttpStatus status, const std::string &longMessage = std::string());
    virtual bool sendResponse(HttpStatus status, const std::string &longMessage = std::string());
    virtual std::string errorMessage(HttpStatus status, const std::string &shortMessage, const std::string &longMessage);
    virtual std::string errorMessageContentType();
    virtual std::string dateTimeString();
    virtual std::shared_ptr<FileLike> bodyAsFile(bool processEncoding = true);
protected:  // support web socket.
    virtual bool switchToWebSocket();
    std::vector<std::string> webSocketProtocols();
protected:  // util methods.
    void sendCommandLine(HttpStatus status, const std::string &shortMessage);
    void sendHeader(KnownHeader name, const std::string &value) { sendHeader(toString(name), value); }
    void sendHeader(const std::string &name, const std::string &value);
    bool endHeader();
    bool readBody();
protected:
    virtual std::string tryToHandleMagicCode(bool &done);
private:
    std::vector<std::string> headers;  // used for sendHeader() & endHeader()
public:
    static std::string normalizePath(const std::string &path);
protected:
    std::string method;  // sent by client.
    std::string path;  // sent by client.
    std::string body;  // sent by client.
    HttpVersion version;  // sent by client.
protected:
    HttpVersion serverVersion;  // default to HTTP 1.1
    float requestTimeout;  // default to 1 hour.
    std::int32_t maxBodySize;  // default to 32MB, unlimited if -1
    enum CloseConnectionStatus { Yes, No, Maybe } closeConnection;  // determined by http version and connection header.
};

// we do a nginx.
class StaticHttpRequestHandler : public BaseHttpRequestHandler
{
public:
    StaticHttpRequestHandler()
        : enableDirectoryListing(false)
    {
    }
protected:
    virtual std::shared_ptr<FileLike> serveStaticFiles(const PosixPath &dir, const std::string &subPath);
    virtual std::shared_ptr<FileLike> listDirectory(const PosixPath &dir, const std::string &displayDir);
    virtual bool loadMissingFile(const PosixPath &fileInfo);
    virtual PosixPath getIndexFile(const PosixPath &dir);
protected:
    bool enableDirectoryListing;
};

class SimpleHttpRequestHandler : public StaticHttpRequestHandler
{
public:
    SimpleHttpRequestHandler()
        : StaticHttpRequestHandler()
        , rootDir(PosixPath::cwd())
    {
    }
public:
    void setRootDir(const PosixPath &rootDir) { this->rootDir = rootDir; }
protected:
    virtual void doGET() override;
    virtual void doHEAD() override;
protected:
    PosixPath rootDir;
};

class BaseHttpProxyRequestHandler : public BaseHttpRequestHandler
{
protected:
    virtual void logRequest(qtng::HttpStatus status, int bodySize) override;
    virtual void logError(qtng::HttpStatus status, const std::string &shortMessage, const std::string &longMessage) override;
    virtual void doMethod() override;
    virtual void doCONNECT() override;
protected:
    virtual void doProxy();
protected:
    virtual void logProxy(const std::string &remoteHostName, std::uint16_t remotePort, const HostAddress &forwardAddress,
                          bool success);
    virtual std::shared_ptr<SocketLike> makeConnection(const std::string &remoteHostName, std::uint16_t remotePort,
                                                      HostAddress *forwardAddress);
protected:
    virtual std::shared_ptr<class HttpResponse> sendRequest(class HttpRequest &request) = 0;
    virtual void exchangeAsync(std::shared_ptr<SocketLike> request, std::shared_ptr<SocketLike> forward) = 0;
    bool asReversed = false;
};

// static http(s) server serving current directory.
class SimpleHttpServer : public TcpServer<SimpleHttpRequestHandler>
{
public:
    SimpleHttpServer(const HostAddress &serverAddress, std::uint16_t serverPort)
        : TcpServer(serverAddress, serverPort)
    {
    }
    SimpleHttpServer(std::uint16_t serverPort)
        : TcpServer(HostAddress::Any, serverPort)
    {
    }
};

#ifndef QTNG_NO_CRYPTO
class SimpleHttpsServer : public SslServer<SimpleHttpRequestHandler>
{
public:
    SimpleHttpsServer(const HostAddress &serverAddress, std::uint16_t serverPort)
        : SslServer(serverAddress, serverPort)
    {
    }
    SimpleHttpsServer(const HostAddress &serverAddress, std::uint16_t serverPort, const SslConfiguration &configuration)
        : SslServer(serverAddress, serverPort, configuration)
    {
    }
    SimpleHttpsServer(std::uint16_t serverPort)
        : SslServer(serverPort)
    {
    }
    SimpleHttpsServer(std::uint16_t serverPort, const SslConfiguration &configuration)
        : SslServer(serverPort, configuration)
    {
    }
};
#endif

}  // namespace qtng

#endif
