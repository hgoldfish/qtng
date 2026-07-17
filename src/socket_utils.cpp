using namespace std;

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/coroutine_utils.h"
#include "qtng/socket_utils.h"

namespace qtng {

SocketLike::SocketLike() { }

SocketLike::~SocketLike() { }

int32_t SocketLike::read(char *data, int32_t size)
{
    return recv(data, size);
}

int32_t SocketLike::write(const char *data, int32_t size)
{
    return sendall(data, size);
}

int64_t SocketLike::size()
{
    return -1;
}

namespace {
class SocketLikeImpl : public SocketLike
{
public:
    SocketLikeImpl(shared_ptr<Socket> s);
public:
    virtual Socket::SocketError error() const override;
    virtual string errorString() const override;
    virtual bool isValid() const override;
    virtual HostAddress localAddress() const override;
    virtual uint16_t localPort() const override;
    virtual HostAddress peerAddress() const override;
    virtual string peerName() const override;
    virtual uint16_t peerPort() const override;
    virtual intptr_t fileno() const override;
    virtual Socket::SocketType type() const override;
    virtual Socket::SocketState state() const override;
    virtual HostAddress::NetworkLayerProtocol protocol() const override;
    virtual string localAddressURI() const override;
    virtual string peerAddressURI() const override;

    virtual Socket *acceptRaw() override;
    virtual shared_ptr<SocketLike> accept() override;
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override;
    virtual bool bind(uint16_t port, Socket::BindMode mode) override;
    virtual bool connect(const HostAddress &addr, uint16_t port) override;
    virtual bool connect(const string &hostName, uint16_t port,
                         shared_ptr<SocketDnsCache> dnsCache = shared_ptr<SocketDnsCache>()) override;
    virtual void close() override;
    virtual void abort() override;
    virtual bool listen(int backlog) override;
    virtual bool setOption(Socket::SocketOption option, int value) override;
    virtual int option(Socket::SocketOption option) const override;

    virtual int32_t peek(char *data, int32_t size) override;
    virtual int32_t peekRaw(char *data, int32_t size) override;
    virtual int32_t recv(char *data, int32_t size) override;
    virtual int32_t recvall(char *data, int32_t size) override;
    virtual int32_t send(const char *data, int32_t size) override;
    virtual int32_t sendall(const char *data, int32_t size) override;
    virtual string recv(int32_t size) override;
    virtual string recvall(int32_t size) override;
    virtual int32_t send(const string &data) override;
    virtual int32_t sendall(const string &data) override;
public:
    shared_ptr<Socket> s;
};

SocketLikeImpl::SocketLikeImpl(shared_ptr<Socket> s)
    : s(s)
{
}

Socket::SocketError SocketLikeImpl::error() const
{
    return s->error();
}

string SocketLikeImpl::errorString() const
{
    return s->errorString();
}

bool SocketLikeImpl::isValid() const
{
    return s->isValid();
}

HostAddress SocketLikeImpl::localAddress() const
{
    return s->localAddress();
}

uint16_t SocketLikeImpl::localPort() const
{
    return s->localPort();
}

HostAddress SocketLikeImpl::peerAddress() const
{
    return s->peerAddress();
}

string SocketLikeImpl::peerName() const
{
    return s->peerName();
}

uint16_t SocketLikeImpl::peerPort() const
{
    return s->peerPort();
}

intptr_t SocketLikeImpl::fileno() const
{
    return s->fileno();
}

Socket::SocketType SocketLikeImpl::type() const
{
    return s->type();
}

Socket::SocketState SocketLikeImpl::state() const
{
    return s->state();
}

HostAddress::NetworkLayerProtocol SocketLikeImpl::protocol() const
{
    return s->protocol();
}

string SocketLikeImpl::localAddressURI() const
{
    return s->localAddressURI();
}

string SocketLikeImpl::peerAddressURI() const
{
    return s->peerAddressURI();
}

Socket *SocketLikeImpl::acceptRaw()
{
    return s->accept();
}

shared_ptr<SocketLike> SocketLikeImpl::accept()
{
    Socket *r = s->accept();
    if (r) {
        return asSocketLike(r);
    } else {
        return shared_ptr<SocketLike>();
    }
}

bool SocketLikeImpl::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    return s->bind(address, port, mode);
}

bool SocketLikeImpl::bind(uint16_t port, Socket::BindMode mode)
{
    return s->bind(port, mode);
}

bool SocketLikeImpl::connect(const HostAddress &addr, uint16_t port)
{
    return s->connect(addr, port);
}

bool SocketLikeImpl::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    return s->connect(hostName, port, dnsCache);
}

void SocketLikeImpl::close()
{
    s->close();
}

void SocketLikeImpl::abort()
{
    s->abort();
}

bool SocketLikeImpl::listen(int backlog)
{
    return s->listen(backlog);
}

bool SocketLikeImpl::setOption(Socket::SocketOption option, int value)
{
    return s->setOption(option, value);
}

int SocketLikeImpl::option(Socket::SocketOption option) const
{
    return s->option(option);
}

int32_t SocketLikeImpl::peek(char *data, int32_t size) 
{
    return s->peek(data, size);
}

int32_t SocketLikeImpl::peekRaw(char *data, int32_t size)
{
    return s->peek(data, size);
}

int32_t SocketLikeImpl::recv(char *data, int32_t size)
{
    return s->recv(data, size);
}

int32_t SocketLikeImpl::recvall(char *data, int32_t size)
{
    return s->recvall(data, size);
}

int32_t SocketLikeImpl::send(const char *data, int32_t size)
{
    return s->send(data, size);
}

int32_t SocketLikeImpl::sendall(const char *data, int32_t size)
{
    return s->sendall(data, size);
}

string SocketLikeImpl::recv(int32_t size)
{
    return s->recv(size);
}

string SocketLikeImpl::recvall(int32_t size)
{
    return s->recvall(size);
}

int32_t SocketLikeImpl::send(const string &data)
{
    return s->send(data);
}

int32_t SocketLikeImpl::sendall(const string &data)
{
    return s->sendall(data);
}

}  // anonymous namespace

shared_ptr<SocketLike> asSocketLike(shared_ptr<Socket> s)
{
    if (!s) {
        return shared_ptr<SocketLike>();
    }
    return static_pointer_cast<SocketLike>(make_shared<SocketLikeImpl>(s));
}

shared_ptr<Socket> convertSocketLikeToSocket(shared_ptr<SocketLike> socket)
{
    auto impl = dynamic_pointer_cast<SocketLikeImpl>(socket);
    if (!impl) {
        return shared_ptr<Socket>();
    } else {
        return impl->s;
    }
}

class ExchangerPrivate
{
public:
    ExchangerPrivate(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward, uint32_t maxBufferSize);
    ~ExchangerPrivate();
public:
    void in2out();
    void out2in();
public:
    shared_ptr<SocketLike> request;
    shared_ptr<SocketLike> forward;
    CoroutineGroup *operations;
    uint32_t maxBufferSize;
};

ExchangerPrivate::ExchangerPrivate(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward,
                                   uint32_t maxBufferSize)
    : request(request)
    , forward(forward)
    , operations(new CoroutineGroup)
    , maxBufferSize(maxBufferSize)
{
}

ExchangerPrivate::~ExchangerPrivate()
{
    delete operations;
}

void ExchangerPrivate::in2out()
{
    if (!sendfile(request, forward, -1, maxBufferSize)) {
        request->abort();
        forward->abort();
    } else {
        request->close();
        forward->close();
    }
}

void ExchangerPrivate::out2in()
{
    if (!sendfile(forward, request, -1, maxBufferSize)) {
        request->abort();
        forward->abort();
    } else {
        request->close();
        forward->close();
    }
}

Exchanger::Exchanger(shared_ptr<SocketLike> request, shared_ptr<SocketLike> forward, uint32_t maxBufferSize)
    : d_ptr(new ExchangerPrivate(request, forward, maxBufferSize))
{
}

Exchanger::~Exchanger()
{
    delete d_ptr;
}

void Exchanger::exchange()
{
    NG_D(Exchanger);
    d->operations->spawnWithName("in2out", [d] { d->in2out(); });
    d->operations->spawnWithName("out2in", [d] { d->out2in(); });
    d->operations->joinall();
}

}  // namespace qtng
