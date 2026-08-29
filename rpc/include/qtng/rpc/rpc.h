#ifndef QTNG_RPC_RPC_H
#define QTNG_RPC_RPC_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qtng/rpc/base.h"
#include "qtng/rpc/header.h"
#include "qtng/rpc/peer.h"
#include "qtng/rpc/value.h"

BEGIN_QTNG_RPC_NAMESPACE

class RpcPrivate;
class RpcBuilder;

// The rpc hub. It does not connect to or listen on anything by itself: the
// application hands it established socketlikes via handleRequest()/connect().
class Rpc : public std::enable_shared_from_this<Rpc>
{
public:
    Rpc();
    virtual ~Rpc();

public:
    std::uint32_t maxPacketSize() const;
    void setMaxPacketSize(std::uint32_t maxPacketSize);
    std::uint32_t payloadSizeHint() const;
    void setPayloadSizeHint(std::uint32_t payloadSizeHint);
    float keepaliveTimeout() const;
    void setKeepaliveTimeout(float keepaliveTimeout);
    std::string myPeerName() const;
    std::shared_ptr<HeaderCallback> headerCallback() const;
    void setHeaderCallback(std::shared_ptr<HeaderCallback> headerCallback);
    std::shared_ptr<LoggingCallback> loggingCallback() const;
    void setLoggingCallback(std::shared_ptr<LoggingCallback> loggingCallback);
    std::function<std::shared_ptr<qtng::SocketLike>(const std::string &)> connectionFactory() const;
    void setConnectionFactory(std::function<std::shared_ptr<qtng::SocketLike>(const std::string &)> f);

    // service registration.
    void registerFunction(const std::string &name, RpcFunction f);
    void unregisterFunction(const std::string &name);
    template<typename T>
    void registerInstance(const std::shared_ptr<T> &instance, const std::string &name);
    void unregisterInstance(const std::string &name);

    // serve an inbound socket (magic bytes are consumed here). Blocking: the
    // rpc handshake runs in the calling coroutine.
    bool handleRequest(std::shared_ptr<qtng::SocketLike> connection);

    // take an established outbound socket and turn it into a peer.
    std::shared_ptr<Peer> connect(std::shared_ptr<qtng::SocketLike> connection, const std::string &peerName = std::string());

    // peer lookup.
    std::shared_ptr<Peer> get(const std::string &peerName) const;
    std::vector<std::shared_ptr<Peer>> getAllPeers() const;
    bool isConnected(const std::string &peerName) const;

    // coroutine-local context of the currently served request.
    std::shared_ptr<Peer> getCurrentPeer();
    Value getRpcHeader();

    // raw sockets (for UseStream::preferRawSocket).
    std::shared_ptr<qtng::SocketLike> makeRawSocket(const std::string &peerName, std::string &connectionId);
    std::shared_ptr<qtng::SocketLike> takeRawSocket(const std::string &connectionId);

    void shutdown();

    // events.
    EventDispatcher<std::shared_ptr<Peer>> newPeer;
    EventDispatcher<std::shared_ptr<Peer>, const std::string &> peerDisconnected;
    EventDispatcher<std::shared_ptr<Peer>>::Connection onNewPeer(std::function<void(std::shared_ptr<Peer>)> cb)
    {
        return newPeer.connect(std::move(cb));
    }

    static RpcBuilder builder();
    static std::shared_ptr<Rpc> create();

private:
    void registerInstanceImpl(const std::string &name, const std::shared_ptr<Callable> &instance);
    RpcPrivate * const d_ptr;
    RpcPrivate *d_func() const { return d_ptr; }
    friend class RpcPrivate;
    friend class PeerPrivate;
    friend class RpcBuilder;
};

class RpcBuilder
{
public:
    RpcBuilder() = default;

    RpcBuilder &myPeerName(const std::string &name);
    RpcBuilder &maxPacketSize(std::uint32_t n);
    RpcBuilder &payloadSizeHint(std::uint32_t n);
    RpcBuilder &keepaliveTimeout(float t);
    RpcBuilder &headerCallback(std::shared_ptr<HeaderCallback> cb);
    RpcBuilder &loggingCallback(std::shared_ptr<LoggingCallback> cb);
    RpcBuilder &connectionFactory(std::function<std::shared_ptr<qtng::SocketLike>(const std::string &)> f);

    std::shared_ptr<Rpc> create();

private:
    std::shared_ptr<Rpc> ensureCreated();
    std::shared_ptr<Rpc> rpc;
};

template<typename T>
void Rpc::registerInstance(const std::shared_ptr<T> &instance, const std::string &name)
{
    static_assert(std::is_base_of<Callable, T>::value,
                  "registerInstance() requires the service class to derive from rpc::Callable "
                  "(e.g. rpc::Service).");
    registerInstanceImpl(name, std::static_pointer_cast<Callable>(instance));
}

END_QTNG_RPC_NAMESPACE

#endif  // QTNG_RPC_RPC_H
