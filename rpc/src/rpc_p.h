#ifndef QTNG_RPC_RPC_P_H
#define QTNG_RPC_RPC_P_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "qtng/rpc/base.h"
#include "qtng/rpc/header.h"
#include "qtng/rpc/rpc.h"
#include "qtng/rpc/value.h"

BEGIN_QTNG_RPC_NAMESPACE

class Peer;

struct PeerAndHeader
{
    std::shared_ptr<Peer> peer;
    Value header;
};

class RpcPrivate
{
public:
    RpcPrivate(Rpc *q);
    ~RpcPrivate();
    bool handleRequest(std::shared_ptr<qtng::SocketLike> connection);
    std::shared_ptr<Peer> connect(std::shared_ptr<qtng::SocketLike> connection, const std::string &peerName);
    std::shared_ptr<Peer> preparePeer(const std::shared_ptr<qtng::DataChannel> &channel, const std::string &peerName,
                                      const std::string &peerAddress);
    void setCurrentPeerAndHeader(const std::shared_ptr<Peer> &peer, const Value &header);
    void deleteCurrentPeerAndHeader();
    std::shared_ptr<Peer> getCurrentPeer();
    Value getRpcHeader();
    void removePeer(const std::string &name, Peer *peer);
    std::shared_ptr<qtng::SocketLike> makeRawSocket(const std::string &peerName, std::string &connectionId);
    std::shared_ptr<qtng::SocketLike> takeRawSocket(const std::string &connectionId);
    void shutdown();
    std::string makeAddress(const std::shared_ptr<qtng::SocketLike> &socket) const;
    void setupChannel(const std::shared_ptr<qtng::SocketChannel> &channel);

public:
    std::string myPeerName;
    std::uint32_t maxPacketSize;
    std::uint32_t payloadSizeHint;
    float keepaliveTimeout;
    std::shared_ptr<HeaderCallback> headerCallback;
    std::shared_ptr<LoggingCallback> loggingCallback;
    std::function<std::shared_ptr<qtng::SocketLike>(const std::string &)> connectionFactory;
    std::map<std::string, std::shared_ptr<Callable>> services;
    std::map<std::string, RpcFunction> functions;
    std::multimap<std::string, std::shared_ptr<Peer>> peers;
    std::map<std::string, std::shared_ptr<qtng::SocketLike>> rawConnections;
    std::map<std::uintptr_t, PeerAndHeader> localStore;

private:
    Rpc * const q_ptr;
    friend class PeerPrivate;
};

END_QTNG_RPC_NAMESPACE

#endif  // QTNG_RPC_RPC_P_H
