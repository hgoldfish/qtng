using namespace std;

#include <limits>

#include "qtng/socks5_proxy.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/platform.h"
#include "qtng/hostaddress.h"

namespace qtng {

#define S5_VERSION_5 0x05
#define S5_CONNECT 0x01
#define S5_BIND 0x02
#define S5_UDP_ASSOCIATE 0x03
#define S5_IP_V4 0x01
#define S5_DOMAINNAME 0x03
#define S5_IP_V6 0x04
#define S5_SUCCESS 0x00
#define S5_R_ERROR_SOCKS_FAILURE 0x01
#define S5_R_ERROR_CON_NOT_ALLOWED 0x02
#define S5_R_ERROR_NET_UNREACH 0x03
#define S5_R_ERROR_HOST_UNREACH 0x04
#define S5_R_ERROR_CONN_REFUSED 0x05
#define S5_R_ERROR_TTL 0x06
#define S5_R_ERROR_CMD_NOT_SUPPORTED 0x07
#define S5_R_ERROR_ADD_TYPE_NOT_SUPORTED 0x08

#define S5_AUTHMETHOD_NONE 0x00
#define S5_AUTHMETHOD_PASSWORD 0x02
#define S5_AUTHMETHOD_NOTACCEPTABLE 0xFF

#define S5_PASSWORDAUTH_VERSION 0x01

string Socks5Exception::errorString() const
{
    switch (err) {
    case ProxyConnectionRefusedError:
        return "Connection to proxy refused";
    case ProxyConnectionClosedError:
        return "Connection to proxy closed prematurely";
    case ProxyNotFoundError:
        return "Proxy host not found";
    case ProxyProtocolError:
        return "SOCKS version 5 protocol error";
    case ProxyAuthenticationRequiredError:
        return "Proxy authentication failed";
    case SocksFailure:
        return "General SOCKSv5 server failure";
    case ConnectionNotAllowed:
        return "Connection not allowed by SOCKSv5 server";
    case NetworkUnreachable:
        return "Network unreachable";
    case HostUnreachable:
        return "Host not found";
    case ConnectionRefused:
        return "Connection refused";
    case TTLExpired:
        return "TTL expired";
    case CommandNotSupported:
        return "SOCKSv5 command not supported";
    case AddressTypeNotSupported:
        return "Address type not supported";
    default:
        return "some error occured in socks5.";
    }
}

class Socks5ProxyPrivate
{
public:
    Socks5ProxyPrivate() { }
    Socks5ProxyPrivate(const string &hostName, uint16_t port, const string &user, const string &password)
        : hostName(hostName)
        , user(user)
        , password(password)
        , port(port)
    {
        capabilities |= Socks5Proxy::TunnelingCapability;
        capabilities |= Socks5Proxy::HostNameLookupCapability;
    }
public:
    shared_ptr<Socket> getControlSocket() const;
    shared_ptr<SocketLike> connect(const string &hostName, uint16_t port) const;
    shared_ptr<SocketLike> connect(const HostAddress &host, uint16_t port) const;
    shared_ptr<SocketLike> listen(uint16_t port) const;
public:
    string hostName;
    string user;
    string password;
    int capabilities;
    uint16_t port;
};

shared_ptr<Socket> Socks5ProxyPrivate::getControlSocket() const
{
    Socket::SocketError error;
    shared_ptr<Socket> s(Socket::createConnection(hostName, port, &error));
    if (!s) {
        if (error == Socket::HostNotFoundError) {
            throw Socks5Exception(Socks5Exception::ProxyNotFoundError);
        } else if (error == Socket::RemoteHostClosedError) {
            throw Socks5Exception(Socks5Exception::ProxyConnectionClosedError);
        } else if (error == Socket::ConnectionRefusedError) {
            throw Socks5Exception(Socks5Exception::ProxyConnectionRefusedError);
        } else if (error == Socket::SocketTimeoutError) {
            throw Socks5Exception(Socks5Exception::ProxyConnectionTimeoutError);
        } else {
            throw Socks5Exception(Socks5Exception::ProxyProtocolError);
        }
    }

    string helloRequest;
    helloRequest.reserve(3);
    helloRequest.push_back(static_cast<char>(S5_VERSION_5));
    helloRequest.push_back(static_cast<char>(1));
    if (!user.empty() && !password.empty()) {
        helloRequest.push_back(static_cast<char>(S5_AUTHMETHOD_PASSWORD));
    } else {
        helloRequest.push_back(static_cast<char>(S5_AUTHMETHOD_NONE));
    }

    int64_t sentBytes = s->sendall(helloRequest);
    if (sentBytes < static_cast<int64_t>(helloRequest.size())) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }

    const string &helloResponse = s->recvall(2);
    if (helloResponse.size() != 2) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }

    if (helloResponse.at(0) != S5_VERSION_5) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }

    constexpr char negOne = numeric_limits<char>::is_signed ? -1 : S5_AUTHMETHOD_NOTACCEPTABLE;
    if (helloResponse.at(1) == S5_AUTHMETHOD_PASSWORD) {
        if (user.empty() || password.empty()) {
            throw Socks5Exception(Socks5Exception::ProxyAuthenticationRequiredError);
        }
        string authRequest;
        authRequest.reserve(3 + user.size() + password.size());
        authRequest.push_back(static_cast<char>(S5_PASSWORDAUTH_VERSION));
        authRequest.push_back(static_cast<char>(user.size()));
        authRequest.append(user);
        authRequest.push_back(static_cast<char>(password.size()));
        authRequest.append(password);
        sentBytes = s->sendall(authRequest);
        if (sentBytes < static_cast<int64_t>(authRequest.size())) {
            throw Socks5Exception(Socks5Exception::ProxyProtocolError);
        }
        const string authResponse = s->recvall(2);
        if (authResponse.size() != 2) {
            throw Socks5Exception(Socks5Exception::ProxyProtocolError);
        }
        if (authResponse.at(0) != S5_PASSWORDAUTH_VERSION) {
            throw Socks5Exception(Socks5Exception::ProxyProtocolError);
        }
        if (authResponse.at(1) != 0x0) {
            throw Socks5Exception(Socks5Exception::ProxyAuthenticationRequiredError);
        }
    } else if (helloResponse.at(1) == negOne) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }
    return s;
}

static string makeConnectRequest()
{
    string connectRequest;
    connectRequest.reserve(270);  // big enough for domain name;
    connectRequest.push_back(static_cast<char>(S5_VERSION_5));
    connectRequest.push_back(static_cast<char>(S5_CONNECT));
    connectRequest.push_back(static_cast<char>(0x00));
    return connectRequest;
}

static bool qt_socks5_set_host_address_and_port(const HostAddress &address, uint16_t port, string *pBuf)
{
    union {
        uint16_t port;
        uint32_t ipv4;
        IPv6Address ipv6;
        char ptr;
    } data;

    if (address.protocol() == HostAddress::IPv4Protocol) {
        ngToBigEndian(address.toIPv4Address(nullptr), &data.ipv4);
        pBuf->push_back(static_cast<char>(S5_IP_V4));
        pBuf->append(&data.ptr, sizeof(data.ipv4));
    } else if (address.protocol() == HostAddress::IPv6Protocol) {
        data.ipv6 = address.toIPv6Address();
        pBuf->push_back(static_cast<char>(S5_IP_V6));
        pBuf->append(&data.ptr, sizeof(data.ipv6));
    } else {
        return false;
    }

    ngToBigEndian(port, &data.port);
    pBuf->append(&data.ptr, sizeof(data.port));
    return true;
}

static bool qt_socks5_set_host_name_and_port(const string &hostname, uint16_t port, string *pBuf)
{
    string encodedHostName = utils::toAce(hostname);
    string &buf = *pBuf;

    if (encodedHostName.length() > 255)
        return false;

    buf.push_back(static_cast<char>(S5_DOMAINNAME));
    buf.push_back(static_cast<char>(encodedHostName.length()));
    buf.append(encodedHostName);

    union {
        uint16_t port;
        char ptr;
    } data;
    ngToBigEndian(port, &data.port);
    buf.append(&data.ptr, sizeof(data.port));

    return true;
}

static shared_ptr<Socket> sendConnectRequest(shared_ptr<Socket> s, const string &connectRequest)
{
    int64_t sentBytes = s->sendall(connectRequest);
    if (sentBytes < static_cast<int64_t>(connectRequest.size())) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }
    const string &connectResponse = s->recvall(2);
    if (connectResponse.size() < 2) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }
    if (connectResponse.at(0) != S5_VERSION_5) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }
    int code = connectResponse.at(1);
    switch (code) {
    case S5_SUCCESS:
        break;
    case S5_R_ERROR_SOCKS_FAILURE:
        throw Socks5Exception(Socks5Exception::SocksFailure);
    case S5_R_ERROR_CON_NOT_ALLOWED:
        throw Socks5Exception(Socks5Exception::ConnectionNotAllowed);
    case S5_R_ERROR_NET_UNREACH:
        throw Socks5Exception(Socks5Exception::NetworkUnreachable);
    case S5_R_ERROR_HOST_UNREACH:
        throw Socks5Exception(Socks5Exception::HostUnreachable);
    case S5_R_ERROR_CONN_REFUSED:
        throw Socks5Exception(Socks5Exception::ConnectionRefused);
    case S5_R_ERROR_TTL:
        throw Socks5Exception(Socks5Exception::TTLExpired);
    case S5_R_ERROR_CMD_NOT_SUPPORTED:
        throw Socks5Exception(Socks5Exception::CommandNotSupported);
    case S5_R_ERROR_ADD_TYPE_NOT_SUPORTED:
        throw Socks5Exception(Socks5Exception::AddressTypeNotSupported);
    default:
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }
    const string &addressType = s->recvall(2);
    if (addressType.size() < 2) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }
    if (addressType.at(1) == S5_IP_V4) {
        const string &ipv4 = s->recvall(4);
        if (ipv4.size() < 4) {
            throw Socks5Exception(Socks5Exception::ProxyProtocolError);
        }
        HostAddress boundIp;
        boundIp.setAddress(ngFromBigEndian<uint32_t>(ipv4.data()));
        (void)boundIp;
    } else if (addressType.at(1) == S5_IP_V6) {
        string ipv6 = s->recvall(16);
        if (ipv6.size() < 16) {
            throw Socks5Exception(Socks5Exception::ProxyProtocolError);
        }
        HostAddress boundIp;
        boundIp.setAddress(reinterpret_cast<uint8_t *>(&ipv6[0]));
        (void)boundIp;
    } else if (addressType.at(1) == S5_DOMAINNAME) {
        const string &len = s->recvall(1);
        if (len.empty()) {
            throw Socks5Exception(Socks5Exception::ProxyProtocolError);
        }
        const string &hostName = s->recvall(static_cast<uint8_t>(len.at(0)));
        if (hostName.size() < static_cast<size_t>(len.at(0))) {
            throw Socks5Exception(Socks5Exception::ProxyProtocolError);
        }
        (void)hostName;
    } else {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }

    const string &portBytes = s->recvall(2);
    if (portBytes.size() < 2) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }
    uint16_t port = ngFromBigEndian<uint16_t>(portBytes.data());
    (void)port;

    return s;
}

shared_ptr<SocketLike> Socks5ProxyPrivate::connect(const string &hostName, uint16_t port) const
{
    shared_ptr<Socket> s = getControlSocket();
    string connectRequest = makeConnectRequest();

    if (!qt_socks5_set_host_name_and_port(hostName, port, &connectRequest)) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }

    shared_ptr<Socket> ret = sendConnectRequest(s, connectRequest);
    if (ret) {
        return asSocketLike(ret);
    }
    return shared_ptr<SocketLike>();
}

shared_ptr<SocketLike> Socks5ProxyPrivate::connect(const HostAddress &host, uint16_t port) const
{
    shared_ptr<Socket> s = getControlSocket();
    string connectRequest = makeConnectRequest();

    if (!qt_socks5_set_host_address_and_port(host, port, &connectRequest)) {
        throw Socks5Exception(Socks5Exception::ProxyProtocolError);
    }

    shared_ptr<Socket> ret = sendConnectRequest(s, connectRequest);
    if (ret) {
        return asSocketLike(ret);
    }
    return shared_ptr<SocketLike>();
}

shared_ptr<SocketLike> Socks5ProxyPrivate::listen(uint16_t port) const
{
    (void)port;
    return shared_ptr<SocketLike>();
}

Socks5Proxy::Socks5Proxy()
    : d_ptr(new Socks5ProxyPrivate)
{
}

Socks5Proxy::Socks5Proxy(const string &hostName, uint16_t port, const string &user, const string &password)
    : d_ptr(new Socks5ProxyPrivate(hostName, port, user, password))
{
}

Socks5Proxy::Socks5Proxy(const Socks5Proxy &other)
    : d_ptr(new Socks5ProxyPrivate(other.d_ptr->hostName, other.d_ptr->port, other.d_ptr->user, other.d_ptr->password))
{
}

Socks5Proxy::~Socks5Proxy()
{
    if (d_ptr)
        delete d_ptr;
}

Socks5Proxy &Socks5Proxy::operator=(const Socks5Proxy &other)
{
    delete d_ptr;
    d_ptr = new Socks5ProxyPrivate(other.hostName(), other.port(), other.user(), other.password());
    return *this;
}

Socks5Proxy &Socks5Proxy::operator=(Socks5Proxy &&other)
{
    delete d_ptr;
    d_ptr = nullptr;
    std::swap(d_ptr, other.d_ptr);
    return *this;
}

bool Socks5Proxy::isNull() const
{
    NG_D(const Socks5Proxy);
    return d->hostName.empty() || d->port == 0;
}

Socks5Proxy::Capabilities Socks5Proxy::capabilities() const
{
    NG_D(const Socks5Proxy);
    return d->capabilities;
}

string Socks5Proxy::hostName() const
{
    NG_D(const Socks5Proxy);
    return d->hostName;
}

uint16_t Socks5Proxy::port() const
{
    NG_D(const Socks5Proxy);
    return d->port;
}

string Socks5Proxy::user() const
{
    NG_D(const Socks5Proxy);
    return d->user;
}

string Socks5Proxy::password() const
{
    NG_D(const Socks5Proxy);
    return d->password;
}

void Socks5Proxy::setCapabilities(Socks5Proxy::Capabilities capabilities)
{
    NG_D(Socks5Proxy);
    d->capabilities = capabilities;
}

void Socks5Proxy::setHostName(const string &hostName)
{
    NG_D(Socks5Proxy);
    d->hostName = hostName;
}

void Socks5Proxy::setPort(uint16_t port)
{
    NG_D(Socks5Proxy);
    d->port = port;
}

void Socks5Proxy::setUser(const string &user)
{
    NG_D(Socks5Proxy);
    d->user = user;
}

void Socks5Proxy::setPassword(const string &password)
{
    NG_D(Socks5Proxy);
    d->password = password;
}

shared_ptr<SocketLike> Socks5Proxy::connect(const string &hostName, uint16_t port)
{
    NG_D(const Socks5Proxy);
    return d->connect(hostName, port);
}

shared_ptr<SocketLike> Socks5Proxy::connect(const HostAddress &host, uint16_t port)
{
    NG_D(const Socks5Proxy);
    return d->connect(host, port);
}

shared_ptr<SocketLike> Socks5Proxy::listen(uint16_t port)
{
    NG_D(const Socks5Proxy);
    return d->listen(port);
}

bool Socks5Proxy::operator==(const Socks5Proxy &other) const
{
    return hostName() == other.hostName() && port() == other.port() && user() == other.user()
            && password() == other.password();
}

Socks5Exception::Error Socks5Exception::error() const
{
    return err;
}

}  // namespace qtng
