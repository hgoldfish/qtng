#include <cstdlib>
#include <memory>
#include <utility>

#include "qtng/local_socket.h"
#include "qtng/private/local_socket_p.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.local_socket");

namespace qtng {

LocalSocketPrivate::LocalSocketPrivate(LocalSocket *parent)
    : q_ptr(parent)
    , error(Socket::NoError)
    , state(Socket::UnconnectedState)
#ifdef NG_OS_WIN
    , fd(0)
    , acceptWait(nullptr)
    , acceptArmed(false)
    , acceptGeneration(0)
#else
    , fd(-1)
    , unlinkPath()
    , shouldUnlink(false)
#endif
{
}

LocalSocketPrivate::LocalSocketPrivate(intptr_t socketDescriptor, LocalSocket *parent)
    : LocalSocketPrivate(parent)
{
    setSocketDescriptor(socketDescriptor);
}

LocalSocketPrivate::~LocalSocketPrivate()
{
    // abort() skips the lock drain: close() would tryAcquire while another
    // coroutine may still hold a lock, and we must not hang in a destructor.
    abort();
#ifdef NG_OS_WIN
    releaseAcceptWait();
#endif
}

void LocalSocketPrivate::setError(Socket::SocketError error, const string &errorString)
{
    this->error = error;
    this->errorString = errorString;
}

void LocalSocketPrivate::setError(Socket::SocketError error, ErrorString errorString)
{
    this->error = error;
    switch (errorString) {
    case NonBlockingInitFailedErrorString:
        this->errorString = "Unable to initialize non-blocking socket";
        break;
    case RemoteHostClosedErrorString:
        this->errorString = "The remote host closed the connection";
        break;
    case TimeOutErrorString:
        this->errorString = "Network operation timed out";
        break;
    case ResourceErrorString:
        this->errorString = "Out of resources";
        break;
    case OperationUnsupportedErrorString:
        this->errorString = "Unsupported socket operation";
        break;
    case InvalidSocketErrorString:
        this->errorString = "Invalid socket descriptor";
        break;
    case AccessErrorString:
        this->errorString = "Permission denied";
        break;
    case ConnectionRefusedErrorString:
        this->errorString = "Connection refused";
        break;
    case AddressInuseErrorString:
        this->errorString = "The bound address is already in use";
        break;
    case AddressNotAvailableErrorString:
        this->errorString = "The address is not available";
        break;
    case WriteErrorString:
        this->errorString = "Unable to write";
        break;
    case ReadErrorString:
        this->errorString = "Network error";
        break;
    case OutOfMemoryErrorString:
        this->errorString = "Out of memory";
        break;
    default:
        this->errorString = "Unknown error";
        break;
    }
}

string LocalSocketPrivate::getErrorString() const
{
    return errorString;
}

bool LocalSocketPrivate::isValid() const
{
    return checkState();
}

bool LocalSocketPrivate::checkState() const
{
    // Structural liveness only. A sticky error from a rejected call (e.g. bind()
    // while already Bound) must remain visible via error(), but must not make
    // accept()/recv()/send() pretend the socket is dead while fd and state are
    // still fine. close() sets Unconnected before draining, which is the hard
    // "stop new I/O" boundary.
    if (state == Socket::UnconnectedState) {
        return false;
    }
#ifdef NG_OS_WIN
    // INVALID_HANDLE_VALUE == (HANDLE)-1; also reject null handle.
    return fd != 0 && fd != -1;
#else
    return fd >= 0;
#endif
}

string LocalSocketPrivate::makeFullServerName(const string &name)
{
    if (name.empty()) {
        return string();
    }
#ifdef NG_OS_WIN
    const string::size_type slash = name.find_first_of("/\\");
    if (slash != string::npos) {
        string n = name;
        for (string::size_type i = 0; i < n.size(); ++i) {
            if (n[i] == '/') {
                n[i] = '\\';
            }
        }
        return n;
    }
    return "\\\\.\\pipe\\" + name;
#else
    if (name.find('/') != string::npos) {
        return name;
    }
    // A bare name lives in the system temp directory, like QLocalServer does.
    const char *env = ::getenv("TMPDIR");
    string dir = (env && *env) ? env : "/tmp";
    if (!dir.empty() && dir[dir.size() - 1] == '/') {
        dir.erase(dir.size() - 1);
    }
    return dir + "/" + name + ".sock";
#endif
}

string LocalSocketPrivate::localAddressURI() const
{
#ifdef NG_OS_WIN
    return string("pipe://") + (serverName.empty() ? fullServerName : serverName);
#else
    return string("unix://") + fullServerName;
#endif
}

string LocalSocketPrivate::peerAddressURI() const
{
    return localAddressURI();
}

bool LocalSocketPrivate::setOption(Socket::SocketOption, int)
{
    return false;
}

int LocalSocketPrivate::option(Socket::SocketOption) const
{
    return -1;
}

LocalSocket::LocalSocket()
    : d_ptr(new LocalSocketPrivate(this))
{
}

LocalSocket::LocalSocket(intptr_t socketDescriptor)
    : d_ptr(new LocalSocketPrivate(socketDescriptor, this))
{
}

LocalSocket::LocalSocket(LocalSocketPrivate *d)
    : d_ptr(d)
{
    d_ptr->q_ptr = this;
}

LocalSocket::~LocalSocket()
{
    if (d_ptr->readLock.isLocked() || d_ptr->writeLock.isLocked()) {
        ngWarning() << "socket is deleted while receiving or sending.";
    }
    delete d_ptr;
}

Socket::SocketError LocalSocket::error() const
{
    NG_D(const LocalSocket);
    return d->error;
}

string LocalSocket::errorString() const
{
    NG_D(const LocalSocket);
    return d->errorString;
}

bool LocalSocket::isValid() const
{
    NG_D(const LocalSocket);
    return d->isValid();
}

HostAddress LocalSocket::localAddress() const
{
    return HostAddress();
}

uint16_t LocalSocket::localPort() const
{
    return 0;
}

HostAddress LocalSocket::peerAddress() const
{
    return HostAddress();
}

string LocalSocket::peerName() const
{
    NG_D(const LocalSocket);
    return d->serverName;
}

uint16_t LocalSocket::peerPort() const
{
    return 0;
}

intptr_t LocalSocket::fileno() const
{
    NG_D(const LocalSocket);
    return static_cast<intptr_t>(d->fd);
}

Socket::SocketType LocalSocket::type() const
{
    return Socket::LocalSocket;
}

Socket::SocketState LocalSocket::state() const
{
    NG_D(const LocalSocket);
    return d->state;
}

HostAddress::NetworkLayerProtocol LocalSocket::protocol() const
{
    return HostAddress::UnknownNetworkLayerProtocol;
}

string LocalSocket::localAddressURI() const
{
    NG_D(const LocalSocket);
    return d->localAddressURI();
}

string LocalSocket::peerAddressURI() const
{
    NG_D(const LocalSocket);
    return d->peerAddressURI();
}

string LocalSocket::serverName() const
{
    NG_D(const LocalSocket);
    return d->serverName;
}

string LocalSocket::fullServerName() const
{
    NG_D(const LocalSocket);
    return d->fullServerName;
}

LocalSocket *LocalSocket::accept()
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return nullptr;
    }
    return d->accept();
}

bool LocalSocket::bind(const string &name, Socket::BindMode mode)
{
    NG_D(LocalSocket);
    return d->bind(name, mode);
}

bool LocalSocket::connect(const string &name)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return false;
    }
    return d->connect(name);
}

void LocalSocket::close()
{
    NG_D(LocalSocket);
    d->close();
}

void LocalSocket::abort()
{
    NG_D(LocalSocket);
    d->abort();
}

bool LocalSocket::listen(int backlog)
{
    NG_D(LocalSocket);
    return d->listen(backlog);
}

bool LocalSocket::setOption(Socket::SocketOption option, int value)
{
    NG_D(LocalSocket);
    return d->setOption(option, value);
}

int LocalSocket::option(Socket::SocketOption option) const
{
    NG_D(const LocalSocket);
    return d->option(option);
}

int32_t LocalSocket::peek(char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->peek(data, size);
}

int32_t LocalSocket::peekRaw(char *data, int32_t size)
{
    return peek(data, size);
}

int32_t LocalSocket::recv(char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recv(data, size, false);
}

int32_t LocalSocket::recvall(char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recv(data, size, true);
}

int32_t LocalSocket::send(const char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->send(data, size, false);
}

int32_t LocalSocket::sendall(const char *data, int32_t size)
{
    NG_D(LocalSocket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->send(data, size, true);
}

string LocalSocket::recv(int32_t size)
{
    string buf(static_cast<size_t>(size > 0 ? size : 0), '\0');
    int32_t bytes = recv(&buf[0], size);
    if (bytes <= 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(bytes));
    return buf;
}

string LocalSocket::recvall(int32_t size)
{
    string buf(static_cast<size_t>(size > 0 ? size : 0), '\0');
    int32_t bytes = recvall(&buf[0], size);
    if (bytes <= 0) {
        return string();
    }
    buf.resize(static_cast<size_t>(bytes));
    return buf;
}

int32_t LocalSocket::send(const string &data)
{
    return send(data.data(), static_cast<int32_t>(data.size()));
}

int32_t LocalSocket::sendall(const string &data)
{
    return sendall(data.data(), static_cast<int32_t>(data.size()));
}

LocalSocket *LocalSocket::createConnection(const string &name, Socket::SocketError *error)
{
    unique_ptr<LocalSocket> socket(new LocalSocket());
    if (!socket->connect(name)) {
        if (error) {
            *error = socket->error();
        }
        return nullptr;
    }
    if (error) {
        *error = Socket::NoError;
    }
    return socket.release();
}

LocalSocket *LocalSocket::createServer(const string &name, int backlog)
{
    unique_ptr<LocalSocket> socket(new LocalSocket());
    if (!socket->bind(name)) {
        return nullptr;
    }
    if (backlog != 0) {
        if (!socket->listen(backlog)) {
            return nullptr;
        }
    }
    return socket.release();
}

namespace {

class LocalSocketLikeImpl : public SocketLike
{
public:
    LocalSocketLikeImpl(shared_ptr<LocalSocket> s);
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
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override;
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
    shared_ptr<LocalSocket> s;
};

LocalSocketLikeImpl::LocalSocketLikeImpl(shared_ptr<LocalSocket> s)
    : s(std::move(s))
{
}

Socket::SocketError LocalSocketLikeImpl::error() const
{
    return s->error();
}

string LocalSocketLikeImpl::errorString() const
{
    return s->errorString();
}

bool LocalSocketLikeImpl::isValid() const
{
    return s->isValid();
}

HostAddress LocalSocketLikeImpl::localAddress() const
{
    return s->localAddress();
}

uint16_t LocalSocketLikeImpl::localPort() const
{
    return s->localPort();
}

HostAddress LocalSocketLikeImpl::peerAddress() const
{
    return s->peerAddress();
}

string LocalSocketLikeImpl::peerName() const
{
    return s->peerName();
}

uint16_t LocalSocketLikeImpl::peerPort() const
{
    return s->peerPort();
}

intptr_t LocalSocketLikeImpl::fileno() const
{
    return s->fileno();
}

Socket::SocketType LocalSocketLikeImpl::type() const
{
    return s->type();
}

Socket::SocketState LocalSocketLikeImpl::state() const
{
    return s->state();
}

HostAddress::NetworkLayerProtocol LocalSocketLikeImpl::protocol() const
{
    return s->protocol();
}

string LocalSocketLikeImpl::localAddressURI() const
{
    return s->localAddressURI();
}

string LocalSocketLikeImpl::peerAddressURI() const
{
    return s->peerAddressURI();
}

Socket *LocalSocketLikeImpl::acceptRaw()
{
    return nullptr;
}

shared_ptr<SocketLike> LocalSocketLikeImpl::accept()
{
    return asSocketLike(s->accept());
}

bool LocalSocketLikeImpl::bind(const HostAddress &, uint16_t, Socket::BindMode)
{
    return false;
}

bool LocalSocketLikeImpl::bind(uint16_t, Socket::BindMode)
{
    return false;
}

bool LocalSocketLikeImpl::connect(const HostAddress &, uint16_t)
{
    return false;
}

bool LocalSocketLikeImpl::connect(const string &, uint16_t, shared_ptr<SocketDnsCache>)
{
    // A local socket has no host:port peer; callers must use LocalSocket::connect(name).
    return false;
}

void LocalSocketLikeImpl::close()
{
    s->close();
}

void LocalSocketLikeImpl::abort()
{
    s->abort();
}

bool LocalSocketLikeImpl::listen(int backlog)
{
    return s->listen(backlog);
}

bool LocalSocketLikeImpl::setOption(Socket::SocketOption option, int value)
{
    return s->setOption(option, value);
}

int LocalSocketLikeImpl::option(Socket::SocketOption option) const
{
    return s->option(option);
}

int32_t LocalSocketLikeImpl::peek(char *data, int32_t size)
{
    return s->peek(data, size);
}

int32_t LocalSocketLikeImpl::peekRaw(char *data, int32_t size)
{
    return s->peekRaw(data, size);
}

int32_t LocalSocketLikeImpl::recv(char *data, int32_t size)
{
    return s->recv(data, size);
}

int32_t LocalSocketLikeImpl::recvall(char *data, int32_t size)
{
    return s->recvall(data, size);
}

int32_t LocalSocketLikeImpl::send(const char *data, int32_t size)
{
    return s->send(data, size);
}

int32_t LocalSocketLikeImpl::sendall(const char *data, int32_t size)
{
    return s->sendall(data, size);
}

string LocalSocketLikeImpl::recv(int32_t size)
{
    return s->recv(size);
}

string LocalSocketLikeImpl::recvall(int32_t size)
{
    return s->recvall(size);
}

int32_t LocalSocketLikeImpl::send(const string &data)
{
    return s->send(data);
}

int32_t LocalSocketLikeImpl::sendall(const string &data)
{
    return s->sendall(data);
}

}  // namespace

shared_ptr<SocketLike> asSocketLike(shared_ptr<LocalSocket> s)
{
    if (!s) {
        return shared_ptr<SocketLike>();
    }
    return static_pointer_cast<SocketLike>(make_shared<LocalSocketLikeImpl>(s));
}

shared_ptr<LocalSocket> convertSocketLikeToLocalSocket(shared_ptr<SocketLike> socket)
{
    shared_ptr<LocalSocketLikeImpl> impl = dynamic_pointer_cast<LocalSocketLikeImpl>(socket);
    if (!impl) {
        return shared_ptr<LocalSocket>();
    }
    return impl->s;
}

}  // namespace qtng
