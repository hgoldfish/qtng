#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qtng/coroutine_utils.h"
#include "qtng/data_channel.h"
#include "qtng/random.h"

#include "qtng/rpc/registration.h"
#include "qtng/rpc/rpc.h"
#include "qtng/rpc/senddir.h"
#include "qtng/rpc/sendfile.h"
#include "rpc_p.h"

using namespace std;

namespace qtng {
namespace rpc {

RpcPrivate::RpcPrivate(Rpc *q)
    : myPeerName(qtng::randomBytes(16))
    , maxPacketSize(0)
    , payloadSizeHint(0)
    , keepaliveTimeout(-1.0f)
    , q_ptr(q)
{
    detail::registerClass<RpcRemoteException>();
    detail::registerClass<RpcFile>();
    detail::registerClass<RpcDir>();
}

RpcPrivate::~RpcPrivate()
{
    shutdown();
}

void RpcPrivate::shutdown()
{
    for (auto &p : peers) {
        if (p.second) {
            p.second->close();
        }
    }
    peers.clear();
    rawConnections.clear();
}

void RpcPrivate::setupChannel(const shared_ptr<qtng::SocketChannel> &channel)
{
    if (maxPacketSize > 0) {
        channel->setMaxPacketSize(maxPacketSize);
    }
    if (payloadSizeHint > 0) {
        channel->setPayloadSizeHint(payloadSizeHint);
    }
    if (keepaliveTimeout > 0) {
        channel->setKeepaliveTimeout(keepaliveTimeout);
    }
}

string RpcPrivate::makeAddress(const shared_ptr<qtng::SocketLike> &socket) const
{
    if (!socket) {
        return string();
    }
    const qtng::HostAddress &addr = socket->peerAddress();
    string ip = addr.toString();
    if (addr.protocol() == qtng::HostAddress::IPv6Protocol) {
        ip = "[" + ip + "]";
    }
    return ip + ":" + std::to_string(socket->peerPort());
}

bool RpcPrivate::handleRequest(shared_ptr<qtng::SocketLike> connection)
{
    if (!connection) {
        return false;
    }
    connection->setOption(qtng::Socket::LowDelayOption, true);
    const string rpcHeader = connection->recvall(2);
    if (rpcHeader == "\x4e\x67") {
        shared_ptr<qtng::SocketChannel> channel =
                make_shared<qtng::SocketChannel>(connection, qtng::NegativePole);
        setupChannel(channel);
        const string address = makeAddress(connection);
        shared_ptr<Peer> peer = preparePeer(channel, string(), address);
        return static_cast<bool>(peer);
    } else if (rpcHeader == "\x33\x74") {
        const string connectionId = connection->recvall(16);
        if (connectionId.size() != 16) {
            return false;
        }
        if (connection->sendall("\xf3\x97") != 2) {
            return false;
        }
        rawConnections[connectionId] = connection;
        return true;
    }
    return false;
}

shared_ptr<Peer> RpcPrivate::connect(shared_ptr<qtng::SocketLike> connection, const string &peerName)
{
    if (!connection) {
        return shared_ptr<Peer>();
    }
    connection->setOption(qtng::Socket::LowDelayOption, true);
    if (connection->sendall("\x4e\x67") != 2) {
        return shared_ptr<Peer>();
    }
    shared_ptr<qtng::SocketChannel> channel =
            make_shared<qtng::SocketChannel>(connection, qtng::PositivePole);
    setupChannel(channel);
    const string address = makeAddress(connection);
    return preparePeer(channel, peerName, address);
}

shared_ptr<Peer> RpcPrivate::preparePeer(const shared_ptr<qtng::DataChannel> &channel, const string &peerName,
                                         const string &peerAddress)
{
    ValueMap myHeader;
    myHeader["peer_name"] = Value::str(myPeerName);
    myHeader["version"] = Value(static_cast<std::int64_t>(1));
    const string data = Value::pack(Value(std::move(myHeader)));
    channel->sendPacketAsync(data);

    string packet;
    try {
        packet = channel->recvPacket();
    } catch (...) {
        return shared_ptr<Peer>();
    }
    if (packet.empty()) {
        return shared_ptr<Peer>();
    }
    Value v;
    try {
        v = Value::unpack(packet);
    } catch (...) {
        return shared_ptr<Peer>();
    }
    if (v.type() != Value::Type::Map) {
        return shared_ptr<Peer>();
    }
    const ValueMap &itsHeader = v.asMap();
    ValueMap::const_iterator it = itsHeader.find("peer_name");
    if (it == itsHeader.end() || it->second.type() != Value::Type::Str) {
        return shared_ptr<Peer>();
    }
    const string itsPeerName = it->second.asStr();
    if (!peerName.empty() && peerName != itsPeerName) {
        return shared_ptr<Peer>();
    }
    if (myPeerName == itsPeerName) {
        return shared_ptr<Peer>();
    }

    shared_ptr<Rpc> rpcSelf = q_ptr->shared_from_this();
    shared_ptr<Peer> peer = make_shared<Peer>(itsPeerName, channel, rpcSelf);
    if (!peerAddress.empty()) {
        peer->setAddress(peerAddress);
    }
    peers.insert(make_pair(itsPeerName, peer));

    qtng::callInEventLoopAsync([rpcSelf, peer] {
        if (!rpcSelf || !peer) {
            return;
        }
        rpcSelf->newPeer.emit(peer);
    });
    return peer;
}

void RpcPrivate::setCurrentPeerAndHeader(const shared_ptr<Peer> &peer, const Value &header)
{
    const std::uintptr_t coroutineId = qtng::Coroutine::current()->id();
    PeerAndHeader &t = localStore[coroutineId];
    t.header = header;
    t.peer = peer;
}

void RpcPrivate::deleteCurrentPeerAndHeader()
{
    const std::uintptr_t coroutineId = qtng::Coroutine::current()->id();
    localStore.erase(coroutineId);
}

shared_ptr<Peer> RpcPrivate::getCurrentPeer()
{
    const std::uintptr_t coroutineId = qtng::Coroutine::current()->id();
    map<std::uintptr_t, PeerAndHeader>::iterator it = localStore.find(coroutineId);
    return it == localStore.end() ? shared_ptr<Peer>() : it->second.peer;
}

Value RpcPrivate::getRpcHeader()
{
    const std::uintptr_t coroutineId = qtng::Coroutine::current()->id();
    map<std::uintptr_t, PeerAndHeader>::iterator it = localStore.find(coroutineId);
    return it == localStore.end() ? Value() : it->second.header;
}

void RpcPrivate::removePeer(const string &name, Peer *peer)
{
    typedef multimap<string, shared_ptr<Peer>>::iterator Iterator;
    pair<Iterator, Iterator> range = peers.equal_range(name);
    for (Iterator it = range.first; it != range.second; ++it) {
        if (it->second.get() == peer) {
            peers.erase(it);
            return;
        }
    }
}

shared_ptr<qtng::SocketLike> RpcPrivate::makeRawSocket(const string &peerName, string &connectionId)
{
    if (!connectionFactory) {
        return shared_ptr<qtng::SocketLike>();
    }
    shared_ptr<qtng::SocketLike> conn = connectionFactory(peerName);
    if (!conn) {
        return shared_ptr<qtng::SocketLike>();
    }
    conn->setOption(qtng::Socket::LowDelayOption, true);
    connectionId = qtng::randomBytes(16);
    string packet("\x33\x74", 2);
    packet += connectionId;
    if (conn->sendall(packet) != static_cast<int>(packet.size())) {
        connectionId.clear();
        return shared_ptr<qtng::SocketLike>();
    }
    if (conn->recvall(2) != "\xf3\x97") {
        connectionId.clear();
        return shared_ptr<qtng::SocketLike>();
    }
    return conn;
}

shared_ptr<qtng::SocketLike> RpcPrivate::takeRawSocket(const string &connectionId)
{
    map<string, shared_ptr<qtng::SocketLike>>::iterator it = rawConnections.find(connectionId);
    if (it == rawConnections.end()) {
        return shared_ptr<qtng::SocketLike>();
    }
    shared_ptr<qtng::SocketLike> socket = it->second;
    rawConnections.erase(it);
    return socket;
}

// ---------------------------------------------------------------------------
// Rpc
// ---------------------------------------------------------------------------
Rpc::Rpc()
    : d_ptr(new RpcPrivate(this))
{
}

Rpc::~Rpc()
{
    delete d_ptr;
}

std::uint32_t Rpc::maxPacketSize() const
{
    return d_ptr->maxPacketSize;
}

void Rpc::setMaxPacketSize(std::uint32_t maxPacketSize)
{
    d_ptr->maxPacketSize = maxPacketSize;
}

std::uint32_t Rpc::payloadSizeHint() const
{
    return d_ptr->payloadSizeHint;
}

void Rpc::setPayloadSizeHint(std::uint32_t payloadSizeHint)
{
    d_ptr->payloadSizeHint = payloadSizeHint;
}

float Rpc::keepaliveTimeout() const
{
    return d_ptr->keepaliveTimeout;
}

void Rpc::setKeepaliveTimeout(float keepaliveTimeout)
{
    d_ptr->keepaliveTimeout = keepaliveTimeout;
}

string Rpc::myPeerName() const
{
    return d_ptr->myPeerName;
}

shared_ptr<HeaderCallback> Rpc::headerCallback() const
{
    return d_ptr->headerCallback;
}

void Rpc::setHeaderCallback(shared_ptr<HeaderCallback> headerCallback)
{
    d_ptr->headerCallback = std::move(headerCallback);
}

shared_ptr<LoggingCallback> Rpc::loggingCallback() const
{
    return d_ptr->loggingCallback;
}

void Rpc::setLoggingCallback(shared_ptr<LoggingCallback> loggingCallback)
{
    d_ptr->loggingCallback = std::move(loggingCallback);
}

function<shared_ptr<qtng::SocketLike>(const string &)> Rpc::connectionFactory() const
{
    return d_ptr->connectionFactory;
}

void Rpc::setConnectionFactory(function<shared_ptr<qtng::SocketLike>(const string &)> f)
{
    d_ptr->connectionFactory = std::move(f);
}

void Rpc::registerFunction(const string &name, RpcFunction f)
{
    d_ptr->functions[name] = std::move(f);
}

void Rpc::unregisterFunction(const string &name)
{
    d_ptr->functions.erase(name);
}

void Rpc::registerInstanceImpl(const string &name, const shared_ptr<Callable> &instance)
{
    d_ptr->services[name] = instance;
}

void Rpc::unregisterInstance(const string &name)
{
    d_ptr->services.erase(name);
}

bool Rpc::handleRequest(shared_ptr<qtng::SocketLike> connection)
{
    return d_ptr->handleRequest(std::move(connection));
}

shared_ptr<Peer> Rpc::connect(shared_ptr<qtng::SocketLike> connection, const string &peerName)
{
    return d_ptr->connect(std::move(connection), peerName);
}

shared_ptr<Peer> Rpc::get(const string &peerName) const
{
    typedef multimap<string, shared_ptr<Peer>>::const_iterator Iterator;
    pair<Iterator, Iterator> range = d_ptr->peers.equal_range(peerName);
    for (Iterator it = range.first; it != range.second; ++it) {
        if (it->second && it->second->isOk()) {
            return it->second;
        }
    }
    return shared_ptr<Peer>();
}

vector<shared_ptr<Peer>> Rpc::getAllPeers() const
{
    vector<shared_ptr<Peer>> result;
    for (const pair<const string, shared_ptr<Peer>> &p : d_ptr->peers) {
        result.push_back(p.second);
    }
    return result;
}

bool Rpc::isConnected(const string &peerName) const
{
    typedef multimap<string, shared_ptr<Peer>>::const_iterator Iterator;
    pair<Iterator, Iterator> range = d_ptr->peers.equal_range(peerName);
    for (Iterator it = range.first; it != range.second; ++it) {
        if (it->second && it->second->isOk()) {
            return true;
        }
    }
    return false;
}

shared_ptr<Peer> Rpc::getCurrentPeer()
{
    return d_ptr->getCurrentPeer();
}

Value Rpc::getRpcHeader()
{
    return d_ptr->getRpcHeader();
}

shared_ptr<qtng::SocketLike> Rpc::makeRawSocket(const string &peerName, string &connectionId)
{
    return d_ptr->makeRawSocket(peerName, connectionId);
}

shared_ptr<qtng::SocketLike> Rpc::takeRawSocket(const string &connectionId)
{
    return d_ptr->takeRawSocket(connectionId);
}

void Rpc::shutdown()
{
    d_ptr->shutdown();
}

RpcBuilder Rpc::builder()
{
    return RpcBuilder();
}

std::shared_ptr<Rpc> Rpc::create()
{
    return std::make_shared<Rpc>();
}

// ---------------------------------------------------------------------------
// RpcBuilder
// ---------------------------------------------------------------------------
RpcBuilder &RpcBuilder::myPeerName(const string &name)
{
    ensureCreated()->d_func()->myPeerName = name;
    return *this;
}

RpcBuilder &RpcBuilder::maxPacketSize(std::uint32_t n)
{
    ensureCreated()->d_func()->maxPacketSize = n;
    return *this;
}

RpcBuilder &RpcBuilder::payloadSizeHint(std::uint32_t n)
{
    ensureCreated()->d_func()->payloadSizeHint = n;
    return *this;
}

RpcBuilder &RpcBuilder::keepaliveTimeout(float t)
{
    ensureCreated()->d_func()->keepaliveTimeout = t;
    return *this;
}

RpcBuilder &RpcBuilder::headerCallback(shared_ptr<HeaderCallback> cb)
{
    ensureCreated()->d_func()->headerCallback = std::move(cb);
    return *this;
}

RpcBuilder &RpcBuilder::loggingCallback(shared_ptr<LoggingCallback> cb)
{
    ensureCreated()->d_func()->loggingCallback = std::move(cb);
    return *this;
}

RpcBuilder &RpcBuilder::connectionFactory(function<shared_ptr<qtng::SocketLike>(const string &)> f)
{
    ensureCreated()->d_func()->connectionFactory = std::move(f);
    return *this;
}

shared_ptr<Rpc> RpcBuilder::create()
{
    return ensureCreated();
}

std::shared_ptr<Rpc> RpcBuilder::ensureCreated()
{
    if (!rpc) {
        rpc = Rpc::create();
    }
    return rpc;
}

}  // namespace rpc
}  // namespace qtng
