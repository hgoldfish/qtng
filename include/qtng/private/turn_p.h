#ifndef QTNG_TURN_P_H
#define QTNG_TURN_P_H

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "qtng/coroutine_utils.h"
#include "qtng/locks.h"
#include "qtng/private/stun_p.h"
#include "qtng/socket.h"
#include "qtng/turn.h"

namespace qtng {

// UDP endpoint of a peer (relay side). Ordering is defined so the type can be
// used as a std::map key.
struct TurnEndpoint
{
    TurnEndpoint()
        : port(0)
    {
    }
    TurnEndpoint(const HostAddress &a, std::uint16_t p)
        : address(a)
        , port(p)
    {
    }
    bool operator<(const TurnEndpoint &other) const
    {
        if (address != other.address) {
            return address.toString() < other.address.toString();
        }
        return port < other.port;
    }
    bool operator==(const TurnEndpoint &other) const
    {
        return address == other.address && port == other.port;
    }
    HostAddress address;
    std::uint16_t port;
};

struct HostAddressLess
{
    bool operator()(const HostAddress &a, const HostAddress &b) const
    {
        return a.toString() < b.toString();
    }
};

// Key for the allocation table: client 5-tuple (client address + port).
struct AllocationKey
{
    AllocationKey()
        : port(0)
    {
    }
    AllocationKey(const HostAddress &a, std::uint16_t p)
        : address(a)
        , port(p)
    {
    }
    bool operator<(const AllocationKey &other) const
    {
        if (address != other.address) {
            return address.toString() < other.address.toString();
        }
        return port < other.port;
    }
    HostAddress address;
    std::uint16_t port;
};

struct IncomingTurnData
{
    IncomingTurnData()
        : port(0)
    {
    }
    HostAddress peer;
    std::uint16_t port;
    std::string data;
};

class TurnClientPrivate
{
public:
    TurnClientPrivate(TurnClient *q, HostAddress::NetworkLayerProtocol proto);
    ~TurnClientPrivate();

    bool open(const HostAddress &server, std::uint16_t port, const std::string &username,
              const std::string &password, float timeoutSecs);
    void close();

    bool refresh(float timeoutSecs);
    bool sendTo(const HostAddress &peer, std::uint16_t port, const std::string &data);
    bool sendIndication(const HostAddress &peer, std::uint16_t port, const std::string &data);
    std::string recvFrom(HostAddress *peer, std::uint16_t *port, float timeoutSecs);

    bool allocate(float timeoutSecs);
    bool ensurePermission(const HostAddress &peer, std::uint16_t port, float timeoutSecs);
    // Binds a channel to the peer. Returns the bound channel number, or 0 on
    // failure; already-bound peers return their existing channel.
    std::uint16_t bindChannel(const HostAddress &peer, std::uint16_t port, float timeoutSecs);
    // Stamp USERNAME/REALM/NONCE/MESSAGE-INTEGRITY onto a request using the
    // credentials learned from the server challenge.
    void applyCredentials(StunMessage *msg) const;
    std::string currentKey() const;
    StunReply rpc(const StunMessage &msg, float timeoutSecs);

    void recvLoop();

    TurnClient *q_ptr;
    HostAddress::NetworkLayerProtocol proto;
    std::shared_ptr<Socket> socket;
    std::unique_ptr<CoroutineGroup> workers;
    std::map<std::string, std::shared_ptr<ValueEvent<StunReply>>> waiters;
    RLock waitersLock;
    Queue<IncomingTurnData> inbox;
    bool opened;
    std::string error;
    int errorCode;

    HostAddress serverAddr;
    std::uint16_t serverPort;
    std::string username;
    std::string password;
    std::string realm;
    std::string nonce;

    HostAddress relayedAddr;
    std::uint16_t relayedPort;
    std::uint32_t lifetimeSecs;

    std::map<TurnEndpoint, std::uint16_t> peerChannels;
    std::map<std::uint16_t, TurnEndpoint> channelPeers;
    std::set<HostAddress, HostAddressLess> permissions;
    std::uint16_t channelCounter;
};

enum TurnAuthResult {
    TurnAuthOk,
    TurnAuthNoCredentials,
    TurnAuthStaleNonce,
    TurnAuthFailed,
};

// One relay allocation: its own relay UDP socket, permissions, channel bindings
// and a forwarding coroutine. Accessed from the listener, relay and maintenance
// coroutines; fields are guarded by `lock`.
class Allocation : public std::enable_shared_from_this<Allocation>
{
public:
    Allocation(const HostAddress &clientAddr, std::uint16_t clientPort, const std::string &key);

    bool hasPermission(const HostAddress &peer) const;
    void addPermission(const HostAddress &peer);
    std::uint16_t channelFor(const HostAddress &peer, std::uint16_t port) const;
    void bindChannel(std::uint16_t channel, const TurnEndpoint &peer);

    HostAddress clientAddr;
    std::uint16_t clientPort;
    std::string key;
    std::shared_ptr<Socket> relaySocket;
    HostAddress relayedAddr;
    std::uint16_t relayedPort;
    std::int64_t expireUnix;
    bool alive;
    std::shared_ptr<Coroutine> relayCoroutine;
    mutable RLock lock;
    std::set<HostAddress, HostAddressLess> permissions;
    std::map<std::uint16_t, TurnEndpoint> channels;
    std::map<TurnEndpoint, std::uint16_t> channelPeers;
};

class TurnServerPrivate
{
public:
    TurnServerPrivate(TurnServer *q, HostAddress::NetworkLayerProtocol proto);
    ~TurnServerPrivate();

    bool open(const HostAddress &addr, std::uint16_t port, const std::string &realm,
              TurnServer::AuthCallback auth);
    void close();

    void recvLoop();
    void maintenanceLoop();
    void relayLoop(const std::shared_ptr<Allocation> &alloc);

    void handleMessage(const std::string &data, const HostAddress &addr, std::uint16_t port);
    void handleAllocate(const StunMessage &msg, const HostAddress &addr, std::uint16_t port);
    void handleRefresh(const StunMessage &msg, const std::shared_ptr<Allocation> &alloc,
                       const HostAddress &addr, std::uint16_t port);
    void handleCreatePermission(const StunMessage &msg, const std::shared_ptr<Allocation> &alloc,
                                const HostAddress &addr, std::uint16_t port);
    void handleChannelBind(const StunMessage &msg, const std::shared_ptr<Allocation> &alloc,
                           const HostAddress &addr, std::uint16_t port);
    void handleSendIndication(const StunMessage &msg, const std::shared_ptr<Allocation> &alloc);
    void handleChannelData(const ChannelDataFrame &frame, const std::shared_ptr<Allocation> &alloc);

    TurnAuthResult authenticate(const StunMessage &msg, std::string *key) const;
    void sendError(const StunMessage &req, int code, const std::string &reason, const HostAddress &addr,
                   std::uint16_t port);
    void sendChallenge(const StunMessage &req, const HostAddress &addr, std::uint16_t port);
    void sendSuccess(const StunMessage &req, const std::string &key, const HostAddress &addr,
                     std::uint16_t port);

    std::shared_ptr<Allocation> findAllocation(const HostAddress &addr, std::uint16_t port);
    void removeAllocation(const std::shared_ptr<Allocation> &alloc);

    TurnServer *q_ptr;
    HostAddress::NetworkLayerProtocol proto;
    std::shared_ptr<Socket> listener;
    std::unique_ptr<CoroutineGroup> workers;
    std::map<AllocationKey, std::shared_ptr<Allocation>> allocations;
    RLock allocLock;
    bool opened;
    std::string error;
    std::string realm;
    std::string nonce;
    std::uint32_t defaultLifetime;
    TurnServer::AuthCallback auth;
};

}  // namespace qtng

#endif  // QTNG_TURN_P_H
