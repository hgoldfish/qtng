#include <cstring>

#include "bridge/core_access.h"
#include "kademlia.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

struct NodeIdCoreBridge
{
    static qtng_core::NodeId core(const NodeId &id);
    static void setCore(NodeId &id, const qtng_core::NodeId &core);
};

namespace {

class SocketPrivate
{
public:
    static qtng_core::HostAddress toCoreAddress(const HostAddress &addr)
    {
        if (addr.isNull()) {
            return qtng_core::HostAddress();
        }
        if (addr.protocol() == HostAddress::IPv4Protocol) {
            return qtng_core::HostAddress(addr.toIPv4Address());
        }
        const IPv6Address v6 = addr.toIPv6Address();
        qtng_core::HostAddress result(v6.c);
        result.setScopeId(toStdString(addr.scopeId()));
        return result;
    }

    static HostAddress toQtAddress(const qtng_core::HostAddress &addr)
    {
        if (addr.isNull()) {
            return HostAddress();
        }
        if (addr.protocol() == qtng_core::HostAddress::IPv4Protocol) {
            return HostAddress(addr.toIPv4Address());
        }
        const qtng_core::IPv6Address v6 = addr.toIPv6Address();
        IPv6Address qv6;
        memcpy(qv6.c, v6.c, 16);
        HostAddress result(qv6);
        result.setScopeId(toQString(addr.scopeId()));
        return result;
    }
};

qtng_core::NodeId toCoreNodeId(const NodeId &id);
NodeId fromCoreNodeId(const qtng_core::NodeId &id);

qtng_core::DhtEndpoint toCoreEndpoint(const DhtEndpoint &ep)
{
    return qtng_core::DhtEndpoint(SocketPrivate::toCoreAddress(ep.address), ep.port);
}

DhtEndpoint fromCoreEndpoint(const qtng_core::DhtEndpoint &ep)
{
    return DhtEndpoint(SocketPrivate::toQtAddress(ep.address), ep.port);
}

qtng_core::DhtNodeInfo toCoreNodeInfo(const DhtNodeInfo &info)
{
    return qtng_core::DhtNodeInfo(toCoreNodeId(info.id), toCoreEndpoint(info.endpoint));
}

DhtNodeInfo fromCoreNodeInfo(const qtng_core::DhtNodeInfo &info)
{
    return DhtNodeInfo(fromCoreNodeId(info.id), fromCoreEndpoint(info.endpoint));
}

qtng_core::DhtPeer toCorePeer(const DhtPeer &peer)
{
    return qtng_core::DhtPeer(SocketPrivate::toCoreAddress(peer.address), peer.port);
}

DhtPeer fromCorePeer(const qtng_core::DhtPeer &peer)
{
    return DhtPeer(SocketPrivate::toQtAddress(peer.address), peer.port);
}

QList<DhtNodeInfo> fromCoreNodeList(const vector<qtng_core::DhtNodeInfo> &nodes)
{
    QList<DhtNodeInfo> result;
    result.reserve(static_cast<int>(nodes.size()));
    for (const qtng_core::DhtNodeInfo &node : nodes) {
        result.append(fromCoreNodeInfo(node));
    }
    return result;
}

vector<qtng_core::DhtNodeInfo> toCoreNodeList(const QList<DhtNodeInfo> &nodes)
{
    vector<qtng_core::DhtNodeInfo> result;
    result.reserve(static_cast<size_t>(nodes.size()));
    for (const DhtNodeInfo &node : nodes) {
        result.push_back(toCoreNodeInfo(node));
    }
    return result;
}

QList<DhtPeer> fromCorePeerList(const vector<qtng_core::DhtPeer> &peers)
{
    QList<DhtPeer> result;
    result.reserve(static_cast<int>(peers.size()));
    for (const qtng_core::DhtPeer &peer : peers) {
        result.append(fromCorePeer(peer));
    }
    return result;
}

class QtDhtStoreAdapter : public qtng_core::DhtStore
{
public:
    explicit QtDhtStoreAdapter(const QSharedPointer< ::qtng::DhtStore> &store)
        : qtStore(store)
    {
    }

    bool loadMeta(qtng_core::NodeId *id, string *tokenSecret) override
    {
        NodeId qtId;
        QByteArray secret;
        if (!qtStore->loadMeta(&qtId, &secret)) {
            return false;
        }
        *id = toCoreNodeId(qtId);
        *tokenSecret = toStdString(secret);
        return true;
    }

    bool saveMeta(const qtng_core::NodeId &id, const string &tokenSecret) override
    {
        return qtStore->saveMeta(fromCoreNodeId(id), toQByteArray(tokenSecret));
    }

    vector<qtng_core::DhtNodeInfo> loadNodes() override
    {
        return toCoreNodeList(qtStore->loadNodes());
    }

    bool saveNodes(const vector<qtng_core::DhtNodeInfo> &nodes) override
    {
        return qtStore->saveNodes(fromCoreNodeList(nodes));
    }

    vector<qtng_core::DhtStore::StoredPeer> loadPeers(const qtng_core::NodeId &infoHash) override
    {
        const QList< ::qtng::DhtStore::StoredPeer> peers = qtStore->loadPeers(fromCoreNodeId(infoHash));
        vector<qtng_core::DhtStore::StoredPeer> result;
        result.reserve(static_cast<size_t>(peers.size()));
        for (const ::qtng::DhtStore::StoredPeer &peer : peers) {
            qtng_core::DhtStore::StoredPeer item;
            item.peer = toCorePeer(peer.peer);
            item.expireUnix = peer.expireUnix;
            result.push_back(item);
        }
        return result;
    }

    bool putPeer(const qtng_core::NodeId &infoHash, const qtng_core::DhtPeer &peer, int64_t expireUnix) override
    {
        return qtStore->putPeer(fromCoreNodeId(infoHash), fromCorePeer(peer), expireUnix);
    }

    bool removeExpiredPeers(int64_t nowUnix) override
    {
        return qtStore->removeExpiredPeers(nowUnix);
    }

    string errorString() const override
    {
        return toStdString(qtStore->errorString());
    }

    QSharedPointer< ::qtng::DhtStore> qtStore;
};

shared_ptr<qtng_core::DhtStore> toCoreStore(const QSharedPointer< ::qtng::DhtStore> &store)
{
    if (!store) {
        return shared_ptr<qtng_core::DhtStore>();
    }
    return make_shared<QtDhtStoreAdapter>(store);
}

}  // namespace

class NodeIdPrivate : public QSharedData
{
public:
    qtng_core::NodeId core;
};

qtng_core::NodeId NodeIdCoreBridge::core(const NodeId &id)
{
    return id.d ? id.d->core : qtng_core::NodeId();
}

void NodeIdCoreBridge::setCore(NodeId &id, const qtng_core::NodeId &core)
{
    if (!id.d) {
        id.d = new NodeIdPrivate;
    }
    id.d->core = core;
}

namespace {

qtng_core::NodeId toCoreNodeId(const NodeId &id)
{
    return NodeIdCoreBridge::core(id);
}

NodeId fromCoreNodeId(const qtng_core::NodeId &id)
{
    NodeId result;
    NodeIdCoreBridge::setCore(result, id);
    return result;
}

}  // namespace

NodeId::NodeId()
    : d(new NodeIdPrivate)
{
}

NodeId::NodeId(const QByteArray &raw20)
    : d(new NodeIdPrivate)
{
    d->core = qtng_core::NodeId::fromBytes(toStdString(raw20));
}

NodeId::NodeId(const NodeId &other)
    : d(other.d)
{
}

NodeId &NodeId::operator=(const NodeId &other)
{
    d = other.d;
    return *this;
}

NodeId::~NodeId() = default;

NodeId NodeId::random()
{
    return fromCoreNodeId(qtng_core::NodeId::random());
}

NodeId NodeId::fromBytes(const QByteArray &raw20)
{
    return fromCoreNodeId(qtng_core::NodeId::fromBytes(toStdString(raw20)));
}

NodeId NodeId::fromHex(const QString &hex40)
{
    return fromCoreNodeId(qtng_core::NodeId::fromHex(toStdString(hex40)));
}

bool NodeId::isValid() const
{
    return d->core.isValid();
}

QByteArray NodeId::toBytes() const
{
    return toQByteArray(d->core.toBytes());
}

QString NodeId::toHex() const
{
    return toQString(d->core.toHex());
}

NodeId NodeId::operator^(const NodeId &other) const
{
    return fromCoreNodeId(d->core ^ other.d->core);
}

int NodeId::commonPrefixLength(const NodeId &other) const
{
    return d->core.commonPrefixLength(other.d->core);
}

int NodeId::bucketIndex(const NodeId &other) const
{
    return d->core.bucketIndex(other.d->core);
}

bool NodeId::operator==(const NodeId &other) const
{
    return d->core == other.d->core;
}

bool NodeId::operator!=(const NodeId &other) const
{
    return d->core != other.d->core;
}

bool NodeId::operator<(const NodeId &other) const
{
    return d->core < other.d->core;
}

QByteArray encodeCompactNodes(const QList<DhtNodeInfo> &nodes)
{
    return toQByteArray(qtng_core::encodeCompactNodes(toCoreNodeList(nodes)));
}

QByteArray encodeCompactNodes6(const QList<DhtNodeInfo> &nodes)
{
    return toQByteArray(qtng_core::encodeCompactNodes6(toCoreNodeList(nodes)));
}

QList<DhtNodeInfo> decodeCompactNodes(const QByteArray &data)
{
    return fromCoreNodeList(qtng_core::decodeCompactNodes(toStdString(data)));
}

QList<DhtNodeInfo> decodeCompactNodes6(const QByteArray &data)
{
    return fromCoreNodeList(qtng_core::decodeCompactNodes6(toStdString(data)));
}

QByteArray encodeCompactPeers(const QList<DhtPeer> &peers)
{
    vector<qtng_core::DhtPeer> corePeers;
    corePeers.reserve(static_cast<size_t>(peers.size()));
    for (const DhtPeer &peer : peers) {
        corePeers.push_back(toCorePeer(peer));
    }
    return toQByteArray(qtng_core::encodeCompactPeers(corePeers));
}

QByteArray encodeCompactPeers6(const QList<DhtPeer> &peers)
{
    vector<qtng_core::DhtPeer> corePeers;
    corePeers.reserve(static_cast<size_t>(peers.size()));
    for (const DhtPeer &peer : peers) {
        corePeers.push_back(toCorePeer(peer));
    }
    return toQByteArray(qtng_core::encodeCompactPeers6(corePeers));
}

QList<DhtPeer> decodeCompactPeers(const QByteArray &data)
{
    return fromCorePeerList(qtng_core::decodeCompactPeers(toStdString(data)));
}

QList<DhtPeer> decodeCompactPeers6(const QByteArray &data)
{
    return fromCorePeerList(qtng_core::decodeCompactPeers6(toStdString(data)));
}

class MemoryDhtStorePrivate
{
public:
    MemoryDhtStorePrivate()
        : core(make_shared<qtng_core::MemoryDhtStore>())
    {
    }

    shared_ptr<qtng_core::MemoryDhtStore> core;
};

MemoryDhtStore::MemoryDhtStore()
    : d_ptr(new MemoryDhtStorePrivate)
{
}

MemoryDhtStore::~MemoryDhtStore()
{
    delete d_ptr;
}

bool MemoryDhtStore::loadMeta(NodeId *id, QByteArray *tokenSecret)
{
    Q_D(MemoryDhtStore);
    qtng_core::NodeId coreId;
    string secret;
    if (!d->core->loadMeta(&coreId, &secret)) {
        return false;
    }
    *id = fromCoreNodeId(coreId);
    *tokenSecret = toQByteArray(secret);
    return true;
}

bool MemoryDhtStore::saveMeta(const NodeId &id, const QByteArray &tokenSecret)
{
    Q_D(MemoryDhtStore);
    return d->core->saveMeta(toCoreNodeId(id), toStdString(tokenSecret));
}

QList<DhtNodeInfo> MemoryDhtStore::loadNodes()
{
    Q_D(MemoryDhtStore);
    return fromCoreNodeList(d->core->loadNodes());
}

bool MemoryDhtStore::saveNodes(const QList<DhtNodeInfo> &nodes)
{
    Q_D(MemoryDhtStore);
    return d->core->saveNodes(toCoreNodeList(nodes));
}

QList<DhtStore::StoredPeer> MemoryDhtStore::loadPeers(const NodeId &infoHash)
{
    Q_D(MemoryDhtStore);
    const vector<qtng_core::DhtStore::StoredPeer> peers = d->core->loadPeers(toCoreNodeId(infoHash));
    QList<DhtStore::StoredPeer> result;
    result.reserve(static_cast<int>(peers.size()));
    for (const qtng_core::DhtStore::StoredPeer &peer : peers) {
        DhtStore::StoredPeer item;
        item.peer = fromCorePeer(peer.peer);
        item.expireUnix = peer.expireUnix;
        result.append(item);
    }
    return result;
}

bool MemoryDhtStore::putPeer(const NodeId &infoHash, const DhtPeer &peer, qint64 expireUnix)
{
    Q_D(MemoryDhtStore);
    return d->core->putPeer(toCoreNodeId(infoHash), toCorePeer(peer), expireUnix);
}

bool MemoryDhtStore::removeExpiredPeers(qint64 nowUnix)
{
    Q_D(MemoryDhtStore);
    return d->core->removeExpiredPeers(nowUnix);
}

QString MemoryDhtStore::errorString() const
{
    Q_D(const MemoryDhtStore);
    return toQString(d->core->errorString());
}

class LmdbDhtStorePrivate
{
public:
    explicit LmdbDhtStorePrivate(const QString &dirPath)
        : core(make_shared<qtng_core::LmdbDhtStore>(toStdString(dirPath)))
    {
    }

    shared_ptr<qtng_core::LmdbDhtStore> core;
};

LmdbDhtStore::LmdbDhtStore(const QString &dirPath)
    : d_ptr(new LmdbDhtStorePrivate(dirPath))
{
}

LmdbDhtStore::~LmdbDhtStore()
{
    delete d_ptr;
}

bool LmdbDhtStore::isOpen() const
{
    Q_D(const LmdbDhtStore);
    return d->core->isOpen();
}

bool LmdbDhtStore::loadMeta(NodeId *id, QByteArray *tokenSecret)
{
    Q_D(LmdbDhtStore);
    qtng_core::NodeId coreId;
    string secret;
    if (!d->core->loadMeta(&coreId, &secret)) {
        return false;
    }
    *id = fromCoreNodeId(coreId);
    *tokenSecret = toQByteArray(secret);
    return true;
}

bool LmdbDhtStore::saveMeta(const NodeId &id, const QByteArray &tokenSecret)
{
    Q_D(LmdbDhtStore);
    return d->core->saveMeta(toCoreNodeId(id), toStdString(tokenSecret));
}

QList<DhtNodeInfo> LmdbDhtStore::loadNodes()
{
    Q_D(LmdbDhtStore);
    return fromCoreNodeList(d->core->loadNodes());
}

bool LmdbDhtStore::saveNodes(const QList<DhtNodeInfo> &nodes)
{
    Q_D(LmdbDhtStore);
    return d->core->saveNodes(toCoreNodeList(nodes));
}

QList<DhtStore::StoredPeer> LmdbDhtStore::loadPeers(const NodeId &infoHash)
{
    Q_D(LmdbDhtStore);
    const vector<qtng_core::DhtStore::StoredPeer> peers = d->core->loadPeers(toCoreNodeId(infoHash));
    QList<DhtStore::StoredPeer> result;
    result.reserve(static_cast<int>(peers.size()));
    for (const qtng_core::DhtStore::StoredPeer &peer : peers) {
        DhtStore::StoredPeer item;
        item.peer = fromCorePeer(peer.peer);
        item.expireUnix = peer.expireUnix;
        result.append(item);
    }
    return result;
}

bool LmdbDhtStore::putPeer(const NodeId &infoHash, const DhtPeer &peer, qint64 expireUnix)
{
    Q_D(LmdbDhtStore);
    return d->core->putPeer(toCoreNodeId(infoHash), toCorePeer(peer), expireUnix);
}

bool LmdbDhtStore::removeExpiredPeers(qint64 nowUnix)
{
    Q_D(LmdbDhtStore);
    return d->core->removeExpiredPeers(nowUnix);
}

QString LmdbDhtStore::errorString() const
{
    Q_D(const LmdbDhtStore);
    return toQString(d->core->errorString());
}

class DhtNodePrivate
{
public:
    explicit DhtNodePrivate(const NodeId &id)
        : core(new qtng_core::DhtNode(toCoreNodeId(id)))
    {
    }

    ~DhtNodePrivate()
    {
        delete core;
    }

    qtng_core::DhtNode *core;
    QSharedPointer< ::qtng::DhtStore> qtStore;
};

DhtNode::DhtNode(const NodeId &id)
    : d_ptr(new DhtNodePrivate(id))
{
}

DhtNode::~DhtNode()
{
    delete d_ptr;
}

bool DhtNode::open(quint16 bindPort, QSharedPointer< ::qtng::DhtStore> store, HostAddress::NetworkLayerProtocol proto)
{
    Q_D(DhtNode);
    d->qtStore = store;
    return d->core->open(bindPort, toCoreStore(store),
                         static_cast<qtng_core::HostAddress::NetworkLayerProtocol>(proto));
}

void DhtNode::close()
{
    Q_D(DhtNode);
    d->core->close();
    d->qtStore.clear();
}

bool DhtNode::isOpen() const
{
    Q_D(const DhtNode);
    return d->core->isOpen();
}

NodeId DhtNode::id() const
{
    Q_D(const DhtNode);
    return fromCoreNodeId(d->core->id());
}

quint16 DhtNode::localPort() const
{
    Q_D(const DhtNode);
    return d->core->localPort();
}

QSharedPointer<DhtStore> DhtNode::store() const
{
    Q_D(const DhtNode);
    return d->qtStore;
}

bool DhtNode::bootstrap(const QList<DhtEndpoint> &seeds)
{
    Q_D(DhtNode);
    vector<qtng_core::DhtEndpoint> coreSeeds;
    coreSeeds.reserve(static_cast<size_t>(seeds.size()));
    for (const DhtEndpoint &seed : seeds) {
        coreSeeds.push_back(toCoreEndpoint(seed));
    }
    return d->core->bootstrap(coreSeeds);
}

QList<DhtNodeInfo> DhtNode::findNode(const NodeId &target)
{
    Q_D(DhtNode);
    return fromCoreNodeList(d->core->findNode(toCoreNodeId(target)));
}

QList<DhtPeer> DhtNode::getPeers(const NodeId &infoHash)
{
    Q_D(DhtNode);
    return fromCorePeerList(d->core->getPeers(toCoreNodeId(infoHash)));
}

bool DhtNode::announcePeer(const NodeId &infoHash, quint16 peerPort, const QByteArray &token)
{
    Q_D(DhtNode);
    return d->core->announcePeer(toCoreNodeId(infoHash), peerPort, toStdString(token));
}

int DhtNode::routingTableSize() const
{
    Q_D(const DhtNode);
    return d->core->routingTableSize();
}

QString DhtNode::errorString() const
{
    Q_D(const DhtNode);
    return toQString(d->core->errorString());
}

}  // namespace QTNETWORKNG_NAMESPACE
