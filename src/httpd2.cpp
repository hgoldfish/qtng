using namespace std;

#include "qtng/httpd.h"
#include "qtng/http.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/url.h"
#include "qtng/utils/logging.h"

namespace qtng {

void BaseHttpProxyRequestHandler::logRequest(HttpStatus, int) { }

void BaseHttpProxyRequestHandler::logError(HttpStatus, const string &, const string &) { }

void BaseHttpProxyRequestHandler::doMethod()
{
    if (!asReversed && utils::toUpper(method) == "CONNECT") {
        doCONNECT();
    } else {
        doProxy();
    }
}

void BaseHttpProxyRequestHandler::doCONNECT()
{
    string host;
    uint16_t port;

    const vector<string> &l = utils::split(path, ':');
    if (l.size() != 2) {
        logProxy(string(), 0, HostAddress(), false);
        sendError(HttpStatus::BadRequest, "Invalid host and port.");
        return;
    }
    host = l[0];
    bool ok = false;
    int portValue = utils::parseInt(l[1], &ok);
    if (!ok || portValue <= 0 || portValue > 65535) {
        logProxy(host, port, HostAddress(), false);
        sendError(HttpStatus::BadRequest, "Invalid port.");
        return;
    }
    port = static_cast<uint16_t>(portValue);

    HostAddress forwardAddress;
    shared_ptr<SocketLike> forward = makeConnection(host, port, &forwardAddress);
    if (!forward) {
        sendError(HttpStatus::BadGateway, "Can not connect to remote host.");
        logProxy(host, port, HostAddress(), false);
        return;
    }

    sendResponse(OK, "Connection established");
    if (!endHeader()) {
        return;
    }

    logProxy(host, port, forwardAddress, true);
    closeConnection = Yes;
    exchangeAsync(request, forward);
    request.reset();
}

void BaseHttpProxyRequestHandler::doProxy()
{
    shared_ptr<SocketLike> forward;
    string host;
    uint16_t port = 0;
    HostAddress forwardAddress;
    if (!asReversed) {
        utils::Url url(path);
        host = url.host();
        int t;
        if (url.scheme() == "https") {
            t = url.port() > 0 ? url.port() : 443;
        } else if (url.scheme() == "http") {
            t = url.port() > 0 ? url.port() : 80;
        } else {
            t = url.port();
        }
        if (t <= 0 || t > 65535) {
            logProxy(host, 0, HostAddress(), false);
            sendError(HttpStatus::BadRequest, "Invalid port.");
            return;
        }
        port = static_cast<uint16_t>(t);
        forward = makeConnection(host, port, &forwardAddress);
        if (!forward) {
            sendError(HttpStatus::BadGateway, "Can not connect to remote host.");
            logProxy(host, port, HostAddress(), false);
            return;
        }
    }

    HttpRequest newRequest;
    newRequest.setUrl(this->path);
    newRequest.setMethod(method);
    newRequest.setVersion(this->version);
    newRequest.useConnection(forward);
    newRequest.setStreamResponse(true);
    newRequest.disableRedirects();
    shared_ptr<FileLike> bodyFile = bodyAsFile(false);
    if (!bodyFile) {
        return;
    }
    newRequest.setBody(bodyFile);

    for (const HttpHeader &header : allHeaders()) {
        const string &hn = utils::toLower(header.name);
        if (utils::startsWith(hn, "proxy-") || hn == "connection") {
            continue;
        }
        newRequest.addHeader(header);
    }

    shared_ptr<HttpResponse> response = sendRequest(newRequest);
    if (!response || (!response->isOk() && !response->hasHttpError())) {
        sendError(HttpStatus::BadGateway, response->error()->what());
        logProxy(host, port, forwardAddress, false);
        return;
    }

    logProxy(host, port, forwardAddress, true);
    sendCommandLine(static_cast<HttpStatus>(response->statusCode()), response->statusText());

    for (const HttpHeader &header : response->allHeaders()) {
        const string &hn = utils::toLower(header.name);
        if (utils::startsWith(hn, "proxy-") || hn == "connection") {
            continue;
        }
        sendHeader(header.name, header.value);
    }
    if (!endHeader()) {
        return;
    }
    if (utils::toUpper(method) != "HEAD") {
        shared_ptr<FileLike> f = response->bodyAsFile(false);
        if (f && !sendfile(f, this->request)) {
            closeConnection = Yes;
            this->request->close();
        }
    }
}

void BaseHttpProxyRequestHandler::logProxy(const string &remoteHostName, uint16_t remotePort,
                                           const HostAddress &forwardAddress, bool success)
{
    const string successStr = success ? "SUCC" : "FAIL";
    const string now = utils::DateTime::currentDateTimeUtc().toString("%Y-%m-%dT%H:%M:%SZ");
    string msg;
    if (remoteHostName.empty() || forwardAddress.isNull()) {
        msg = utils::formatMessage("[%1 %2] %3", {now, successStr, path});
    } else {
        msg = utils::formatMessage("[%1 %2] -- %3:%4 -> %5",
                                         {now, successStr, remoteHostName, utils::number(remotePort),
                                          forwardAddress.toString()});
    }
    printf("%s\n", msg.c_str());
}

shared_ptr<SocketLike> BaseHttpProxyRequestHandler::makeConnection(const string &remoteHostName,
                                                                       uint16_t remotePort, HostAddress *forwardAddress)
{
    shared_ptr<Socket> s(new Socket());
    if (s->connect(remoteHostName, remotePort)) {
        if (forwardAddress) {
            *forwardAddress = s->peerAddress();
        }
        return asSocketLike(s);
    } else {
        return shared_ptr<SocketLike>();
    }
}

}  // namespace qtng
