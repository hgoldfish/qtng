#ifndef QTNG_RPC_PEER_H
#define QTNG_RPC_PEER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "qtng/rpc/base.h"
#include "qtng/rpc/value.h"

BEGIN_QTNG_RPC_NAMESPACE

class Rpc;
class PeerPrivate;

// A handle to a remote peer. call() blocks the calling coroutine until the
// response arrives (or the connection is lost). Call it from a coroutine.
class Peer : public std::enable_shared_from_this<Peer>
{
public:
    Peer(const std::string &name, const std::shared_ptr<qtng::DataChannel> &channel, const std::shared_ptr<Rpc> &rpc);
    virtual ~Peer();

public:
    void shutdown();
    void close() { shutdown(); }
    bool isOk() const;       // peer is connected.
    bool isActive() const;   // is making calls.
    std::string name() const;
    void setName(const std::string &name);
    std::string address() const;
    void setAddress(const std::string &address);

    /* call() throws RpcException */
    Value call(const std::string &method, const ValueList &args = ValueList(), const ValueMap &kwargs = ValueMap());
    Value call(const std::string &method, const Value &arg1);
    Value call(const std::string &method, const Value &arg1, const Value &arg2);
    Value call(const std::string &method, const Value &arg1, const Value &arg2, const Value &arg3);
    Value call(const std::string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4);
    Value call(const std::string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
               const Value &arg5);
    Value call(const std::string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
               const Value &arg5, const Value &arg6);
    Value call(const std::string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
               const Value &arg5, const Value &arg6, const Value &arg7);
    Value call(const std::string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
               const Value &arg5, const Value &arg6, const Value &arg7, const Value &arg8);
    Value call(const std::string &method, const Value &arg1, const Value &arg2, const Value &arg3, const Value &arg4,
               const Value &arg5, const Value &arg6, const Value &arg7, const Value &arg8, const Value &arg9);

    std::shared_ptr<qtng::VirtualChannel> makeChannel();
    std::shared_ptr<qtng::VirtualChannel> takeChannel(std::uint32_t channelNumber);

    // opaque per-peer storage (pbook keeps its encryption key/iv here).
    void setProperty(const std::string &key, const Value &v);
    Value property(const std::string &key) const;

    EventDispatcher<Peer *> disconnected;
    EventDispatcher<Peer *>::Connection onDisconnected(std::function<void(Peer *)> cb)
    {
        return disconnected.connect(std::move(cb));
    }

private:
    PeerPrivate * const d_ptr;
    PeerPrivate *d_func() const { return d_ptr; }
    friend class PeerPrivate;
    friend class RpcPrivate;
};

END_QTNG_RPC_NAMESPACE

#endif  // QTNG_RPC_PEER_H
