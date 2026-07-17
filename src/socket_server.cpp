#include <memory>

#include "qtng/socket_server.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.socket_server");

namespace qtng {

class BaseStreamServerPrivate
{
public:
    BaseStreamServerPrivate(BaseStreamServer *q, const HostAddress &serverAddress, uint16_t serverPort)
        : operations(new CoroutineGroup)
        , started(new Event())
        , stopped(new Event())
        , serverAddress(serverAddress)
        , userData(nullptr)
        , requestQueueSize(100)
        , serverPort(serverPort)
        , allowReuseAddress(true)
        , bound(false)
        , q_ptr(q)
    {
        started->clear();
        stopped->set();
    }
    ~BaseStreamServerPrivate() { delete operations; }
    void serveForever();
public:
    shared_ptr<SocketLike> serverSocket;
    CoroutineGroup *operations;
    shared_ptr<Event> started;
    shared_ptr<Event> stopped;
    HostAddress serverAddress;
    void *userData;
    int requestQueueSize;
    uint16_t serverPort;
    bool allowReuseAddress;
    bool bound;
private:
    BaseStreamServer * const q_ptr;
    NG_DECLARE_PUBLIC(BaseStreamServer)
};

BaseStreamServer::BaseStreamServer(const HostAddress &serverAddress, uint16_t serverPort)
    : d_ptr(new BaseStreamServerPrivate(this, serverAddress, serverPort))
{

}

BaseStreamServer::~BaseStreamServer()
{
    stop();
    delete d_ptr;
}

bool BaseStreamServer::allowReuseAddress() const
{
    NG_D(const BaseStreamServer);
    return d->allowReuseAddress;
}

void BaseStreamServer::setAllowReuseAddress(bool b)
{
    NG_D(BaseStreamServer);
    d->allowReuseAddress = b;
}

int BaseStreamServer::requestQueueSize() const
{
    NG_D(const BaseStreamServer);
    return d->requestQueueSize;
}

void BaseStreamServer::setRequestQueueSize(int requestQueueSize)
{
    NG_D(BaseStreamServer);
    d->requestQueueSize = requestQueueSize;
}

bool BaseStreamServer::serverBind()
{
    NG_D(BaseStreamServer);
    if (d->bound) {
        Socket::SocketState state = d->serverSocket->state();
        return state == Socket::BoundState || state == Socket::ListeningState;
    }

    Socket::BindMode mode;
    if (d->allowReuseAddress) {
        mode = Socket::ReuseAddressHint;
    } else {
        mode = Socket::DefaultForPlatform;
    }
    d->bound = d->serverSocket->bind(d->serverAddress, d->serverPort, mode);
#ifdef DEBUG_PROTOCOL
    if (!d->bound) {
        ngDebug() << "server can not bind to" << d->serverAddress.toString() << ":" << d->serverPort;
    }
#endif
    return d->bound;
}

bool BaseStreamServer::serverActivate()
{
    NG_D(BaseStreamServer);
    if (!d->bound) {
        return false;
    }
    if (d->serverSocket->state() == Socket::ListeningState) {
        return true;
    }
    if (d->serverSocket->state() != Socket::BoundState) {
        return false;
    }
    bool ok = d->serverSocket->listen(d->requestQueueSize);
#ifdef DEBUG_PROTOCOL
    if (!ok) {
        ngDebug() << "server can not listen to" << d->serverAddress.toString() << ":" << d->serverPort;
    }
#endif
    return ok;
}

void BaseStreamServer::serverClose()
{
    NG_D(BaseStreamServer);
    d->serverSocket->close();
}

void BaseStreamServerPrivate::serveForever()
{
    NG_Q(BaseStreamServer);
    started->set();
    stopped->clear();
    while (true) {
        shared_ptr<SocketLike> request = q->getRequest();
        if (!request) {
            break;
        }
        if (q->verifyRequest(request)) {
            operations->spawn([this, request] {
                NG_Q(BaseStreamServer);
                shared_ptr<SocketLike> sslRequest = q->prepareRequest(request);
                if (sslRequest) {
                    try {
                        q->processRequest(sslRequest);  // close request.
                        return;
                    } catch (CoroutineExitException &) {
                    } catch (...) {
                        q->handleError(sslRequest);
                    }
                    q->closeRequest(sslRequest);
                }
            });
        } else {
            request->close();
        }
        if (!q->serviceActions()) {
            break;
        }
    }
    q->serverClose();
    started->clear();
    stopped->set();
}

bool BaseStreamServer::serveForever()
{
    NG_D(BaseStreamServer);
    shared_ptr<SocketLike> serverSocket = createServer();
    if (!serverSocket) {
        return false;
    }
    d->serveForever();
    return true;
}

bool BaseStreamServer::start()
{
    NG_D(BaseStreamServer);

    if (d->started->isSet() || d->operations->has("serve")) {
        return true;
    }
    shared_ptr<SocketLike> serverSocket = createServer();
    if (!serverSocket) {
        return false;
    }
    d->operations->spawnWithName("serve", [d] { d->serveForever(); });
    return true;
}

void BaseStreamServer::stop()
{
    NG_D(BaseStreamServer);
    if (d->serverSocket) {
        serverClose();
    }
}

bool BaseStreamServer::wait()
{
    NG_D(BaseStreamServer);
    shared_ptr<Coroutine> coroutine = d->operations->get("serve");
    if (!coroutine) {
        return true;
    }
    if (coroutine->isFinished() || d->stopped->isSet()) {
        return true;
    }
    return coroutine->join();
}

bool BaseStreamServer::isSecure() const
{
    return false;
}

void BaseStreamServer::setUserData(void *data)
{
    NG_D(BaseStreamServer);
    d->userData = data;
}

void *BaseStreamServer::userData() const
{
    NG_D(const BaseStreamServer);
    return d->userData;
}

uint16_t BaseStreamServer::serverPort() const
{
    NG_D(const BaseStreamServer);
    if (d->serverPort) {
        return d->serverPort;
    } else if (d->serverSocket && d->serverSocket->isValid()) {
        return d->serverSocket->localPort();
    } else {
        return 0;
    }
}

HostAddress BaseStreamServer::serverAddress() const
{
    NG_D(const BaseStreamServer);
    return d->serverAddress;
}

shared_ptr<SocketLike> BaseStreamServer::serverSocket()
{
    NG_D(BaseStreamServer);
    if (!d->serverSocket) {
        d->serverSocket = serverCreate();
        if (!d->serverSocket) {
            return shared_ptr<SocketLike>();
        }
        if (!serverBind()) {
            serverClose();
            return shared_ptr<SocketLike>();
        }
        if (!serverActivate()) {
            serverClose();
            return shared_ptr<SocketLike>();
        }
    }
    return d->serverSocket;
}

shared_ptr<Event> BaseStreamServer::started()
{
    NG_D(BaseStreamServer);
    return d->started;
}

shared_ptr<Event> BaseStreamServer::stopped()
{
    NG_D(BaseStreamServer);
    return d->stopped;
}

bool BaseStreamServer::serviceActions()
{
    return true;
}

shared_ptr<SocketLike> BaseStreamServer::prepareRequest(shared_ptr<SocketLike> request)
{
    return request;
}

bool BaseStreamServer::verifyRequest(shared_ptr<SocketLike>)
{
    return true;
}

shared_ptr<SocketLike> BaseStreamServer::getRequest()
{
    NG_D(BaseStreamServer);
    return d->serverSocket->accept();
}

void BaseStreamServer::handleError(shared_ptr<SocketLike>) { }

void BaseStreamServer::closeRequest(shared_ptr<SocketLike> request)
{
    request->close();
}

BaseRequestHandler::BaseRequestHandler() { }

BaseRequestHandler::~BaseRequestHandler() { }

void BaseRequestHandler::run()
{
    if (!setup()) {
        return;
    }
    try {
        handle();
        finish();
    } catch (...) {
        finish();
    }
}

bool BaseRequestHandler::setup()
{
    return true;
}

void BaseRequestHandler::handle() { }

void BaseRequestHandler::finish()
{
    if (request) {
        request->close();
    }
}

}  // namespace qtng
