#ifndef QTNG_KADEMLIA_P_H
#define QTNG_KADEMLIA_P_H

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qtng/bencode.h"
#include "qtng/coroutine_utils.h"
#include "qtng/kademlia.h"
#include "qtng/locks.h"
#include "qtng/socket.h"

namespace qtng {

static const int DhtK = 8;
static const int DhtAlpha = 3;
static const int DhtBucketCount = 160;
static const float DhtRpcTimeoutSecs = 3.0f;
static const std::int64_t DhtPeerTtlSecs = 30 * 60;
static const std::int64_t DhtTokenTtlSecs = 10 * 60;

class RoutingTable
{
public:
    explicit RoutingTable(const NodeId &selfId);

    NodeId selfId() const { return m_selfId; }
    void setSelfId(const NodeId &id) { m_selfId = id; }

    // Returns true if inserted or refreshed. If bucket full, *evictCandidate set.
    bool offer(const DhtNodeInfo &node, DhtNodeInfo *evictCandidate = nullptr);
    void replace(const DhtNodeInfo &evict, const DhtNodeInfo &neu);
    void remove(const NodeId &id);

    std::vector<DhtNodeInfo> closest(const NodeId &target, int count) const;
    std::vector<DhtNodeInfo> allNodes() const;
    int size() const;
    void clear();
    void load(const std::vector<DhtNodeInfo> &nodes);
private:
    int bucketFor(const NodeId &id) const;
    NodeId m_selfId;
    // each bucket: MRU at front
    std::vector<std::list<DhtNodeInfo>> m_buckets;
};

struct DhtRpcReply
{
    bool ok;
    Bencode message;
    HostAddress fromAddress;
    std::uint16_t fromPort;

    DhtRpcReply()
        : ok(false)
        , fromPort(0)
    {
    }
};

class DhtNodePrivate
{
public:
    DhtNodePrivate(DhtNode *q, const NodeId &id);
    ~DhtNodePrivate();

    bool open(std::uint16_t bindPort, std::shared_ptr<DhtStore> store, HostAddress::NetworkLayerProtocol proto);
    void close();

    bool bootstrap(const std::vector<DhtEndpoint> &seeds);
    std::vector<DhtNodeInfo> findNode(const NodeId &target);
    std::vector<DhtPeer> getPeers(const NodeId &infoHash);
    bool announcePeer(const NodeId &infoHash, std::uint16_t peerPort, const std::string &token);

    void recvLoop();
    void maintenanceLoop();

    void handlePacket(const std::string &data, const HostAddress &addr, std::uint16_t port);
    void handleQuery(const Bencode &msg, const HostAddress &addr, std::uint16_t port);
    Bencode makeResponse(const std::string &tid, const Bencode &rDict);
    Bencode makeError(const std::string &tid, int code, const std::string &msg);

    DhtRpcReply rpc(const DhtEndpoint &ep, const std::string &query, const Bencode &args);
    bool ping(const DhtEndpoint &ep, NodeId *remoteId = nullptr);
    void heardFrom(const DhtNodeInfo &node, bool allowEvictionPing = true);

    std::string nextTid();
    std::string makeToken(const HostAddress &addr) const;
    bool verifyToken(const HostAddress &addr, const std::string &token) const;

    struct LookupResult {
        std::vector<DhtNodeInfo> nodes;
        std::vector<DhtPeer> peers;
        std::string token;
        DhtEndpoint tokenFrom;
    };
    LookupResult iterativeLookup(const NodeId &target, bool wantPeers);

    void persistNodes();
    void loadLocalPeers();

    DhtNode *q_ptr;
    NodeId preferredId;
    NodeId localId;
    std::string tokenSecret;
    std::shared_ptr<DhtStore> store;
    std::shared_ptr<Socket> socket;
    std::unique_ptr<RoutingTable> table;
    std::unique_ptr<CoroutineGroup> workers;
    std::map<std::string, std::shared_ptr<ValueEvent<DhtRpcReply>>> waiters;
    RLock waitersLock;
    std::uint16_t tidCounter;
    std::uint16_t bindPort;
    bool opened;
    std::string error;
    HostAddress::NetworkLayerProtocol proto;
};

}  // namespace qtng

#endif  // QTNG_KADEMLIA_P_H
