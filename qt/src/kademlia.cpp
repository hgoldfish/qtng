#include <cstring>

#include "bridge/core_access.h"
#include "bridge/hostaddress_access.h"
#include "kademlia.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

struct NodeIdCoreBridge
{
    static QTNG_NAMESPACE::NodeId core(const NodeId &id);
    static void setCore(NodeId &id, const QTNG_NAMESPACE::NodeId &core);
};

namespace {

QTNG_NAMESPACE::NodeId toCoreNodeId(const NodeId &id);
NodeId fromCoreNodeId(const QTNG_NAMESPACE::NodeId &id);

QTNG_NAMESPACE::DhtEndpoint toCoreEndpoint(const DhtEndpoint &ep)
{
    return QTNG_NAMESPACE::DhtEndpoint(toCoreAddress(ep.address()), ep.port());
}

DhtEndpoint fromCoreEndpoint(const QTNG_NAMESPACE::DhtEndpoint &ep)
{
    return DhtEndpoint(toQtAddress(ep.address()), ep.port());
}

QTNG_NAMESPACE::DhtNodeInfo toCoreNodeInfo(const DhtNodeInfo &info)
{
    return QTNG_NAMESPACE::DhtNodeInfo(toCoreNodeId(info.id()), toCoreEndpoint(info.endpoint()));
}

DhtNodeInfo fromCoreNodeInfo(const QTNG_NAMESPACE::DhtNodeInfo &info)
{
    return DhtNodeInfo(fromCoreNodeId(info.id()), fromCoreEndpoint(info.endpoint()));
}

QTNG_NAMESPACE::DhtPeer toCorePeer(const DhtPeer &peer)
{
    return QTNG_NAMESPACE::DhtPeer(toCoreAddress(peer.address()), peer.port());
}

DhtPeer fromCorePeer(const QTNG_NAMESPACE::DhtPeer &peer)
{
    return DhtPeer(toQtAddress(peer.address()), peer.port());
}

QList<DhtNodeInfo> fromCoreNodeList(const vector<QTNG_NAMESPACE::DhtNodeInfo> &nodes)
{
    QList<DhtNodeInfo> result;
    result.reserve(static_cast<int>(nodes.size()));
    for (const QTNG_NAMESPACE::DhtNodeInfo &node : nodes) {
        result.append(fromCoreNodeInfo(node));
    }
    return result;
}

vector<QTNG_NAMESPACE::DhtNodeInfo> toCoreNodeList(const QList<DhtNodeInfo> &nodes)
{
    vector<QTNG_NAMESPACE::DhtNodeInfo> result;
    result.reserve(static_cast<size_t>(nodes.size()));
    for (const DhtNodeInfo &node : nodes) {
        result.push_back(toCoreNodeInfo(node));
    }
    return result;
}

QList<DhtPeer> fromCorePeerList(const vector<QTNG_NAMESPACE::DhtPeer> &peers)
{
    QList<DhtPeer> result;
    result.reserve(static_cast<int>(peers.size()));
    for (const QTNG_NAMESPACE::DhtPeer &peer : peers) {
        result.append(fromCorePeer(peer));
    }
    return result;
}

class QtDhtStoreAdapter : public QTNG_NAMESPACE::DhtStore
{
public:
    explicit QtDhtStoreAdapter(const QSharedPointer< ::qtng::DhtStore> &store)
        : qtStore(store)
    {
    }

    bool loadMeta(QTNG_NAMESPACE::NodeId *id, string *tokenSecret) override
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

    bool saveMeta(const QTNG_NAMESPACE::NodeId &id, const char *tokenSecret, size_t size) override
    {
        return qtStore->saveMeta(fromCoreNodeId(id), QByteArray(tokenSecret, static_cast<int>(size)));
    }

    vector<QTNG_NAMESPACE::DhtNodeInfo> loadNodes() override
    {
        return toCoreNodeList(qtStore->loadNodes());
    }

    bool saveNodes(const vector<QTNG_NAMESPACE::DhtNodeInfo> &nodes) override
    {
        return qtStore->saveNodes(fromCoreNodeList(nodes));
    }

    vector<QTNG_NAMESPACE::DhtStore::StoredPeer> loadPeers(const QTNG_NAMESPACE::NodeId &infoHash) override
    {
        const QList< ::qtng::DhtStore::StoredPeer> peers = qtStore->loadPeers(fromCoreNodeId(infoHash));
        vector<QTNG_NAMESPACE::DhtStore::StoredPeer> result;
        result.reserve(static_cast<size_t>(peers.size()));
        for (const ::qtng::DhtStore::StoredPeer &peer : peers) {
            QTNG_NAMESPACE::DhtStore::StoredPeer item;
            item.setPeer(toCorePeer(peer.peer()));
            item.setExpireUnix(peer.expireUnix());
            result.push_back(item);
        }
        return result;
    }

    bool putPeer(const QTNG_NAMESPACE::NodeId &infoHash, const QTNG_NAMESPACE::DhtPeer &peer, int64_t expireUnix) override
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

shared_ptr<QTNG_NAMESPACE::DhtStore> toCoreStore(const QSharedPointer< ::qtng::DhtStore> &store)
{
    if (!store) {
        return shared_ptr<QTNG_NAMESPACE::DhtStore>();
    }
    return make_shared<QtDhtStoreAdapter>(store);
}

}  // namespace

class NodeIdPrivate : public QSharedData
{
public:
    QTNG_NAMESPACE::NodeId core;
};

QTNG_NAMESPACE::NodeId NodeIdCoreBridge::core(const NodeId &id)
{
    return id.d ? id.d->core : QTNG_NAMESPACE::NodeId();
}

void NodeIdCoreBridge::setCore(NodeId &id, const QTNG_NAMESPACE::NodeId &core)
{
    if (!id.d) {
        id.d = new NodeIdPrivate;
    }
    id.d->core = core;
}

namespace {

QTNG_NAMESPACE::NodeId toCoreNodeId(const NodeId &id)
{
    return NodeIdCoreBridge::core(id);
}

NodeId fromCoreNodeId(const QTNG_NAMESPACE::NodeId &id)
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
    d->core = QTNG_NAMESPACE::NodeId::fromBytes(toStdString(raw20));
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
    return fromCoreNodeId(QTNG_NAMESPACE::NodeId::random());
}

NodeId NodeId::fromBytes(const QByteArray &raw20)
{
    return fromCoreNodeId(QTNG_NAMESPACE::NodeId::fromBytes(toStdString(raw20)));
}

NodeId NodeId::fromHex(const QString &hex40)
{
    return fromCoreNodeId(QTNG_NAMESPACE::NodeId::fromHex(toStdString(hex40)));
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
    return toQByteArray(QTNG_NAMESPACE::encodeCompactNodes(toCoreNodeList(nodes)));
}

QByteArray encodeCompactNodes6(const QList<DhtNodeInfo> &nodes)
{
    return toQByteArray(QTNG_NAMESPACE::encodeCompactNodes6(toCoreNodeList(nodes)));
}

QList<DhtNodeInfo> decodeCompactNodes(const QByteArray &data)
{
    return fromCoreNodeList(QTNG_NAMESPACE::decodeCompactNodes(toStdString(data)));
}

QList<DhtNodeInfo> decodeCompactNodes6(const QByteArray &data)
{
    return fromCoreNodeList(QTNG_NAMESPACE::decodeCompactNodes6(toStdString(data)));
}

QByteArray encodeCompactPeers(const QList<DhtPeer> &peers)
{
    vector<QTNG_NAMESPACE::DhtPeer> corePeers;
    corePeers.reserve(static_cast<size_t>(peers.size()));
    for (const DhtPeer &peer : peers) {
        corePeers.push_back(toCorePeer(peer));
    }
    return toQByteArray(QTNG_NAMESPACE::encodeCompactPeers(corePeers));
}

QByteArray encodeCompactPeers6(const QList<DhtPeer> &peers)
{
    vector<QTNG_NAMESPACE::DhtPeer> corePeers;
    corePeers.reserve(static_cast<size_t>(peers.size()));
    for (const DhtPeer &peer : peers) {
        corePeers.push_back(toCorePeer(peer));
    }
    return toQByteArray(QTNG_NAMESPACE::encodeCompactPeers6(corePeers));
}

QList<DhtPeer> decodeCompactPeers(const QByteArray &data)
{
    return fromCorePeerList(QTNG_NAMESPACE::decodeCompactPeers(toStdString(data)));
}

QList<DhtPeer> decodeCompactPeers6(const QByteArray &data)
{
    return fromCorePeerList(QTNG_NAMESPACE::decodeCompactPeers6(toStdString(data)));
}

class MemoryDhtStorePrivate
{
public:
    MemoryDhtStorePrivate()
        : core(make_shared<QTNG_NAMESPACE::MemoryDhtStore>())
    {
    }

    explicit MemoryDhtStorePrivate(const shared_ptr<QTNG_NAMESPACE::MemoryDhtStore> &c)
        : core(c)
    {
    }

    shared_ptr<QTNG_NAMESPACE::MemoryDhtStore> core;
};

MemoryDhtStore::MemoryDhtStore()
    : d_ptr(new MemoryDhtStorePrivate)
{
}

MemoryDhtStore::MemoryDhtStore(MemoryDhtStorePrivate *priv)
    : d_ptr(priv)
{
}

MemoryDhtStore::~MemoryDhtStore()
{
    delete d_ptr;
}

bool MemoryDhtStore::loadMeta(NodeId *id, QByteArray *tokenSecret)
{
    Q_D(MemoryDhtStore);
    QTNG_NAMESPACE::NodeId coreId;
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
    const vector<QTNG_NAMESPACE::DhtStore::StoredPeer> peers = d->core->loadPeers(toCoreNodeId(infoHash));
    QList<DhtStore::StoredPeer> result;
    result.reserve(static_cast<int>(peers.size()));
    for (const QTNG_NAMESPACE::DhtStore::StoredPeer &peer : peers) {
        DhtStore::StoredPeer item;
        item.setPeer(fromCorePeer(peer.peer()));
        item.setExpireUnix(peer.expireUnix());
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
        : core(make_shared<QTNG_NAMESPACE::LmdbDhtStore>(toStdString(dirPath)))
    {
    }

    shared_ptr<QTNG_NAMESPACE::LmdbDhtStore> core;
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
    QTNG_NAMESPACE::NodeId coreId;
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
    const vector<QTNG_NAMESPACE::DhtStore::StoredPeer> peers = d->core->loadPeers(toCoreNodeId(infoHash));
    QList<DhtStore::StoredPeer> result;
    result.reserve(static_cast<int>(peers.size()));
    for (const QTNG_NAMESPACE::DhtStore::StoredPeer &peer : peers) {
        DhtStore::StoredPeer item;
        item.setPeer(fromCorePeer(peer.peer()));
        item.setExpireUnix(peer.expireUnix());
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
        : core(new QTNG_NAMESPACE::DhtNode(toCoreNodeId(id)))
    {
    }

    ~DhtNodePrivate()
    {
        delete core;
    }

    // 以 core DhtNode 的 store 状态为唯一事实来源，把 core store 转回 qt store：
    // - core store 是 QtDhtStoreAdapter（用户显式传入的 qt store 的适配器）→ 取回原始 qt store；
    // - core store 是 core 默认创建的 MemoryDhtStore → 包装为 qt 版 MemoryDhtStore；
    // - 其它情况 → 返回空。
    static QSharedPointer<DhtStore> qtStoreFromCore(const shared_ptr<QTNG_NAMESPACE::DhtStore> &coreStore);

    QTNG_NAMESPACE::DhtNode *core;
    // 例外处理：qt store 视图缓存。
    // core 的 DhtNode 是 DhtStore（core 类型）的唯一事实来源；qt binding 的 store() 必须返回
    // QSharedPointer<qtng::DhtStore>，而 core store → qt store 的转换（qtStoreFromCore）每次都要
    // 动态识别 store 的真实类型（QtDhtStoreAdapter / MemoryDhtStore 等）并构造包装，有真实成本。
    // 因为 store 只会在 open() 时被整体替换、之后内容由 core 自身维护，所以把转换结果缓存下来、
    // getter 直接返回。它与"qt binding 不再镜像状态"的总原则不同，是有意保留的例外：
    // 这里缓存的是 core store 的只读视图，唯一的写入路径仍是 core->store()，不存在双向同步。
    QSharedPointer< ::qtng::DhtStore> qtStore;
};

QSharedPointer<DhtStore> DhtNodePrivate::qtStoreFromCore(const shared_ptr<QTNG_NAMESPACE::DhtStore> &coreStore)
{
    if (!coreStore) {
        return QSharedPointer<DhtStore>();
    }
    if (const QtDhtStoreAdapter *adapter = dynamic_cast<const QtDhtStoreAdapter *>(coreStore.get())) {
        return adapter->qtStore;
    }
    const shared_ptr<QTNG_NAMESPACE::MemoryDhtStore> memory =
            dynamic_pointer_cast<QTNG_NAMESPACE::MemoryDhtStore>(coreStore);
    if (memory) {
        return QSharedPointer<MemoryDhtStore>(new MemoryDhtStore(new MemoryDhtStorePrivate(memory)));
    }
    return QSharedPointer<DhtStore>();
}

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
    d->qtStore.clear();
    const bool ok = d->core->open(bindPort, toCoreStore(store),
                                  static_cast<QTNG_NAMESPACE::HostAddress::NetworkLayerProtocol>(proto));
    if (ok) {
        d->qtStore = DhtNodePrivate::qtStoreFromCore(d->core->store());
    }
    return ok;
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
    vector<QTNG_NAMESPACE::DhtEndpoint> coreSeeds;
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
