#ifndef QTNG_KADEMLIA_H
#define QTNG_KADEMLIA_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qtng/hostaddress.h"
#include "qtng/utils/platform.h"

namespace qtng {

// 20-byte SHA-1 node id / infohash used by BitTorrent DHT (BEP-5).
// Distance is bitwise XOR; closer ids share a longer common prefix.
class NodeId
{
public:
    static const int Size = 20;

    NodeId();
    explicit NodeId(const std::string &raw20);

    static NodeId random();
    static NodeId fromBytes(const std::string &raw20);
    static NodeId fromHex(const std::string &hex40);

    bool isValid() const { return m_raw.size() == static_cast<std::size_t>(Size); }
    std::string toBytes() const { return m_raw; }
    std::string toHex() const;

    // XOR distance metric from the Kademlia paper / BEP-5.
    NodeId operator^(const NodeId &other) const;
    // Number of leading equal bits (0..160).
    int commonPrefixLength(const NodeId &other) const;
    // Routing-table bucket index relative to *this*: 0..159, or -1 if equal.
    int bucketIndex(const NodeId &other) const;

    bool operator==(const NodeId &other) const { return m_raw == other.m_raw; }
    bool operator!=(const NodeId &other) const { return m_raw != other.m_raw; }
    bool operator<(const NodeId &other) const { return m_raw < other.m_raw; }
private:
    std::string m_raw;
};

// UDP address of a DHT node (not a torrent peer).
struct DhtEndpoint
{
    DhtEndpoint()
        : m_port(0)
    {
    }
    DhtEndpoint(const HostAddress &addr, std::uint16_t p)
        : m_address(addr)
        , m_port(p)
    {
    }

    bool isValid() const { return !m_address.isNull() && m_port != 0; }

    HostAddress address() const { return m_address; }
    void setAddress(const HostAddress &addr) { m_address = addr; }
    std::uint16_t port() const { return m_port; }
    void setPort(std::uint16_t p) { m_port = p; }
private:
    HostAddress m_address;
    std::uint16_t m_port;
};

// DHT contact: node id plus UDP endpoint (routing-table entry / find_node result).
struct DhtNodeInfo
{
    DhtNodeInfo() { }
    DhtNodeInfo(const NodeId &nid, const DhtEndpoint &ep)
        : m_id(nid)
        , m_endpoint(ep)
    {
    }

    bool isValid() const { return m_id.isValid() && m_endpoint.isValid(); }

    NodeId id() const { return m_id; }
    void setId(const NodeId &nid) { m_id = nid; }
    DhtEndpoint endpoint() const { return m_endpoint; }
    void setEndpoint(const DhtEndpoint &ep) { m_endpoint = ep; }
private:
    NodeId m_id;
    DhtEndpoint m_endpoint;
};

// Torrent peer announced via get_peers / announce_peer (TCP/µTP download port).
struct DhtPeer
{
    DhtPeer()
        : m_port(0)
    {
    }
    DhtPeer(const HostAddress &addr, std::uint16_t p)
        : m_address(addr)
        , m_port(p)
    {
    }

    bool isValid() const { return !m_address.isNull() && m_port != 0; }
    bool operator==(const DhtPeer &other) const
    {
        return m_address == other.m_address && m_port == other.m_port;
    }

    HostAddress address() const { return m_address; }
    void setAddress(const HostAddress &addr) { m_address = addr; }
    std::uint16_t port() const { return m_port; }
    void setPort(std::uint16_t p) { m_port = p; }
private:
    HostAddress m_address;
    std::uint16_t m_port;
};

// BEP-5 compact encodings for "nodes" / "nodes6" / "values" fields.
// IPv4 node = 20-byte id + 4-byte IP + 2-byte port (26); IPv6 node = 38.
// IPv4 peer = 4-byte IP + 2-byte port (6); IPv6 peer = 18.
std::string encodeCompactNodes(const std::vector<DhtNodeInfo> &nodes);
std::string encodeCompactNodes6(const std::vector<DhtNodeInfo> &nodes);
std::vector<DhtNodeInfo> decodeCompactNodes(const std::string &data);
std::vector<DhtNodeInfo> decodeCompactNodes6(const std::string &data);
std::string encodeCompactPeers(const std::vector<DhtPeer> &peers);
std::string encodeCompactPeers6(const std::vector<DhtPeer> &peers);
std::vector<DhtPeer> decodeCompactPeers(const std::string &data);
std::vector<DhtPeer> decodeCompactPeers6(const std::string &data);

// Pluggable persistence for local id, routing contacts, and announced peers.
// DhtNode keeps hot data in memory and calls this on load / save.
class DhtStore
{
public:
    struct StoredPeer {
        StoredPeer() { }

        DhtPeer peer() const { return m_peer; }
        void setPeer(const DhtPeer &p) { m_peer = p; }
        std::int64_t expireUnix() const { return m_expireUnix; }
        void setExpireUnix(std::int64_t t) { m_expireUnix = t; }
    private:
        DhtPeer m_peer;
        std::int64_t m_expireUnix;
    };

    virtual ~DhtStore() { }

    // Local node id and token secret used to mint announce tokens.
    // Returns false if nothing stored yet (first boot).
    virtual bool loadMeta(NodeId *id, std::string *tokenSecret) = 0;
    virtual bool saveMeta(const NodeId &id, const std::string &tokenSecret) = 0;

    virtual std::vector<DhtNodeInfo> loadNodes() = 0;
    virtual bool saveNodes(const std::vector<DhtNodeInfo> &nodes) = 0;

    virtual std::vector<StoredPeer> loadPeers(const NodeId &infoHash) = 0;
    virtual bool putPeer(const NodeId &infoHash, const DhtPeer &peer, std::int64_t expireUnix) = 0;
    virtual bool removeExpiredPeers(std::int64_t nowUnix) = 0;

    virtual std::string errorString() const = 0;
};

// In-process maps; default when DhtNode::open() is called with a null store.
class MemoryDhtStorePrivate;
class MemoryDhtStore : public DhtStore
{
public:
    MemoryDhtStore();
    ~MemoryDhtStore() override;

    bool loadMeta(NodeId *id, std::string *tokenSecret) override;
    bool saveMeta(const NodeId &id, const std::string &tokenSecret) override;
    std::vector<DhtNodeInfo> loadNodes() override;
    bool saveNodes(const std::vector<DhtNodeInfo> &nodes) override;
    std::vector<StoredPeer> loadPeers(const NodeId &infoHash) override;
    bool putPeer(const NodeId &infoHash, const DhtPeer &peer, std::int64_t expireUnix) override;
    bool removeExpiredPeers(std::int64_t nowUnix) override;
    std::string errorString() const override;
private:
    MemoryDhtStorePrivate * const d_ptr;
    NG_DECLARE_PRIVATE(MemoryDhtStore)
    NG_DISABLE_COPY(MemoryDhtStore)
};

// LMDB-backed store (meta / nodes / peers). dirPath is an LMDB path
// (MDB_NOSUBDIR by default — typically a single file, not a directory).
class LmdbDhtStorePrivate;
class LmdbDhtStore : public DhtStore
{
public:
    explicit LmdbDhtStore(const std::string &dirPath);
    ~LmdbDhtStore() override;

    bool isOpen() const;
    bool loadMeta(NodeId *id, std::string *tokenSecret) override;
    bool saveMeta(const NodeId &id, const std::string &tokenSecret) override;
    std::vector<DhtNodeInfo> loadNodes() override;
    bool saveNodes(const std::vector<DhtNodeInfo> &nodes) override;
    std::vector<StoredPeer> loadPeers(const NodeId &infoHash) override;
    bool putPeer(const NodeId &infoHash, const DhtPeer &peer, std::int64_t expireUnix) override;
    bool removeExpiredPeers(std::int64_t nowUnix) override;
    std::string errorString() const override;
private:
    LmdbDhtStorePrivate * const d_ptr;
    NG_DECLARE_PRIVATE(LmdbDhtStore)
    NG_DISABLE_COPY(LmdbDhtStore)
};

// BitTorrent DHT node (BEP-5) over UDP. Coroutine-blocking API:
// open() starts recv / maintenance coroutines; bootstrap / findNode / getPeers
// / announcePeer perform iterative lookups (α=3, k=8).
class DhtNodePrivate;
class DhtNode
{
public:
    // If id is invalid, open() loads from store or generates a random id.
    explicit DhtNode(const NodeId &id = NodeId());
    ~DhtNode();

    // Bind UDP port (0 = ephemeral). Null store → MemoryDhtStore.
    bool open(std::uint16_t bindPort, std::shared_ptr<DhtStore> store = std::shared_ptr<DhtStore>(),
              HostAddress::NetworkLayerProtocol proto = HostAddress::IPv4Protocol);
    void close();
    bool isOpen() const;

    NodeId id() const;
    std::uint16_t localPort() const;
    // Returns the store in use. Without an explicit store in open(), this is the
    // MemoryDhtStore created internally by the node.
    std::shared_ptr<DhtStore> store() const;

    // Ping seeds then iterative find_node(self) to fill the routing table.
    bool bootstrap(const std::vector<DhtEndpoint> &seeds);
    // Iterative find_node; returns up to k closest contacts.
    std::vector<DhtNodeInfo> findNode(const NodeId &target);
    // Iterative get_peers for an infohash (also merges locally stored peers).
    std::vector<DhtPeer> getPeers(const NodeId &infoHash);
    // Announce this peer port for infoHash (fetches tokens via get_peers if needed).
    bool announcePeer(const NodeId &infoHash, std::uint16_t peerPort, const std::string &token = std::string());

    int routingTableSize() const;
    std::string errorString() const;
private:
    DhtNodePrivate * const d_ptr;
    NG_DECLARE_PRIVATE(DhtNode)
    NG_DISABLE_COPY(DhtNode)
};

}  // namespace qtng

#endif  // QTNG_KADEMLIA_H
