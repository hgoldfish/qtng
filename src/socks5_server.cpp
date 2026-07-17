#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "qtng/socks5_proxy.h"
#include "qtng/utils/string_utils.h"
#include "qtng/socket_server.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.socks5server");

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

namespace qtng {

void Socks5RequestHandler::doConnect(const string &hostName, const HostAddress &hostAddress, uint16_t port)
{
    HostAddress forwardAddress;
    shared_ptr<SocketLike> forward = makeConnection(hostName, hostAddress, port, &forwardAddress);
    if (!forward) {
        sendFailedReply();
        logProxy(hostName, hostAddress, port, forwardAddress, false);
        return;
    }
    if (!sendConnectReply(forwardAddress, port)) {
        logProxy(hostName, forwardAddress, port, forwardAddress, false);
        return;
    } else {
        logProxy(hostName, forwardAddress, port, forwardAddress, true);
    }
    exchange(request, forward);
}

bool Socks5RequestHandler::sendConnectReply(const HostAddress &hostAddress, uint16_t port)
{
    bool ok;
    if (hostAddress.isNull()) {
        return false;
    }
    string reply;
    uint32_t ipv4 = hostAddress.toIPv4Address(&ok);
    if (!ok && hostAddress.protocol() == HostAddress::IPv4Protocol) {
        return false;
    }
    if (ok && ipv4) {
        reply.resize(10);
        reply[0] = S5_VERSION_5;
        reply[1] = S5_SUCCESS;
        reply[2] = 0x00;
        reply[3] = S5_IP_V4;
        ngToBigEndian<uint32_t>(ipv4, &reply[4]);
        ngToBigEndian<uint16_t>(port, &reply[8]);
    } else if (hostAddress.protocol() == HostAddress::IPv6Protocol) {
        reply.resize(22);
        IPv6Address ipv6 = hostAddress.toIPv6Address();
        reply[0] = S5_VERSION_5;
        reply[1] = S5_SUCCESS;
        reply[2] = 0x00;
        reply[3] = S5_IP_V6;
        memcpy(&reply[4], reinterpret_cast<char *>(ipv6.c), 16);
        ngToBigEndian<uint16_t>(port, &reply[20]);
    }

    return request->sendall(reply) == reply.size();
}

void Socks5RequestHandler::logProxy(const string &hostName, const HostAddress &hostAddress, uint16_t port,
                                    const HostAddress &forwardAddress, bool success)
{
    const string &status = success ? "OK" : "FAIL";
    const utils::DateTime &now = utils::DateTime::currentDateTimeUtc();
    string host;
    if (hostName.empty()) {
        host = hostAddress.toString();
    } else {
        host = hostName;
    }
    const string message = utils::formatMessage("%1 -- %2 CONNECT %3 -> %4:%5 %6", {
        request->peerAddress().toString(),
        now.toString("%Y-%m-%dT%H:%M:%SZ"),
        host,
        forwardAddress.toString(),
        utils::number(port),
        status});
    printf("%s\n", message.c_str());
}

void Socks5RequestHandler::exchange(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward)
{
    Exchanger exchanger(request, forward);
    exchanger.exchange();
}

void Socks5RequestHandler::doFailed(const string &hostName, const HostAddress &hostAddress, uint16_t port)
{
    string reply(3, '\0');
    reply[0] = S5_VERSION_5;
    reply[1] = S5_R_ERROR_CMD_NOT_SUPPORTED;
    reply[2] = 0x00;
    request->sendall(reply);
    logProxy(hostName, hostAddress, port, HostAddress(), false);
}

shared_ptr<SocketLike> Socks5RequestHandler::makeConnection(const string &hostName, const HostAddress &hostAddress,
                                                                uint16_t port, HostAddress *forwardAddress)
{
    unique_ptr<Socket> s;
    if (!hostName.empty()) {
        s.reset(Socket::createConnection(hostName, port));
    } else if (!hostAddress.isNull()) {
        s.reset(Socket::createConnection(hostAddress, port));
    }
    if (s) {
        if (forwardAddress) {
            *forwardAddress = s->peerAddress();
        }
        return asSocketLike(s.release());
    } else {
        return shared_ptr<SocketLike>();
    }
}

bool Socks5RequestHandler::sendFailedReply()
{
    string reply(3, '\0');
    reply[0] = S5_VERSION_5;
    reply[1] = S5_R_ERROR_SOCKS_FAILURE;
    reply[2] = 0x00;
    return request->sendall(reply) == 3;
}

void Socks5RequestHandler::handle()
{
    if (!handshake()) {
        return;
    }
    const string &commandHeader = request->recvall(2);
    if (commandHeader.size() < 2 || commandHeader[0] != S5_VERSION_5) {
        logProxy(string(), HostAddress(), 0, HostAddress(), false);
        return;
    }

    string hostName;
    HostAddress addr;
    uint16_t port;

    if (!parseAddress(&hostName, &addr, &port)) {
        logProxy(string(), HostAddress(), 0, HostAddress(), false);
        return;
    }

    if ((hostName.empty() && addr.isNull()) || port == 0) {
        logProxy(string(), HostAddress(), 0, HostAddress(), false);
        return;
    }

    switch (commandHeader[1]) {
    case S5_CONNECT:
        doConnect(hostName, addr, port);
        break;
    default:
        ngDebug() << "unsupported command: " << commandHeader[1];
        doFailed(hostName, addr, port);
        break;
    }
}

bool Socks5RequestHandler::handshake()
{
    const string &header = request->recvall(2);
    if (header.size() != 2) {
        return false;
    }
    if (header[0] != S5_VERSION_5) {
        return false;
    }
    uint8_t methods = static_cast<uint8_t>(header[1]);
    bool ok = true;
    if (methods == 0) {
        ok = true;
    } else {
        string authMethods = request->recvall(methods);
        if (authMethods.size() != methods) {
            return false;
        }
        if (authMethods.find(static_cast<char>(S5_AUTHMETHOD_NONE)) == string::npos) {
            ok = false;
        }
    }
    string replyHeader(2, '\0');
    replyHeader[0] = S5_VERSION_5;
    if (ok) {
        replyHeader[1] = S5_AUTHMETHOD_NONE;
    } else {
        replyHeader[1] = numeric_limits<char>::is_signed ? -1 : S5_AUTHMETHOD_NOTACCEPTABLE;
    }
    int32_t sentBytes = request->sendall(replyHeader);
    if (sentBytes != replyHeader.size()) {
        ngDebug() << "can not send reply header.";
        return false;
    }
    return ok;
}

bool Socks5RequestHandler::parseAddress(string *hostName, HostAddress *addr, uint16_t *port)
{
    const string &addressType = request->recvall(2);
    if (addressType.size() < 2 || addressType[0] != 0x00) {
        return false;
    }

    if (addressType[1] == S5_IP_V4) {
        const string &ipv4 = request->recvall(4);
        if (ipv4.size() < 4) {
            return false;
        }
        addr->setAddress(ngFromBigEndian<uint32_t>(ipv4.data()));
    } else if (addressType[1] == S5_IP_V6) {
        string ipv6 = request->recvall(16);
        if (ipv6.size() < 16) {
            return false;
        }
        addr->setAddress(reinterpret_cast<uint8_t *>(&ipv6[0]));
    } else if (addressType[1] == S5_DOMAINNAME) {
        const string &len = request->recvall(1);
        if (len.empty()) {
            return false;
        }

        const string &buf = request->recvall(uint8_t(len[0]));
        if (buf.size() < len[0]) {
            return false;
        }
        *hostName = utils::fromAce(buf);
    } else {
        return false;
    }

    const string &portBytes = request->recvall(2);
    if (portBytes.size() < 2) {
        return false;
    }
    *port = ngFromBigEndian<uint16_t>(portBytes.data());
    return true;
}

}  // namespace qtng
