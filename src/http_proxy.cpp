using namespace std;

#include <memory>
#include <string>
#include <vector>

#include "qtng/http_proxy.h"
#include "qtng/utils/string_utils.h"

namespace qtng {

class HttpProxyPrivate
{
public:
    HttpProxyPrivate() { }
    HttpProxyPrivate(const string &hostName, uint16_t port, const string &user, const string &password)
        : hostName(hostName)
        , user(user)
        , password(password)
        , port(port)
    {
    }
public:
    string hostName;
    string user;
    string password;
    uint16_t port;
};

HttpProxy::HttpProxy()
    : d_ptr(new HttpProxyPrivate)
{
}

HttpProxy::HttpProxy(const string &hostName, uint16_t port, const string &user, const string &password)
    : d_ptr(new HttpProxyPrivate(hostName, port, user, password))
{
}

HttpProxy::HttpProxy(const HttpProxy &other)
    : d_ptr(new HttpProxyPrivate(other.d_ptr->hostName, other.d_ptr->port, other.d_ptr->user, other.d_ptr->password))
{
}

HttpProxy::~HttpProxy()
{
    delete d_ptr;
}

HttpProxy &HttpProxy::operator=(const HttpProxy &other)
{
    NG_D(HttpProxy);
    d->user = other.d_ptr->user;
    d->hostName = other.d_ptr->hostName;
    d->password = other.d_ptr->password;
    d->port = other.d_ptr->port;
    return *this;
}

HttpProxy &HttpProxy::operator=(HttpProxy &&other)
{
    delete d_ptr;
    d_ptr = new HttpProxyPrivate(other.hostName(), other.port(), other.user(), other.password());
    return *this;
}

bool HttpProxy::operator==(const HttpProxy &other) const
{
    NG_D(const HttpProxy);
    return d->user == other.d_ptr->user && d->hostName == other.d_ptr->hostName && d->password == other.d_ptr->password
            && d->port == other.d_ptr->port;
}

shared_ptr<SocketLike> HttpProxy::connect(const string &remoteHost, uint16_t port)
{
    NG_D(HttpProxy);
    if (remoteHost.empty()) {
        return shared_ptr<SocketLike>();
    }
    shared_ptr<Socket> connection(Socket::createConnection(d->hostName, d->port));
    if (!connection) {
        return shared_ptr<SocketLike>();
    }

    vector<string> lines;
    const string &firstLine = string("CONNECT ") + remoteHost + string(":")
            + utils::number(port) + string(" HTTP/1.1\r\n");
    const string &secondLine = string("Host: ") + remoteHost + string("\r\n");
    lines.push_back(firstLine);
    lines.push_back(secondLine);
    lines.push_back("Proxy-Connection: keep-alive\r\n");
    lines.push_back("User-Agent: Mozilla/5.0\r\n");
    lines.push_back("\r\n");
    const string &headersBytes = utils::join(lines, "");
    if (connection->sendall(headersBytes) != headersBytes.size()) {
        return shared_ptr<SocketLike>();
    }

    HeaderSplitter headerSplitter(asSocketLike(connection), 3);
    HeaderSplitter::Error headerSplitterError;
    string statusLine = headerSplitter.nextLine(&headerSplitterError);
    if (statusLine.empty() || headerSplitterError != HeaderSplitter::NoError) {
        return shared_ptr<SocketLike>();
    }
    vector<string> commands = utils::split(statusLine, ' ');
    if (commands.size() < 3) {
        return shared_ptr<SocketLike>();
    }
    if (commands[0] != "HTTP/1.0" && commands[0] != "HTTP/1.1") {
        return shared_ptr<SocketLike>();
    }
    bool ok = false;
    if (utils::parseInt(commands[1], &ok) != 200) {
        return shared_ptr<SocketLike>();
    }
    const int MaxHeaders = 64;
    headerSplitter.headers(MaxHeaders, &headerSplitterError);
    if (headerSplitterError != HeaderSplitter::NoError) {
        return shared_ptr<SocketLike>();
    }
    return asSocketLike(connection);
}

shared_ptr<SocketLike> HttpProxy::connect(const HostAddress &remoteHost, uint16_t port)
{
    if (remoteHost.isNull()) {
        return shared_ptr<SocketLike>();
    }
    string hostName;
    if (remoteHost.protocol() == HostAddress::IPv6Protocol) {
        hostName = utils::formatMessage("[%1]", {remoteHost.toString()});
    } else {
        hostName = remoteHost.toString();
    }
    return connect(hostName, port);
}

string HttpProxy::hostName() const
{
    NG_D(const HttpProxy);
    return d->hostName;
}

uint16_t HttpProxy::port() const
{
    NG_D(const HttpProxy);
    return d->port;
}

string HttpProxy::user() const
{
    NG_D(const HttpProxy);
    return d->user;
}

string HttpProxy::password() const
{
    NG_D(const HttpProxy);
    return d->password;
}

void HttpProxy::setHostName(const string &hostName)
{
    NG_D(HttpProxy);
    d->hostName = hostName;
}

void HttpProxy::setPort(uint16_t port)
{
    NG_D(HttpProxy);
    d->port = port;
}

void HttpProxy::setUser(const string &user)
{
    NG_D(HttpProxy);
    d->user = user;
}

void HttpProxy::setPassword(const string &password)
{
    NG_D(HttpProxy);
    d->password = password;
}

BaseProxySwitcher::BaseProxySwitcher() { }
BaseProxySwitcher::~BaseProxySwitcher() { }

shared_ptr<SocketProxy> SimpleProxySwitcher::selectSocketProxy(const string &url)
{
    (void)(url);
    if (!socketProxies.empty()) {
        return socketProxies.front();
    } else if (!httpProxies.empty()) {
        return httpProxies.front();
    }
    return shared_ptr<SocketProxy>();
}

shared_ptr<HttpProxy> SimpleProxySwitcher::selectHttpProxy(const string &url)
{
    (void)(url);
    if (httpProxies.size() > 0) {
        return httpProxies[0];
    }
    return shared_ptr<HttpProxy>();
}

// implemented in http.cpp
void setProxySwitcher(class HttpSession *session, shared_ptr<BaseProxySwitcher> switcher);

}  // namespace qtng
