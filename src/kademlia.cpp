#include "qtng/private/kademlia_p.h"

#include <algorithm>
#include <cstring>

#include "qtng/eventloop.h"
#include "qtng/lmdb.h"
#include "qtng/md.h"
#include "qtng/random.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

namespace {

int hexNibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

const Bencode &dictGet(const Bencode &v, const string &key)
{
    static const Bencode invalid;
    const map<string, Bencode> &m = v.toMap();
    map<string, Bencode>::const_iterator it = m.find(key);
    return it != m.end() ? it->second : invalid;
}

bool dictHas(const Bencode &v, const string &key)
{
    const map<string, Bencode> &m = v.toMap();
    return m.find(key) != m.end();
}

string ipv4Bytes(const HostAddress &addr)
{
    bool ok = false;
    uint32_t ip = addr.toIPv4Address(&ok);
    if (!ok) {
        return string();
    }
    uint32_t be = ngToBigEndian(ip);
    return string(reinterpret_cast<const char *>(&be), 4);
}

string ipv6Bytes(const HostAddress &addr)
{
    IPv6Address a6 = addr.toIPv6Address();
    return string(reinterpret_cast<const char *>(a6.c), 16);
}

string portBytes(uint16_t port)
{
    uint16_t be = ngToBigEndian(port);
    return string(reinterpret_cast<const char *>(&be), 2);
}

bool readIpv4(const char *p, HostAddress *addr, uint16_t *port)
{
    uint32_t be = 0;
    memcpy(&be, p, 4);
    addr->setAddress(ngFromBigEndian<uint32_t>(&be));
    uint16_t pbe = 0;
    memcpy(&pbe, p + 4, 2);
    *port = ngFromBigEndian<uint16_t>(&pbe);
    return true;
}

bool readIpv6(const char *p, HostAddress *addr, uint16_t *port)
{
    addr->setAddress(reinterpret_cast<const uint8_t *>(p));
    uint16_t pbe = 0;
    memcpy(&pbe, p + 16, 2);
    *port = ngFromBigEndian<uint16_t>(&pbe);
    return true;
}

int64_t nowUnix()
{
    return utils::DateTime::currentDateTimeUtc().toSecsSinceEpoch();
}

string packExpire(int64_t expireUnix)
{
    int64_t be = ngToBigEndian(expireUnix);
    return string(reinterpret_cast<const char *>(&be), sizeof(be));
}

int64_t unpackExpire(const string &v)
{
    if (v.size() < sizeof(int64_t)) {
        return 0;
    }
    return ngFromBigEndian<int64_t>(v.data());
}

struct DistanceLess {
    NodeId target;
    bool operator()(const DhtNodeInfo &a, const DhtNodeInfo &b) const
    {
        return (a.id ^ target) < (b.id ^ target);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// NodeId
// ---------------------------------------------------------------------------

NodeId::NodeId() { }

NodeId::NodeId(const string &raw20)
    : m_raw(raw20.size() == static_cast<size_t>(Size) ? raw20 : string())
{
}

NodeId NodeId::random()
{
    return NodeId(randomBytes(Size));
}

NodeId NodeId::fromBytes(const string &raw20)
{
    return NodeId(raw20);
}

NodeId NodeId::fromHex(const string &hex40)
{
    if (hex40.size() != 40) {
        return NodeId();
    }
    string raw;
    raw.resize(Size);
    for (int i = 0; i < Size; ++i) {
        int hi = hexNibble(hex40[static_cast<size_t>(i * 2)]);
        int lo = hexNibble(hex40[static_cast<size_t>(i * 2 + 1)]);
        if (hi < 0 || lo < 0) {
            return NodeId();
        }
        raw[static_cast<size_t>(i)] = static_cast<char>((hi << 4) | lo);
    }
    return NodeId(raw);
}

string NodeId::toHex() const
{
    return utils::bytesToHex(m_raw);
}

NodeId NodeId::operator^(const NodeId &other) const
{
    if (!isValid() || !other.isValid()) {
        return NodeId();
    }
    string out;
    out.resize(Size);
    for (int i = 0; i < Size; ++i) {
        out[static_cast<size_t>(i)] =
                static_cast<char>(static_cast<unsigned char>(m_raw[static_cast<size_t>(i)])
                                  ^ static_cast<unsigned char>(other.m_raw[static_cast<size_t>(i)]));
    }
    return NodeId(out);
}

int NodeId::commonPrefixLength(const NodeId &other) const
{
    if (!isValid() || !other.isValid()) {
        return 0;
    }
    int bits = 0;
    for (int i = 0; i < Size; ++i) {
        unsigned char x = static_cast<unsigned char>(m_raw[static_cast<size_t>(i)])
                ^ static_cast<unsigned char>(other.m_raw[static_cast<size_t>(i)]);
        if (x == 0) {
            bits += 8;
            continue;
        }
        for (int b = 7; b >= 0; --b) {
            if (x & (1u << b)) {
                return bits;
            }
            ++bits;
        }
    }
    return bits;
}

int NodeId::bucketIndex(const NodeId &other) const
{
    int cpl = commonPrefixLength(other);
    if (cpl >= DhtBucketCount) {
        return -1;
    }
    return DhtBucketCount - 1 - cpl;
}

// ---------------------------------------------------------------------------
// Compact encodings
// ---------------------------------------------------------------------------

string encodeCompactNodes(const vector<DhtNodeInfo> &nodes)
{
    string out;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].isValid() || nodes[i].endpoint.address.protocol() != HostAddress::IPv4Protocol) {
            continue;
        }
        string ip = ipv4Bytes(nodes[i].endpoint.address);
        if (ip.empty()) {
            continue;
        }
        out.append(nodes[i].id.toBytes());
        out.append(ip);
        out.append(portBytes(nodes[i].endpoint.port));
    }
    return out;
}

string encodeCompactNodes6(const vector<DhtNodeInfo> &nodes)
{
    string out;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].isValid() || nodes[i].endpoint.address.protocol() != HostAddress::IPv6Protocol) {
            continue;
        }
        out.append(nodes[i].id.toBytes());
        out.append(ipv6Bytes(nodes[i].endpoint.address));
        out.append(portBytes(nodes[i].endpoint.port));
    }
    return out;
}

vector<DhtNodeInfo> decodeCompactNodes(const string &data)
{
    vector<DhtNodeInfo> out;
    if (data.size() % 26 != 0) {
        return out;
    }
    for (size_t i = 0; i + 26 <= data.size(); i += 26) {
        DhtNodeInfo n;
        n.id = NodeId::fromBytes(data.substr(i, 20));
        HostAddress addr;
        uint16_t port = 0;
        readIpv4(data.data() + i + 20, &addr, &port);
        n.endpoint = DhtEndpoint(addr, port);
        if (n.isValid()) {
            out.push_back(n);
        }
    }
    return out;
}

vector<DhtNodeInfo> decodeCompactNodes6(const string &data)
{
    vector<DhtNodeInfo> out;
    if (data.size() % 38 != 0) {
        return out;
    }
    for (size_t i = 0; i + 38 <= data.size(); i += 38) {
        DhtNodeInfo n;
        n.id = NodeId::fromBytes(data.substr(i, 20));
        HostAddress addr;
        uint16_t port = 0;
        readIpv6(data.data() + i + 20, &addr, &port);
        n.endpoint = DhtEndpoint(addr, port);
        if (n.isValid()) {
            out.push_back(n);
        }
    }
    return out;
}

string encodeCompactPeers(const vector<DhtPeer> &peers)
{
    string out;
    for (size_t i = 0; i < peers.size(); ++i) {
        if (!peers[i].isValid() || peers[i].address.protocol() != HostAddress::IPv4Protocol) {
            continue;
        }
        string ip = ipv4Bytes(peers[i].address);
        if (ip.empty()) {
            continue;
        }
        out.append(ip);
        out.append(portBytes(peers[i].port));
    }
    return out;
}

string encodeCompactPeers6(const vector<DhtPeer> &peers)
{
    string out;
    for (size_t i = 0; i < peers.size(); ++i) {
        if (!peers[i].isValid() || peers[i].address.protocol() != HostAddress::IPv6Protocol) {
            continue;
        }
        out.append(ipv6Bytes(peers[i].address));
        out.append(portBytes(peers[i].port));
    }
    return out;
}

vector<DhtPeer> decodeCompactPeers(const string &data)
{
    vector<DhtPeer> out;
    if (data.size() % 6 != 0) {
        return out;
    }
    for (size_t i = 0; i + 6 <= data.size(); i += 6) {
        DhtPeer p;
        readIpv4(data.data() + i, &p.address, &p.port);
        if (p.isValid()) {
            out.push_back(p);
        }
    }
    return out;
}

vector<DhtPeer> decodeCompactPeers6(const string &data)
{
    vector<DhtPeer> out;
    if (data.size() % 18 != 0) {
        return out;
    }
    for (size_t i = 0; i + 18 <= data.size(); i += 18) {
        DhtPeer p;
        readIpv6(data.data() + i, &p.address, &p.port);
        if (p.isValid()) {
            out.push_back(p);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// RoutingTable
// ---------------------------------------------------------------------------

RoutingTable::RoutingTable(const NodeId &selfId)
    : m_selfId(selfId)
    , m_buckets(static_cast<size_t>(DhtBucketCount))
{
}

int RoutingTable::bucketFor(const NodeId &id) const
{
    int idx = m_selfId.bucketIndex(id);
    if (idx < 0) {
        return -1;
    }
    return idx;
}

bool RoutingTable::offer(const DhtNodeInfo &node, DhtNodeInfo *evictCandidate)
{
    if (!node.isValid() || !m_selfId.isValid() || node.id == m_selfId) {
        return false;
    }
    int bi = bucketFor(node.id);
    if (bi < 0) {
        return false;
    }
    list<DhtNodeInfo> &bucket = m_buckets[static_cast<size_t>(bi)];
    for (list<DhtNodeInfo>::iterator it = bucket.begin(); it != bucket.end(); ++it) {
        if (it->id == node.id) {
            DhtNodeInfo updated = node;
            bucket.erase(it);
            bucket.push_front(updated);
            return true;
        }
    }
    if (static_cast<int>(bucket.size()) < DhtK) {
        bucket.push_front(node);
        return true;
    }
    if (evictCandidate) {
        *evictCandidate = bucket.back();
    }
    return false;
}

void RoutingTable::replace(const DhtNodeInfo &evict, const DhtNodeInfo &neu)
{
    int bi = bucketFor(neu.id);
    if (bi < 0) {
        return;
    }
    list<DhtNodeInfo> &bucket = m_buckets[static_cast<size_t>(bi)];
    for (list<DhtNodeInfo>::iterator it = bucket.begin(); it != bucket.end(); ++it) {
        if (it->id == evict.id) {
            bucket.erase(it);
            break;
        }
    }
    offer(neu, nullptr);
}

void RoutingTable::remove(const NodeId &id)
{
    int bi = bucketFor(id);
    if (bi < 0) {
        return;
    }
    list<DhtNodeInfo> &bucket = m_buckets[static_cast<size_t>(bi)];
    for (list<DhtNodeInfo>::iterator it = bucket.begin(); it != bucket.end(); ++it) {
        if (it->id == id) {
            bucket.erase(it);
            return;
        }
    }
}

vector<DhtNodeInfo> RoutingTable::closest(const NodeId &target, int count) const
{
    vector<DhtNodeInfo> all = allNodes();
    DistanceLess cmp;
    cmp.target = target;
    sort(all.begin(), all.end(), cmp);
    if (static_cast<int>(all.size()) > count) {
        all.resize(static_cast<size_t>(count));
    }
    return all;
}

vector<DhtNodeInfo> RoutingTable::allNodes() const
{
    vector<DhtNodeInfo> all;
    for (size_t i = 0; i < m_buckets.size(); ++i) {
        for (list<DhtNodeInfo>::const_iterator it = m_buckets[i].begin(); it != m_buckets[i].end(); ++it) {
            all.push_back(*it);
        }
    }
    return all;
}

int RoutingTable::size() const
{
    int n = 0;
    for (size_t i = 0; i < m_buckets.size(); ++i) {
        n += static_cast<int>(m_buckets[i].size());
    }
    return n;
}

void RoutingTable::clear()
{
    for (size_t i = 0; i < m_buckets.size(); ++i) {
        m_buckets[i].clear();
    }
}

void RoutingTable::load(const vector<DhtNodeInfo> &nodes)
{
    clear();
    for (size_t i = 0; i < nodes.size(); ++i) {
        offer(nodes[i], nullptr);
    }
}

// ---------------------------------------------------------------------------
// MemoryDhtStore
// ---------------------------------------------------------------------------

class MemoryDhtStorePrivate
{
public:
    NodeId id;
    string tokenSecret;
    vector<DhtNodeInfo> nodes;
    // key: infohash bytes + '\0' + compact peer
    map<string, int64_t> peers;
    string error;
};

MemoryDhtStore::MemoryDhtStore()
    : d_ptr(new MemoryDhtStorePrivate)
{
}

MemoryDhtStore::~MemoryDhtStore()
{
    delete d_ptr;
}

bool MemoryDhtStore::loadMeta(NodeId *id, string *tokenSecret)
{
    NG_D(MemoryDhtStore);
    if (!d->id.isValid()) {
        return false;
    }
    if (id) {
        *id = d->id;
    }
    if (tokenSecret) {
        *tokenSecret = d->tokenSecret;
    }
    return true;
}

bool MemoryDhtStore::saveMeta(const NodeId &id, const string &tokenSecret)
{
    NG_D(MemoryDhtStore);
    d->id = id;
    d->tokenSecret = tokenSecret;
    return true;
}

vector<DhtNodeInfo> MemoryDhtStore::loadNodes()
{
    NG_D(MemoryDhtStore);
    return d->nodes;
}

bool MemoryDhtStore::saveNodes(const vector<DhtNodeInfo> &nodes)
{
    NG_D(MemoryDhtStore);
    d->nodes = nodes;
    return true;
}

vector<DhtStore::StoredPeer> MemoryDhtStore::loadPeers(const NodeId &infoHash)
{
    NG_D(MemoryDhtStore);
    vector<StoredPeer> out;
    if (!infoHash.isValid()) {
        return out;
    }
    string prefix = infoHash.toBytes();
    for (map<string, int64_t>::const_iterator it = d->peers.begin(); it != d->peers.end(); ++it) {
        if (it->first.size() < prefix.size() + 1) {
            continue;
        }
        if (it->first.compare(0, prefix.size(), prefix) != 0 || it->first[prefix.size()] != '\0') {
            continue;
        }
        string compact = it->first.substr(prefix.size() + 1);
        vector<DhtPeer> peers;
        if (compact.size() == 6) {
            peers = decodeCompactPeers(compact);
        } else if (compact.size() == 18) {
            peers = decodeCompactPeers6(compact);
        }
        for (size_t i = 0; i < peers.size(); ++i) {
            StoredPeer sp;
            sp.peer = peers[i];
            sp.expireUnix = it->second;
            out.push_back(sp);
        }
    }
    return out;
}

bool MemoryDhtStore::putPeer(const NodeId &infoHash, const DhtPeer &peer, int64_t expireUnix)
{
    NG_D(MemoryDhtStore);
    if (!infoHash.isValid() || !peer.isValid()) {
        d->error = "invalid infohash or peer";
        return false;
    }
    string compact;
    if (peer.address.protocol() == HostAddress::IPv4Protocol) {
        compact = encodeCompactPeers(vector<DhtPeer>(1, peer));
    } else {
        compact = encodeCompactPeers6(vector<DhtPeer>(1, peer));
    }
    if (compact.empty()) {
        d->error = "failed to encode peer";
        return false;
    }
    d->peers[infoHash.toBytes() + string("\0", 1) + compact] = expireUnix;
    return true;
}

bool MemoryDhtStore::removeExpiredPeers(int64_t nowUnix)
{
    NG_D(MemoryDhtStore);
    for (map<string, int64_t>::iterator it = d->peers.begin(); it != d->peers.end();) {
        if (it->second <= nowUnix) {
            d->peers.erase(it++);
        } else {
            ++it;
        }
    }
    return true;
}

string MemoryDhtStore::errorString() const
{
    NG_D(const MemoryDhtStore);
    return d->error;
}

// ---------------------------------------------------------------------------
// LmdbDhtStore
// ---------------------------------------------------------------------------

class LmdbDhtStorePrivate
{
public:
    explicit LmdbDhtStorePrivate(const string &dirPath)
        : dirPath(dirPath)
    {
        LmdbBuilder builder(dirPath);
        builder.maxDbs(8);
        builder.maxMapSize(64 * 1024 * 1024);
        db = builder.create();
        if (!db) {
            error = "failed to open LMDB at " + dirPath;
        }
    }

    string dirPath;
    shared_ptr<Lmdb> db;
    string error;
};

LmdbDhtStore::LmdbDhtStore(const string &dirPath)
    : d_ptr(new LmdbDhtStorePrivate(dirPath))
{
}

LmdbDhtStore::~LmdbDhtStore()
{
    delete d_ptr;
}

bool LmdbDhtStore::isOpen() const
{
    NG_D(const LmdbDhtStore);
    return !!d->db;
}

bool LmdbDhtStore::loadMeta(NodeId *id, string *tokenSecret)
{
    NG_D(LmdbDhtStore);
    if (!d->db) {
        return false;
    }
    shared_ptr<const Transaction> txn = d->db->toRead();
    if (!txn) {
        d->error = "LMDB read txn failed";
        return false;
    }
    const Database &meta = txn->db("meta");
    string idBytes = meta.value("id");
    string secret = meta.value("token_secret");
    if (idBytes.size() != static_cast<size_t>(NodeId::Size)) {
        return false;
    }
    if (id) {
        *id = NodeId::fromBytes(idBytes);
    }
    if (tokenSecret) {
        *tokenSecret = secret;
    }
    return true;
}

bool LmdbDhtStore::saveMeta(const NodeId &id, const string &tokenSecret)
{
    NG_D(LmdbDhtStore);
    if (!d->db || !id.isValid()) {
        d->error = "LMDB not open or invalid id";
        return false;
    }
    shared_ptr<Transaction> txn = d->db->toWrite();
    if (!txn) {
        d->error = "LMDB write txn failed";
        return false;
    }
    Database &meta = txn->db("meta");
    meta.insert("id", id.toBytes());
    meta.insert("token_secret", tokenSecret);
    if (!txn->commit()) {
        d->error = "LMDB commit failed";
        return false;
    }
    return true;
}

vector<DhtNodeInfo> LmdbDhtStore::loadNodes()
{
    NG_D(LmdbDhtStore);
    vector<DhtNodeInfo> out;
    if (!d->db) {
        return out;
    }
    shared_ptr<const Transaction> txn = d->db->toRead();
    if (!txn) {
        return out;
    }
    const Database &nodes = txn->db("nodes");
    for (ConstLmdbIterator it = nodes.constBegin(); it; ++it) {
        string key = it.key();
        string val = it.value();
        if (key.size() != static_cast<size_t>(NodeId::Size)) {
            continue;
        }
        // value: compact endpoint without id (4+2 or 16+2) + optional last_seen(8)
        DhtNodeInfo n;
        n.id = NodeId::fromBytes(key);
        if (val.size() >= 6 && (val.size() == 6 || val.size() == 14)) {
            HostAddress addr;
            uint16_t port = 0;
            readIpv4(val.data(), &addr, &port);
            n.endpoint = DhtEndpoint(addr, port);
        } else if (val.size() >= 18 && (val.size() == 18 || val.size() == 26)) {
            HostAddress addr;
            uint16_t port = 0;
            readIpv6(val.data(), &addr, &port);
            n.endpoint = DhtEndpoint(addr, port);
        } else {
            continue;
        }
        if (n.isValid()) {
            out.push_back(n);
        }
    }
    return out;
}

bool LmdbDhtStore::saveNodes(const vector<DhtNodeInfo> &nodes)
{
    NG_D(LmdbDhtStore);
    if (!d->db) {
        return false;
    }
    shared_ptr<Transaction> txn = d->db->toWrite();
    if (!txn) {
        d->error = "LMDB write txn failed";
        return false;
    }
    Database &db = txn->db("nodes");
    db.clear();
    int64_t ts = nowUnix();
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].isValid()) {
            continue;
        }
        string val;
        if (nodes[i].endpoint.address.protocol() == HostAddress::IPv4Protocol) {
            val = ipv4Bytes(nodes[i].endpoint.address) + portBytes(nodes[i].endpoint.port);
        } else {
            val = ipv6Bytes(nodes[i].endpoint.address) + portBytes(nodes[i].endpoint.port);
        }
        val.append(packExpire(ts));
        db.insert(nodes[i].id.toBytes(), val);
    }
    if (!txn->commit()) {
        d->error = "LMDB commit failed";
        return false;
    }
    return true;
}

vector<DhtStore::StoredPeer> LmdbDhtStore::loadPeers(const NodeId &infoHash)
{
    NG_D(LmdbDhtStore);
    vector<StoredPeer> out;
    if (!d->db || !infoHash.isValid()) {
        return out;
    }
    shared_ptr<const Transaction> txn = d->db->toRead();
    if (!txn) {
        return out;
    }
    const Database &peers = txn->db("peers");
    string prefix = infoHash.toBytes();
    for (ConstLmdbIterator it = peers.lowerBound(prefix); it; ++it) {
        string key = it.key();
        if (key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0) {
            break;
        }
        if (key.size() <= prefix.size()) {
            continue;
        }
        string compact = key.substr(prefix.size());
        vector<DhtPeer> decoded;
        if (compact.size() == 6) {
            decoded = decodeCompactPeers(compact);
        } else if (compact.size() == 18) {
            decoded = decodeCompactPeers6(compact);
        }
        for (size_t i = 0; i < decoded.size(); ++i) {
            StoredPeer sp;
            sp.peer = decoded[i];
            sp.expireUnix = unpackExpire(it.value());
            out.push_back(sp);
        }
    }
    return out;
}

bool LmdbDhtStore::putPeer(const NodeId &infoHash, const DhtPeer &peer, int64_t expireUnix)
{
    NG_D(LmdbDhtStore);
    if (!d->db || !infoHash.isValid() || !peer.isValid()) {
        d->error = "invalid args";
        return false;
    }
    string compact;
    if (peer.address.protocol() == HostAddress::IPv4Protocol) {
        compact = encodeCompactPeers(vector<DhtPeer>(1, peer));
    } else {
        compact = encodeCompactPeers6(vector<DhtPeer>(1, peer));
    }
    if (compact.empty()) {
        return false;
    }
    shared_ptr<Transaction> txn = d->db->toWrite();
    if (!txn) {
        d->error = "LMDB write txn failed";
        return false;
    }
    Database &peers = txn->db("peers");
    peers.insert(infoHash.toBytes() + compact, packExpire(expireUnix));
    if (!txn->commit()) {
        d->error = "LMDB commit failed";
        return false;
    }
    return true;
}

bool LmdbDhtStore::removeExpiredPeers(int64_t nowUnix)
{
    NG_D(LmdbDhtStore);
    if (!d->db) {
        return false;
    }
    shared_ptr<Transaction> txn = d->db->toWrite();
    if (!txn) {
        return false;
    }
    Database &peers = txn->db("peers");
    vector<string> dead;
    for (LmdbIterator it = peers.begin(); !it.isEnd(); ++it) {
        if (unpackExpire(it.value()) <= nowUnix) {
            dead.push_back(it.key());
        }
    }
    for (size_t i = 0; i < dead.size(); ++i) {
        peers.remove(dead[i]);
    }
    return txn->commit();
}

string LmdbDhtStore::errorString() const
{
    NG_D(const LmdbDhtStore);
    return d->error;
}

// ---------------------------------------------------------------------------
// DhtNodePrivate
// ---------------------------------------------------------------------------

DhtNodePrivate::DhtNodePrivate(DhtNode *q, const NodeId &id)
    : q_ptr(q)
    , preferredId(id)
    , tidCounter(0)
    , bindPort(0)
    , opened(false)
    , proto(HostAddress::IPv4Protocol)
{
}

DhtNodePrivate::~DhtNodePrivate()
{
    close();
}

bool DhtNodePrivate::open(uint16_t bindPort, shared_ptr<DhtStore> storeIn,
                          HostAddress::NetworkLayerProtocol protocol)
{
    if (opened) {
        error = "already open";
        return false;
    }
    store = storeIn ? storeIn : make_shared<MemoryDhtStore>();
    proto = protocol;
    this->bindPort = bindPort;

    NodeId loadedId;
    string secret;
    if (store->loadMeta(&loadedId, &secret) && loadedId.isValid()) {
        localId = loadedId;
        tokenSecret = secret;
    } else if (preferredId.isValid()) {
        localId = preferredId;
        tokenSecret = randomBytes(16);
        store->saveMeta(localId, tokenSecret);
    } else {
        localId = NodeId::random();
        tokenSecret = randomBytes(16);
        store->saveMeta(localId, tokenSecret);
    }
    if (tokenSecret.empty()) {
        tokenSecret = randomBytes(16);
        store->saveMeta(localId, tokenSecret);
    }

    table = make_unique<RoutingTable>(localId);
    table->load(store->loadNodes());
    store->removeExpiredPeers(nowUnix());

    socket = make_shared<Socket>(protocol, Socket::UdpSocket);
    HostAddress bindAddr = (protocol == HostAddress::IPv6Protocol) ? HostAddress::AnyIPv6 : HostAddress::AnyIPv4;
    if (!socket->bind(bindAddr, bindPort)) {
        error = "UDP bind failed: " + socket->errorString();
        socket.reset();
        return false;
    }
    this->bindPort = socket->localPort();

    workers = make_unique<CoroutineGroup>();
    workers->spawnWithName("dht-recv", [this] { recvLoop(); });
    workers->spawnWithName("dht-maint", [this] { maintenanceLoop(); });
    opened = true;
    return true;
}

void DhtNodePrivate::close()
{
    if (!opened && !socket) {
        return;
    }
    opened = false;
    if (table && store) {
        persistNodes();
    }
    if (socket) {
        socket->abort();
    }
    if (workers) {
        workers->killall(true);
        workers.reset();
    }
    {
        waitersLock.tryAcquire();
        waiters.clear();
        waitersLock.release();
    }
    socket.reset();
}

void DhtNodePrivate::persistNodes()
{
    if (table && store) {
        store->saveNodes(table->allNodes());
    }
}

void DhtNodePrivate::recvLoop()
{
    while (opened && socket) {
        HostAddress addr;
        uint16_t port = 0;
        string data = socket->recvfrom(65535, &addr, &port);
        if (data.empty()) {
            if (!opened) {
                break;
            }
            Coroutine::msleep(10);
            continue;
        }
        handlePacket(data, addr, port);
    }
}

void DhtNodePrivate::maintenanceLoop()
{
    while (opened) {
        Coroutine::sleep(60.0f);
        if (!opened) {
            break;
        }
        if (store) {
            store->removeExpiredPeers(nowUnix());
            persistNodes();
        }
        // bucket refresh: find_node(self)
        if (table && table->size() > 0) {
            findNode(localId);
        }
    }
}

string DhtNodePrivate::nextTid()
{
    ++tidCounter;
    char buf[2];
    buf[0] = static_cast<char>((tidCounter >> 8) & 0xff);
    buf[1] = static_cast<char>(tidCounter & 0xff);
    return string(buf, 2);
}

string DhtNodePrivate::makeToken(const HostAddress &addr) const
{
    MessageDigest md(MessageDigest::Sha1);
    md.addData(tokenSecret);
    md.addData(addr.toString());
    // rotate token roughly every TokenTtl by mixing time slot
    int64_t slot = nowUnix() / DhtTokenTtlSecs;
    md.addData(reinterpret_cast<const char *>(&slot), sizeof(slot));
    string dig = md.result();
    return dig.substr(0, 8);
}

bool DhtNodePrivate::verifyToken(const HostAddress &addr, const string &token) const
{
    if (token.empty()) {
        return false;
    }
    if (token == makeToken(addr)) {
        return true;
    }
    // previous time slot
    MessageDigest md(MessageDigest::Sha1);
    md.addData(tokenSecret);
    md.addData(addr.toString());
    int64_t slot = nowUnix() / DhtTokenTtlSecs - 1;
    md.addData(reinterpret_cast<const char *>(&slot), sizeof(slot));
    string dig = md.result();
    return token == dig.substr(0, 8);
}

void DhtNodePrivate::heardFrom(const DhtNodeInfo &node, bool allowEvictionPing)
{
    if (!table || !node.isValid()) {
        return;
    }
    DhtNodeInfo evict;
    if (table->offer(node, &evict)) {
        return;
    }
    // Bucket full. Do not ping from the recvLoop path — that would deadlock
    // because the same coroutine cannot wait for the pong it must deliver.
    if (!allowEvictionPing) {
        return;
    }
    NodeId remote;
    if (!ping(evict.endpoint, &remote) || remote != evict.id) {
        table->replace(evict, node);
    }
}

DhtRpcReply DhtNodePrivate::rpc(const DhtEndpoint &ep, const string &query, const Bencode &args)
{
    DhtRpcReply reply;
    if (!socket || !ep.isValid()) {
        return reply;
    }
    string tid = nextTid();
    shared_ptr<ValueEvent<DhtRpcReply>> waiter = make_shared<ValueEvent<DhtRpcReply>>();
    {
        waitersLock.tryAcquire();
        waiters[tid] = waiter;
        waitersLock.release();
    }

    map<string, Bencode> msg;
    msg["t"] = tid;
    msg["y"] = "q";
    msg["q"] = query;
    msg["a"] = args;
    string packet = Bencode(std::move(msg)).encode();
    if (socket->sendto(packet, ep.address, ep.port) <= 0) {
        waitersLock.tryAcquire();
        waiters.erase(tid);
        waitersLock.release();
        return reply;
    }

    try {
        Timeout timeout(DhtRpcTimeoutSecs);
        (void) timeout;
        DhtRpcReply got = waiter->tryWait(static_cast<uint32_t>(DhtRpcTimeoutSecs * 1000));
        reply = got;
    } catch (TimeoutException &) {
        reply.ok = false;
    }

    waitersLock.tryAcquire();
    waiters.erase(tid);
    waitersLock.release();
    return reply;
}

bool DhtNodePrivate::ping(const DhtEndpoint &ep, NodeId *remoteId)
{
    map<string, Bencode> args;
    args["id"] = localId.toBytes();
    DhtRpcReply r = rpc(ep, "ping", Bencode(std::move(args)));
    if (!r.ok || !r.message.isDict()) {
        return false;
    }
    Bencode resp = dictGet(r.message, "r");
    string idBytes = dictGet(resp, "id").toString();
    NodeId rid = NodeId::fromBytes(idBytes);
    if (!rid.isValid()) {
        return false;
    }
    if (remoteId) {
        *remoteId = rid;
    }
    heardFrom(DhtNodeInfo(rid, DhtEndpoint(r.fromAddress, r.fromPort)));
    return true;
}

void DhtNodePrivate::handlePacket(const string &data, const HostAddress &addr, uint16_t port)
{
    string err;
    Bencode msg = Bencode::decode(data, &err);
    if (!msg.isDict()) {
        return;
    }
    string y = dictGet(msg, "y").toString();
    string tid = dictGet(msg, "t").toString();
    if (y == "q") {
        handleQuery(msg, addr, port);
        return;
    }
    if (y == "r" || y == "e") {
        DhtRpcReply reply;
        reply.ok = (y == "r");
        reply.message = msg;
        reply.fromAddress = addr;
        reply.fromPort = port;
        waitersLock.tryAcquire();
        map<string, shared_ptr<ValueEvent<DhtRpcReply>>>::iterator it = waiters.find(tid);
        if (it != waiters.end()) {
            it->second->send(reply);
        }
        waitersLock.release();
        if (y == "r") {
            string idBytes = dictGet(dictGet(msg, "r"), "id").toString();
            NodeId rid = NodeId::fromBytes(idBytes);
            if (rid.isValid()) {
                // refresh without recursive ping (we are inside recvLoop)
                heardFrom(DhtNodeInfo(rid, DhtEndpoint(addr, port)), false);
            }
        }
    }
}

Bencode DhtNodePrivate::makeResponse(const string &tid, const Bencode &rDict)
{
    map<string, Bencode> msg;
    msg["t"] = tid;
    msg["y"] = "r";
    msg["r"] = rDict;
    return Bencode(std::move(msg));
}

Bencode DhtNodePrivate::makeError(const string &tid, int code, const string &msgText)
{
    vector<Bencode> err;
    err.push_back(Bencode(static_cast<int64_t>(code)));
    err.push_back(Bencode(msgText));
    map<string, Bencode> msg;
    msg["t"] = tid;
    msg["y"] = "e";
    msg["e"] = Bencode(std::move(err));
    return Bencode(std::move(msg));
}

void DhtNodePrivate::handleQuery(const Bencode &msg, const HostAddress &addr, uint16_t port)
{
    string tid = dictGet(msg, "t").toString();
    string q = dictGet(msg, "q").toString();
    Bencode args = dictGet(msg, "a");
    string remoteIdBytes = dictGet(args, "id").toString();
    NodeId remoteId = NodeId::fromBytes(remoteIdBytes);
    if (remoteId.isValid()) {
        heardFrom(DhtNodeInfo(remoteId, DhtEndpoint(addr, port)), false);
    }

    map<string, Bencode> r;
    r["id"] = localId.toBytes();

    if (q == "ping") {
        socket->sendto(makeResponse(tid, Bencode(r)).encode(), addr, port);
        return;
    }

    if (q == "find_node") {
        NodeId target = NodeId::fromBytes(dictGet(args, "target").toString());
        vector<DhtNodeInfo> closest = table ? table->closest(target.isValid() ? target : remoteId, DhtK)
                                           : vector<DhtNodeInfo>();
        r["nodes"] = encodeCompactNodes(closest);
        r["nodes6"] = encodeCompactNodes6(closest);
        socket->sendto(makeResponse(tid, Bencode(std::move(r))).encode(), addr, port);
        return;
    }

    if (q == "get_peers") {
        NodeId infoHash = NodeId::fromBytes(dictGet(args, "info_hash").toString());
        r["token"] = makeToken(addr);
        bool havePeers = false;
        if (infoHash.isValid() && store) {
            vector<DhtStore::StoredPeer> stored = store->loadPeers(infoHash);
            vector<DhtPeer> v4;
            vector<DhtPeer> v6;
            int64_t now = nowUnix();
            for (size_t i = 0; i < stored.size(); ++i) {
                if (stored[i].expireUnix <= now) {
                    continue;
                }
                if (stored[i].peer.address.protocol() == HostAddress::IPv4Protocol) {
                    v4.push_back(stored[i].peer);
                } else {
                    v6.push_back(stored[i].peer);
                }
            }
            if (!v4.empty() || !v6.empty()) {
                vector<Bencode> values;
                string c4 = encodeCompactPeers(v4);
                string c6 = encodeCompactPeers6(v6);
                // BEP-5: values is list of compact peer strings
                for (size_t i = 0; i + 6 <= c4.size(); i += 6) {
                    values.push_back(Bencode(c4.substr(i, 6)));
                }
                for (size_t i = 0; i + 18 <= c6.size(); i += 18) {
                    values.push_back(Bencode(c6.substr(i, 18)));
                }
                r["values"] = Bencode(std::move(values));
                havePeers = true;
            }
        }
        if (!havePeers) {
            vector<DhtNodeInfo> closest =
                    table ? table->closest(infoHash.isValid() ? infoHash : remoteId, DhtK) : vector<DhtNodeInfo>();
            r["nodes"] = encodeCompactNodes(closest);
            r["nodes6"] = encodeCompactNodes6(closest);
        }
        socket->sendto(makeResponse(tid, Bencode(std::move(r))).encode(), addr, port);
        return;
    }

    if (q == "announce_peer") {
        NodeId infoHash = NodeId::fromBytes(dictGet(args, "info_hash").toString());
        string token = dictGet(args, "token").toString();
        int64_t implied = dictGet(args, "implied_port").toInteger(0);
        int64_t peerPort = dictGet(args, "port").toInteger(0);
        uint16_t announcePort = implied ? port : static_cast<uint16_t>(peerPort);
        if (!infoHash.isValid() || !verifyToken(addr, token) || announcePort == 0) {
            socket->sendto(makeError(tid, 203, "Bad token").encode(), addr, port);
            return;
        }
        if (store) {
            store->putPeer(infoHash, DhtPeer(addr, announcePort), nowUnix() + DhtPeerTtlSecs);
        }
        socket->sendto(makeResponse(tid, Bencode(std::move(r))).encode(), addr, port);
        return;
    }

    socket->sendto(makeError(tid, 204, "Method Unknown").encode(), addr, port);
}

DhtNodePrivate::LookupResult DhtNodePrivate::iterativeLookup(const NodeId &target, bool wantPeers)
{
    LookupResult result;
    if (!table || !target.isValid()) {
        return result;
    }

    vector<DhtNodeInfo> shortlist = table->closest(target, DhtK);
    map<string, bool> queried;  // id bytes -> done
    map<string, bool> seen;

    for (size_t i = 0; i < shortlist.size(); ++i) {
        seen[shortlist[i].id.toBytes()] = true;
    }

    bool improved = true;
    while (improved) {
        improved = false;
        vector<DhtNodeInfo> toQuery;
        DistanceLess cmp;
        cmp.target = target;
        sort(shortlist.begin(), shortlist.end(), cmp);
        for (size_t i = 0; i < shortlist.size() && static_cast<int>(toQuery.size()) < DhtAlpha; ++i) {
            string key = shortlist[i].id.toBytes();
            if (queried[key]) {
                continue;
            }
            toQuery.push_back(shortlist[i]);
        }
        if (toQuery.empty()) {
            break;
        }

        // sequential α queries (coroutine-friendly; could parallelize with CoroutineGroup)
        for (size_t i = 0; i < toQuery.size(); ++i) {
            queried[toQuery[i].id.toBytes()] = true;
            map<string, Bencode> args;
            args["id"] = localId.toBytes();
            DhtRpcReply reply;
            if (wantPeers) {
                args["info_hash"] = target.toBytes();
                reply = rpc(toQuery[i].endpoint, "get_peers", Bencode(std::move(args)));
            } else {
                args["target"] = target.toBytes();
                reply = rpc(toQuery[i].endpoint, "find_node", Bencode(std::move(args)));
            }
            if (!reply.ok) {
                table->remove(toQuery[i].id);
                continue;
            }
            Bencode r = dictGet(reply.message, "r");
            string rid = dictGet(r, "id").toString();
            NodeId remote = NodeId::fromBytes(rid);
            if (remote.isValid()) {
                heardFrom(DhtNodeInfo(remote, DhtEndpoint(reply.fromAddress, reply.fromPort)));
            }
            if (dictHas(r, "token")) {
                result.token = dictGet(r, "token").toString();
                result.tokenFrom = DhtEndpoint(reply.fromAddress, reply.fromPort);
            }
            if (dictHas(r, "values") && dictGet(r, "values").isList()) {
                const vector<Bencode> &vals = dictGet(r, "values").toList();
                for (size_t v = 0; v < vals.size(); ++v) {
                    string c = vals[v].toString();
                    vector<DhtPeer> peers;
                    if (c.size() == 6) {
                        peers = decodeCompactPeers(c);
                    } else if (c.size() == 18) {
                        peers = decodeCompactPeers6(c);
                    }
                    for (size_t p = 0; p < peers.size(); ++p) {
                        result.peers.push_back(peers[p]);
                    }
                }
            }
            vector<DhtNodeInfo> nodes = decodeCompactNodes(dictGet(r, "nodes").toString());
            vector<DhtNodeInfo> nodes6 = decodeCompactNodes6(dictGet(r, "nodes6").toString());
            nodes.insert(nodes.end(), nodes6.begin(), nodes6.end());
            for (size_t n = 0; n < nodes.size(); ++n) {
                heardFrom(nodes[n]);
                string key = nodes[n].id.toBytes();
                if (!seen[key]) {
                    seen[key] = true;
                    shortlist.push_back(nodes[n]);
                    improved = true;
                }
            }
        }
        // keep shortlist bounded
        sort(shortlist.begin(), shortlist.end(), cmp);
        if (static_cast<int>(shortlist.size()) > DhtK * 4) {
            shortlist.resize(static_cast<size_t>(DhtK * 4));
        }
    }

    DistanceLess cmp;
    cmp.target = target;
    sort(shortlist.begin(), shortlist.end(), cmp);
    if (static_cast<int>(shortlist.size()) > DhtK) {
        shortlist.resize(static_cast<size_t>(DhtK));
    }
    result.nodes = shortlist;
    persistNodes();
    return result;
}

bool DhtNodePrivate::bootstrap(const vector<DhtEndpoint> &seeds)
{
    if (!opened) {
        error = "not open";
        return false;
    }
    for (size_t i = 0; i < seeds.size(); ++i) {
        NodeId rid;
        if (ping(seeds[i], &rid)) {
            // ok
        }
    }
    findNode(localId);
    return table && table->size() > 0;
}

vector<DhtNodeInfo> DhtNodePrivate::findNode(const NodeId &target)
{
    return iterativeLookup(target, false).nodes;
}

vector<DhtPeer> DhtNodePrivate::getPeers(const NodeId &infoHash)
{
    LookupResult r = iterativeLookup(infoHash, true);
    // also return locally stored
    if (store) {
        vector<DhtStore::StoredPeer> local = store->loadPeers(infoHash);
        int64_t now = nowUnix();
        for (size_t i = 0; i < local.size(); ++i) {
            if (local[i].expireUnix > now) {
                r.peers.push_back(local[i].peer);
            }
        }
    }
    // dedupe
    vector<DhtPeer> unique;
    for (size_t i = 0; i < r.peers.size(); ++i) {
        bool found = false;
        for (size_t j = 0; j < unique.size(); ++j) {
            if (unique[j] == r.peers[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            unique.push_back(r.peers[i]);
        }
    }
    return unique;
}

bool DhtNodePrivate::announcePeer(const NodeId &infoHash, uint16_t peerPort, const string &token)
{
    if (!opened || !infoHash.isValid() || peerPort == 0) {
        error = "invalid announce args";
        return false;
    }
    LookupResult r = iterativeLookup(infoHash, true);
    string useToken = token.empty() ? r.token : token;
    DhtEndpoint ep = r.tokenFrom;
    if (useToken.empty() || !ep.isValid()) {
        // announce to closest nodes after get_peers-style queries
        bool any = false;
        for (size_t i = 0; i < r.nodes.size(); ++i) {
            map<string, Bencode> args;
            args["id"] = localId.toBytes();
            args["info_hash"] = infoHash.toBytes();
            // fetch token first
            DhtRpcReply gp = rpc(r.nodes[i].endpoint, "get_peers", Bencode(std::move(args)));
            if (!gp.ok) {
                continue;
            }
            string t = dictGet(dictGet(gp.message, "r"), "token").toString();
            if (t.empty()) {
                continue;
            }
            map<string, Bencode> aargs;
            aargs["id"] = localId.toBytes();
            aargs["info_hash"] = infoHash.toBytes();
            aargs["port"] = Bencode(static_cast<int64_t>(peerPort));
            aargs["token"] = t;
            DhtRpcReply ar = rpc(r.nodes[i].endpoint, "announce_peer", Bencode(std::move(aargs)));
            if (ar.ok) {
                any = true;
            }
        }
        // also store locally
        if (store) {
            HostAddress local = (proto == HostAddress::IPv6Protocol) ? HostAddress::LocalHostIPv6
                                                                     : HostAddress::LocalHost;
            store->putPeer(infoHash, DhtPeer(local, peerPort), nowUnix() + DhtPeerTtlSecs);
            return true;
        }
        return any;
    }

    map<string, Bencode> aargs;
    aargs["id"] = localId.toBytes();
    aargs["info_hash"] = infoHash.toBytes();
    aargs["port"] = Bencode(static_cast<int64_t>(peerPort));
    aargs["token"] = useToken;
    DhtRpcReply ar = rpc(ep, "announce_peer", Bencode(std::move(aargs)));
    if (store) {
        HostAddress local =
                (proto == HostAddress::IPv6Protocol) ? HostAddress::LocalHostIPv6 : HostAddress::LocalHost;
        store->putPeer(infoHash, DhtPeer(local, peerPort), nowUnix() + DhtPeerTtlSecs);
    }
    return ar.ok;
}

// ---------------------------------------------------------------------------
// DhtNode
// ---------------------------------------------------------------------------

DhtNode::DhtNode(const NodeId &id)
    : d_ptr(new DhtNodePrivate(this, id))
{
}

DhtNode::~DhtNode()
{
    delete d_ptr;
}

bool DhtNode::open(uint16_t bindPort, shared_ptr<DhtStore> store, HostAddress::NetworkLayerProtocol proto)
{
    NG_D(DhtNode);
    return d->open(bindPort, store, proto);
}

void DhtNode::close()
{
    NG_D(DhtNode);
    d->close();
}

bool DhtNode::isOpen() const
{
    NG_D(const DhtNode);
    return d->opened;
}

NodeId DhtNode::id() const
{
    NG_D(const DhtNode);
    return d->localId;
}

uint16_t DhtNode::localPort() const
{
    NG_D(const DhtNode);
    return d->bindPort;
}

shared_ptr<DhtStore> DhtNode::store() const
{
    NG_D(const DhtNode);
    return d->store;
}

bool DhtNode::bootstrap(const vector<DhtEndpoint> &seeds)
{
    NG_D(DhtNode);
    return d->bootstrap(seeds);
}

vector<DhtNodeInfo> DhtNode::findNode(const NodeId &target)
{
    NG_D(DhtNode);
    return d->findNode(target);
}

vector<DhtPeer> DhtNode::getPeers(const NodeId &infoHash)
{
    NG_D(DhtNode);
    return d->getPeers(infoHash);
}

bool DhtNode::announcePeer(const NodeId &infoHash, uint16_t peerPort, const string &token)
{
    NG_D(DhtNode);
    return d->announcePeer(infoHash, peerPort, token);
}

int DhtNode::routingTableSize() const
{
    NG_D(const DhtNode);
    return d->table ? d->table->size() : 0;
}

string DhtNode::errorString() const
{
    NG_D(const DhtNode);
    return d->error;
}

}  // namespace qtng
