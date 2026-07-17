using namespace std;

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "qtng/coroutine_utils.h"
#include "qtng/private/socket_p.h"
#include "qtng/socket.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/logging.h"

NG_LOGGER("qtng.socket");

namespace qtng {

SocketPrivate::SocketPrivate(HostAddress::NetworkLayerProtocol protocol, Socket::SocketType type, Socket *parent)
    : q_ptr(parent)
    , protocol(protocol)
    , type(type)
    , error(Socket::NoError)
    , state(Socket::UnconnectedState)
    , localPort(0)
    , peerPort(0)
{
#ifdef NG_OS_WIN
    initWinSock();
#endif
    if (!createSocket())
        return;
    if (type == Socket::UdpSocket) {
        setOption(Socket::BroadcastSocketOption, 1);
        setOption(Socket::ReceivePacketInformation, 1);
        setOption(Socket::ReceiveHopLimit, 1);
    } else if (type == Socket::TcpSocket) {
        setTcpKeepalive(true, 10, 2);
    }
}

SocketPrivate::SocketPrivate(intptr_t socketDescriptor, Socket *parent)
    : q_ptr(parent)
    , error(Socket::NoError)
{
#ifdef NG_OS_WIN
    initWinSock();
#endif
    fd = static_cast<int>(socketDescriptor);
    setNonblocking();
    if (!checkState())
        return;
    // FIXME determine the type and state of socket
    protocol = HostAddress::IPv4Protocol;
    type = Socket::TcpSocket;
    state = Socket::ConnectedState;
    fetchConnectionParameters();
    if (type == Socket::UdpSocket) {
        state = Socket::UnconnectedState;
    } else if (type == Socket::TcpSocket) {
        setTcpKeepalive(true, 10, 2);
    }
}

SocketPrivate::~SocketPrivate()
{
#ifdef NG_OS_WIN
    freeWinSock();
#endif
}

bool SocketPrivate::bind(uint16_t port, Socket::BindMode mode)
{
    return bind(HostAddress(HostAddress::Any), port, mode);
}

void SocketPrivate::setError(Socket::SocketError error, const string &errorString)
{
    this->error = error;
    this->errorString = errorString;
}

void SocketPrivate::setError(Socket::SocketError error, ErrorString errorString)
{
    this->error = error;
    string socketErrorString;
    switch (errorString) {
    case NonBlockingInitFailedErrorString:
        socketErrorString = "Unable to initialize non-blocking socket";
        break;
    case BroadcastingInitFailedErrorString:
        socketErrorString = "Unable to initialize broadcast socket";
        break;
    // should not happen anymore
    case NoIpV6ErrorString:
        socketErrorString = "Attempt to use IPv6 socket on a platform with no IPv6 support";
        break;
    case RemoteHostClosedErrorString:
        socketErrorString = "The remote host closed the connection";
        break;
    case TimeOutErrorString:
        socketErrorString = "Network operation timed out";
        break;
    case ResourceErrorString:
        socketErrorString = "Out of resources";
        break;
    case OperationUnsupportedErrorString:
        socketErrorString = "Unsupported socket operation";
        break;
    case ProtocolUnsupportedErrorString:
        socketErrorString = "Protocol type not supported";
        break;
    case InvalidSocketErrorString:
        socketErrorString = "Invalid socket descriptor";
        break;
    case HostUnreachableErrorString:
        socketErrorString = "Host unreachable";
        break;
    case NetworkUnreachableErrorString:
        socketErrorString = "Network unreachable";
        break;
    case AccessErrorString:
        socketErrorString = "Permission denied";
        break;
    case ConnectionTimeOutErrorString:
        socketErrorString = "Connection timed out";
        break;
    case ConnectionRefusedErrorString:
        socketErrorString = "Connection refused";
        break;
    case AddressInuseErrorString:
        socketErrorString = "The bound address is already in use";
        break;
    case AddressNotAvailableErrorString:
        socketErrorString = "The address is not available";
        break;
    case AddressProtectedErrorString:
        socketErrorString = "The address is protected";
        break;
    case DatagramTooLargeErrorString:
        socketErrorString = "Datagram was too large to send";
        break;
    case SendDatagramErrorString:
        socketErrorString = "Unable to send a message";
        break;
    case ReceiveDatagramErrorString:
        socketErrorString = "Unable to receive a message";
        break;
    case WriteErrorString:
        socketErrorString = "Unable to write";
        break;
    case ReadErrorString:
        socketErrorString = "Network error";
        break;
    case PortInuseErrorString:
        socketErrorString = "Another socket is already listening on the same port";
        break;
    case NotSocketErrorString:
        socketErrorString = "Operation on non-socket";
        break;
    case InvalidProxyTypeString:
        socketErrorString = "The proxy type is invalid for this operation";
        break;
    case TemporaryErrorString:
        socketErrorString = "Temporary error";
        break;
    case NetworkDroppedConnectionErrorString:
        socketErrorString = "Network dropped connection on reset";
        break;
    case ConnectionResetErrorString:
        socketErrorString = "Connection reset by peer";
        break;
    case UnknownSocketErrorString:
        socketErrorString = "Unknown error";
        break;
    case OutOfMemoryErrorString:
        socketErrorString = "Out of memeory.";
        break;
    }
    this->errorString = socketErrorString;
}

string SocketPrivate::getErrorString() const
{
    return errorString;
}

bool SocketPrivate::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    if (state != Socket::UnconnectedState && state != Socket::BoundState) {
        return false;
    }
    Socket::SocketState oldState = state;
    state = Socket::HostLookupState;
    vector<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.push_back(t);
    } else {
        if (!dnsCache) {
            addresses = Socket::resolve(hostName);
        } else {
            addresses = dnsCache->resolve(hostName);
        }
    }

    if (addresses.empty()) {
        state = oldState;
        setError(Socket::HostNotFoundError, "Host not found.");
        return false;
    }
    bool done = false;
    for (int i = 0; i < addresses.size(); ++i) {
        const HostAddress &addr = addresses[i];
        if (protocol == HostAddress::IPv4Protocol && addr.protocol() == HostAddress::IPv6Protocol) {
            continue;
        }
        if (protocol == HostAddress::IPv6Protocol && addr.protocol() == HostAddress::IPv4Protocol) {
            continue;
        }
        state = oldState;
        done = connect(addr, port);
        if (done)
            return true;
    }
    if (error == Socket::NoError) {  // and done must be false!
        setError(Socket::UnsupportedSocketOperationError,
                 "No host with matching protocol found.");
    }
    state = oldState;
    return false;
}

Socket::Socket(HostAddress::NetworkLayerProtocol protocol, SocketType type)
    : d_ptr(new SocketPrivate(protocol, type, this))
{
}

Socket::Socket(intptr_t socketDescriptor)
    : d_ptr(new SocketPrivate(socketDescriptor, this))
{
}

Socket::~Socket()
{
    NG_D(Socket);
    d->abort();
    if (d->readLock.isLocked() || d->writeLock.isLocked()) {
        ngWarning() << "socket is deleted while receiving or sending.";
    }
    delete d_ptr;
}

Socket::SocketError Socket::error() const
{
    NG_D(const Socket);
    return d->error;
}

string Socket::errorString() const
{
    NG_D(const Socket);
    return d->getErrorString();
}

bool Socket::isValid() const
{
    NG_D(const Socket);
    return d->isValid();
}

HostAddress Socket::localAddress() const
{
    NG_D(const Socket);
    return d->localAddress;
}

uint16_t Socket::localPort() const
{
    NG_D(const Socket);
    return d->localPort;
}

HostAddress Socket::peerAddress() const
{
    NG_D(const Socket);
    return d->peerAddress;
}

string Socket::peerName() const
{
    return string();
}

uint16_t Socket::peerPort() const
{
    NG_D(const Socket);
    return d->peerPort;
}

intptr_t Socket::fileno() const
{
    NG_D(const Socket);
    return d->fd;
}

Socket::SocketType Socket::type() const
{
    NG_D(const Socket);
    return d->type;
}

Socket::SocketState Socket::state() const
{
    NG_D(const Socket);
    return d->state;
}

HostAddress::NetworkLayerProtocol Socket::protocol() const
{
    NG_D(const Socket);
    return d->protocol;
}

string Socket::localAddressURI() const
{
    NG_D(const Socket);
    const char *scheme = d->type == Socket::TcpSocket ? "tcp" : "udp";
    char buf[256];
    if (d->localAddress.protocol() == HostAddress::IPv6Protocol) {
        snprintf(buf, sizeof(buf), "%s://[%s]:%u", scheme, d->localAddress.toString().c_str(), d->localPort);
    } else {
        snprintf(buf, sizeof(buf), "%s://%s:%u", scheme, d->localAddress.toString().c_str(), d->localPort);
    }
    return buf;
}

string Socket::peerAddressURI() const
{
    NG_D(const Socket);
    const char *scheme = d->type == Socket::TcpSocket ? "tcp" : "udp";
    char buf[256];
    if (d->peerAddress.protocol() == HostAddress::IPv6Protocol) {
        snprintf(buf, sizeof(buf), "%s://[%s]:%u", scheme, d->peerAddress.toString().c_str(), d->peerPort);
    } else {
        snprintf(buf, sizeof(buf), "%s://%s:%u", scheme, d->peerAddress.toString().c_str(), d->peerPort);
    }
    return buf;
}

Socket *Socket::accept()
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return nullptr;
    }
    return d->accept();
}

bool Socket::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    NG_D(Socket);
    return d->bind(address, port, mode);
}

bool Socket::bind(uint16_t port, Socket::BindMode mode)
{
    NG_D(Socket);
    return d->bind(port, mode);
}

bool Socket::connect(const HostAddress &host, uint16_t port)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return false;
    }
    return d->connect(host, port);
}

bool Socket::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return false;
    }
    return d->connect(hostName, port, dnsCache);
}

void Socket::close()
{
    NG_D(Socket);
    d->close();
    if (d->readLock.isLocked()) {
        d->readLock.tryAcquire();
        d->readLock.release();
    }
    if (d->writeLock.isLocked()) {
        d->writeLock.tryAcquire();
        d->writeLock.release();
    }
}

void Socket::abort()
{
    NG_D(Socket);
    d->abort();
    if (d->readLock.isLocked()) {
        d->readLock.release();
    }
    if (d->writeLock.isLocked()) {
        d->writeLock.release();
    }
}

bool Socket::listen(int backlog)
{
    NG_D(Socket);
    return d->listen(backlog);
}

bool Socket::setTcpKeepalive(bool keepalve, int keepaliveTimeoutSesc, int keepaliveIntervalSesc)
{
    NG_D(Socket);
    return d->setTcpKeepalive(keepalve, keepaliveTimeoutSesc, keepaliveIntervalSesc);
}

bool Socket::setOption(Socket::SocketOption option, int value)
{
    NG_D(Socket);
    return d->setOption(option, value);
}

int Socket::option(Socket::SocketOption option) const
{
    NG_D(const Socket);
    return d->option(option);
}

bool Socket::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    NG_D(Socket);
    return d->joinMulticastGroup(groupAddress, iface);
}

bool Socket::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    NG_D(Socket);
    return d->leaveMulticastGroup(groupAddress, iface);
}

NetworkInterface Socket::multicastInterface() const
{
    NG_D(const Socket);
    return d->multicastInterface();
}

bool Socket::setMulticastInterface(const NetworkInterface &iface)
{
    NG_D(Socket);
    return d->setMulticastInterface(iface);
}

int32_t Socket::peek(char *data, int32_t size)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->peek(data, size);
}

int32_t Socket::recv(char *data, int32_t size)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recv(data, size, false);
}

int32_t Socket::recvall(char *data, int32_t size)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recv(data, size, true);
}

int32_t Socket::send(const char *data, int32_t size)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    int32_t bytesSent = d->send(data, size, false);
    if (bytesSent == 0 && !d->checkState()) {
        return -1;
    } else {
        return bytesSent;
    }
}

int32_t Socket::sendall(const char *data, int32_t size)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->send(data, size, true);
}

int32_t Socket::recvfrom(char *data, int32_t size, HostAddress *addr, uint16_t *port)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->recvfrom(data, size, addr, port);
}

int32_t Socket::sendto(const char *data, int32_t size, const HostAddress &addr, uint16_t port)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->sendto(data, size, addr, port);
}

string Socket::recv(int32_t size)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return string();
    }
    string bs(size, '\0');
    int32_t bytes = d->recv(&bs[0], static_cast<int32_t>(bs.size()), false);
    if (bytes > 0) {
        bs.resize(static_cast<int>(bytes));
        return bs;
    }
    return string();
}

string Socket::recvall(int32_t size)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return string();
    }
    string bs(size, '\0');
    int32_t bytes = d->recv(&bs[0], static_cast<int32_t>(bs.size()), true);
    if (bytes > 0) {
        bs.resize(static_cast<int>(bytes));
        return bs;
    }
    return string();
}

int32_t Socket::send(const string &data)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    int32_t bytesSent = d->send(data.data(), data.size(), false);
    if (bytesSent == 0 && !d->checkState()) {
        return -1;
    } else {
        return bytesSent;
    }
}

int32_t Socket::sendall(const string &data)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->send(data.data(), data.size(), true);
}

string Socket::recvfrom(int32_t size, HostAddress *addr, uint16_t *port)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->readLock);
    if (!lock.isSuccess()) {
        return string();
    }
    string bs(size, '\0');
    int32_t bytes = d->recvfrom(&bs[0], size, addr, port);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return string();
}

int32_t Socket::sendto(const string &data, const HostAddress &addr, uint16_t port)
{
    NG_D(Socket);
    ScopedLock<Lock> lock(d->writeLock);
    if (!lock.isSuccess()) {
        return -1;
    }
    return d->sendto(data.data(), data.size(), addr, port);
}

vector<HostAddress> Socket::resolve(const string &hostName)
{
    HostAddress tmp;
    if (tmp.setAddress(hostName)) {
        vector<HostAddress> result;
        result.push_back(tmp);
        return result;
    }

    function<vector<HostAddress>()> task = [hostName]() {
        vector<HostAddress> addr = HostAddress::getHostAddressByName(hostName);
        return addr;
    };

    vector<HostAddress> addr = callInThread<vector<HostAddress>>(task);
    return addr;
}

Socket *Socket::createConnection(const HostAddress &host, uint16_t port, Socket::SocketError *error, int allowProtocol)
{
    return qtng::createConnection<Socket>(host, port, error, allowProtocol, MakeSocketType<Socket>);
}

Socket *Socket::createConnection(const string &hostName, uint16_t port, Socket::SocketError *error,
                                 shared_ptr<SocketDnsCache> dnsCache, int allowProtocol)
{
    return qtng::createConnection<Socket>(hostName, port, error, dnsCache, allowProtocol,
                                                           MakeSocketType<Socket>);
}

Socket *Socket::createServer(const HostAddress &host, uint16_t port, int backlog)
{
    return qtng::createServer<Socket>(host, port, backlog, MakeSocketType<Socket>);
}

class PollPrivate
{
public:
    PollPrivate();
    ~PollPrivate();
public:
    void add(shared_ptr<Socket> socket, EventLoopCoroutine::EventType event);
    void remove(shared_ptr<Socket> socket);
    shared_ptr<Socket> wait(float secs = 0);
private:
    map<shared_ptr<Socket>, int> watchers;
    shared_ptr<unordered_set<shared_ptr<Socket>>> events;
    shared_ptr<Event> done;
};

class PollFunctor : public Functor
{
public:
    PollFunctor(shared_ptr<unordered_set<shared_ptr<Socket>>> events, shared_ptr<Event> done,
                shared_ptr<Socket> socket);
    virtual bool operator()();
    shared_ptr<unordered_set<shared_ptr<Socket>>> events;
    shared_ptr<Event> done;
    weak_ptr<Socket> socket;
};

PollFunctor::PollFunctor(shared_ptr<unordered_set<shared_ptr<Socket>>> events, shared_ptr<Event> done,
                         shared_ptr<Socket> socket)
    : events(events)
    , done(done)
    , socket(socket)
{
}

bool PollFunctor::operator()()
{
    if (auto s = socket.lock()) {
        events->insert(s);
        done->set();
    }
    return true;
}

PollPrivate::PollPrivate()
    : events(new unordered_set<shared_ptr<Socket>>())
    , done(new Event())
{
}

PollPrivate::~PollPrivate()
{
    for (const auto &entry : watchers) {
        EventLoopCoroutine::get()->removeWatcher(entry.second);
    }
}

void PollPrivate::add(shared_ptr<Socket> socket, EventLoopCoroutine::EventType event)
{
    if (watchers.find(socket) != watchers.end()) {
        remove(socket);
    }
    PollFunctor *callback = new PollFunctor(events, done, socket);
    int watcherId = EventLoopCoroutine::get()->createWatcher(event, socket->fileno(), callback);
    EventLoopCoroutine::get()->startWatcher(watcherId);
    watchers[socket] = watcherId;
}

void PollPrivate::remove(shared_ptr<Socket> socket)
{
    auto it = watchers.find(socket);
    if (it == watchers.end()) {
        return;
    }
    EventLoopCoroutine::get()->removeWatcher(it->second);
    watchers.erase(it);
}

shared_ptr<Socket> PollPrivate::wait(float secs)
{
    if (!events->empty()) {
        auto it = events->begin();
        shared_ptr<Socket> socket = *it;
        events->erase(it);
        return socket;
    }
    done->clear();
    if (secs != 0.0f) {
        try {
            Timeout timeout(secs);
            (void)(timeout);
            done->tryWait();
        } catch (TimeoutException &) {
            return shared_ptr<Socket>();
        }
    } else {
        done->tryWait();
    }

    if (!events->empty()) {
        auto it = events->begin();
        shared_ptr<Socket> socket = *it;
        events->erase(it);
        return socket;
    } else {
        return shared_ptr<Socket>();
    }
}

Poll::Poll()
    : d_ptr(new PollPrivate())
{
}

Poll::~Poll()
{
    delete d_ptr;
}

void Poll::add(shared_ptr<Socket> socket, Poll::EventType event)
{
    NG_D(Poll);
    d->add(socket, static_cast<EventLoopCoroutine::EventType>(event));
}

void Poll::remove(shared_ptr<Socket> socket)
{
    NG_D(Poll);
    d->remove(socket);
}

shared_ptr<Socket> Poll::wait(float secs)
{
    NG_D(Poll);
    return d->wait(secs);
}

struct SocketDnsCacheItem
{
    vector<HostAddress> addresses;
    uint64_t firstSeen;
};

class SocketDnsCachePrivate
{
public:
    SocketDnsCachePrivate()
        : timeToLive(1000 * 60 * 5)
        , maxSize(1024)
    {
    }
    uint64_t timeToLive;
    size_t maxSize;
    map<string, SocketDnsCacheItem> cache;
};

SocketDnsCache::SocketDnsCache()
    : d_ptr(new SocketDnsCachePrivate())
{
}

SocketDnsCache::~SocketDnsCache()
{
    delete d_ptr;
}

vector<HostAddress> SocketDnsCache::resolve(const string &hostName)
{
    NG_D(SocketDnsCache);
    uint64_t now = static_cast<uint64_t>(utils::DateTime::currentDateTimeUtc().toMSecsSinceEpoch());
    auto it = d->cache.find(hostName);
    if (it != d->cache.end()) {
        if (now > it->second.firstSeen && (now - it->second.firstSeen < d->timeToLive)) {
            return it->second.addresses;
        }
    }
    const vector<HostAddress> &addresses = Socket::resolve(hostName);
    if (addresses.empty()) {
        return vector<HostAddress>();
    }
    if (d->cache.size() >= d->maxSize && !d->cache.empty()) {
        d->cache.erase(d->cache.begin());
    }
    SocketDnsCacheItem item;
    item.firstSeen = now;
    item.addresses = addresses;
    d->cache[hostName] = item;
    return addresses;
}

bool SocketDnsCache::hasHost(const string &hostName) const
{
    NG_D(const SocketDnsCache);
    return d->cache.find(hostName) != d->cache.end();
}

void SocketDnsCache::addHost(const string &hostName, const vector<HostAddress> &addresses)
{
    NG_D(SocketDnsCache);
    SocketDnsCacheItem item;
    item.firstSeen = static_cast<uint64_t>(utils::DateTime::currentDateTimeUtc().toMSecsSinceEpoch());
    item.addresses = addresses;
    d->cache[hostName] = item;
}

void SocketDnsCache::addHost(const string &hostName, const HostAddress &addr)
{
    NG_D(SocketDnsCache);
    SocketDnsCacheItem item;
    item.firstSeen = static_cast<uint64_t>(utils::DateTime::currentDateTimeUtc().toMSecsSinceEpoch());
    item.addresses.push_back(addr);
    d->cache[hostName] = item;
}

uint64_t SocketDnsCache::timeToLive() const
{
    NG_D(const SocketDnsCache);
    return d->timeToLive;
}

void SocketDnsCache::setTimeToLive(uint64_t msecs)
{
    NG_D(SocketDnsCache);
    d->timeToLive = msecs;
}

}  // namespace qtng
