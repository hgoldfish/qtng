#include "bridge/core_access.h"
#include "bridge/io_bridge.h"
#include "socket_server.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class BaseStreamServerPrivate
{
public:
    BaseStreamServerPrivate(BaseStreamServer *q, const HostAddress &serverAddress, quint16 serverPort)
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

    QSharedPointer<SocketLike> serverSocket;
    CoroutineGroup *operations;
    QSharedPointer<Event> started;
    QSharedPointer<Event> stopped;
    HostAddress serverAddress;
    void *userData;
    int requestQueueSize;
    quint16 serverPort;
    bool allowReuseAddress;
    bool bound;
    BaseStreamServer *q_ptr;
};

BaseStreamServer::BaseStreamServer(const HostAddress &serverAddress, quint16 serverPort)
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
    Q_D(const BaseStreamServer);
    return d->allowReuseAddress;
}

void BaseStreamServer::setAllowReuseAddress(bool b)
{
    Q_D(BaseStreamServer);
    d->allowReuseAddress = b;
}

int BaseStreamServer::requestQueueSize() const
{
    Q_D(const BaseStreamServer);
    return d->requestQueueSize;
}

void BaseStreamServer::setRequestQueueSize(int requestQueueSize)
{
    Q_D(BaseStreamServer);
    d->requestQueueSize = requestQueueSize;
}

bool BaseStreamServer::serverBind()
{
    Q_D(BaseStreamServer);
    if (d->bound) {
        const Socket::SocketState state = d->serverSocket->state();
        return state == Socket::BoundState || state == Socket::ListeningState;
    }
    const Socket::BindMode mode = d->allowReuseAddress ? Socket::ReuseAddressHint : Socket::DefaultForPlatform;
    d->bound = d->serverSocket->bind(d->serverAddress, d->serverPort, mode);
    return d->bound;
}

bool BaseStreamServer::serverActivate()
{
    Q_D(BaseStreamServer);
    if (!d->bound) {
        return false;
    }
    if (d->serverSocket->state() == Socket::ListeningState) {
        return true;
    }
    if (d->serverSocket->state() != Socket::BoundState) {
        return false;
    }
    return d->serverSocket->listen(d->requestQueueSize);
}

void BaseStreamServer::serverClose()
{
    Q_D(BaseStreamServer);
    if (d->serverSocket) {
        d->serverSocket->close();
    }
}

class ServingStateGuard
{
public:
    ServingStateGuard(QSharedPointer<Event> started, QSharedPointer<Event> stopped)
        : started(started)
        , stopped(stopped)
    {
        this->started->set();
        this->stopped->clear();
    }
    ~ServingStateGuard()
    {
        started->clear();
        stopped->set();
    }

private:
    QSharedPointer<Event> started;
    QSharedPointer<Event> stopped;
};

void BaseStreamServerPrivate::serveForever()
{
    ServingStateGuard servingStateGuard(started, stopped);
    while (true) {
        QSharedPointer<SocketLike> request = q_ptr->getRequest();
        if (!request) {
            break;
        }
        if (q_ptr->verifyRequest(request)) {
            operations->spawn([this, request] {
                QSharedPointer<SocketLike> prepared = q_ptr->prepareRequest(request);
                if (prepared) {
                    try {
                        q_ptr->processRequest(prepared);
                        return;
                    } catch (CoroutineExitException &) {
                    } catch (...) {
                        q_ptr->handleError(prepared);
                    }
                    q_ptr->closeRequest(prepared);
                }
            });
        } else {
            request->close();
        }
        if (!q_ptr->serviceActions()) {
            break;
        }
    }
    q_ptr->serverClose();
}

bool BaseStreamServer::serveForever()
{
    Q_D(BaseStreamServer);
    if (!serverSocket()) {
        return false;
    }
    d->serveForever();
    return true;
}

bool BaseStreamServer::start()
{
    Q_D(BaseStreamServer);
    if (d->started->isSet() || d->operations->has(QString::fromLatin1("serve"))) {
        return true;
    }
    if (!serverSocket()) {
        return false;
    }
    d->operations->spawnWithName(QString::fromLatin1("serve"), [d] { d->serveForever(); });
    return true;
}

void BaseStreamServer::stop()
{
    serverClose();
}

bool BaseStreamServer::wait()
{
    Q_D(BaseStreamServer);
    QSharedPointer<Coroutine> coroutine = d->operations->get(QString::fromLatin1("serve"));
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
    Q_D(BaseStreamServer);
    d->userData = data;
}

void *BaseStreamServer::userData() const
{
    Q_D(const BaseStreamServer);
    return d->userData;
}

quint16 BaseStreamServer::serverPort() const
{
    Q_D(const BaseStreamServer);
    if (d->serverPort) {
        return d->serverPort;
    }
    if (d->serverSocket && d->serverSocket->isValid()) {
        return d->serverSocket->localPort();
    }
    return 0;
}

HostAddress BaseStreamServer::serverAddress() const
{
    Q_D(const BaseStreamServer);
    return d->serverAddress;
}

QSharedPointer<SocketLike> BaseStreamServer::serverSocket()
{
    Q_D(BaseStreamServer);
    if (!d->serverSocket) {
        d->serverSocket = serverCreate();
        if (!d->serverSocket) {
            return QSharedPointer<SocketLike>();
        }
        if (!serverBind()) {
            serverClose();
            return QSharedPointer<SocketLike>();
        }
        if (!serverActivate()) {
            serverClose();
            return QSharedPointer<SocketLike>();
        }
    }
    return d->serverSocket;
}

QSharedPointer<Event> BaseStreamServer::started()
{
    Q_D(BaseStreamServer);
    return d->started;
}

QSharedPointer<Event> BaseStreamServer::stopped()
{
    Q_D(BaseStreamServer);
    return d->stopped;
}

bool BaseStreamServer::serviceActions()
{
    return true;
}

QSharedPointer<SocketLike> BaseStreamServer::prepareRequest(QSharedPointer<SocketLike> request)
{
    return request;
}

bool BaseStreamServer::verifyRequest(QSharedPointer<SocketLike>)
{
    return true;
}

QSharedPointer<SocketLike> BaseStreamServer::getRequest()
{
    Q_D(BaseStreamServer);
    return d->serverSocket->accept();
}

void BaseStreamServer::handleError(QSharedPointer<SocketLike>) { }

void BaseStreamServer::closeRequest(QSharedPointer<SocketLike> request)
{
    if (request) {
        request->close();
    }
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

}  // namespace QTNETWORKNG_NAMESPACE
