#ifndef QTNG_KADEMLIA_H
#define QTNG_KADEMLIA_H

#include <QtCore/qbytearray.h>
#include <QtCore/qlist.h>
#include <QtCore/qsharedpointer.h>
#include <QtCore/qstring.h>
#include "hostaddress.h"
#include "config.h"

QTNETWORKNG_NAMESPACE_BEGIN

class NodeIdPrivate;
class NodeId
{
public:
    static const int Size = 20;

    NodeId();
    explicit NodeId(const QByteArray &raw20);
    NodeId(const NodeId &other);
    NodeId &operator=(const NodeId &other);
    ~NodeId();

    static NodeId random();
    static NodeId fromBytes(const QByteArray &raw20);
    static NodeId fromHex(const QString &hex40);

    bool isValid() const;
    QByteArray toBytes() const;
    QString toHex() const;

    NodeId operator^(const NodeId &other) const;
    int commonPrefixLength(const NodeId &other) const;
    int bucketIndex(const NodeId &other) const;

    bool operator==(const NodeId &other) const;
    bool operator!=(const NodeId &other) const;
    bool operator<(const NodeId &other) const;

private:
    friend class DhtNodePrivate;
    friend struct NodeIdCoreBridge;
    QSharedDataPointer<NodeIdPrivate> d;
};

struct DhtEndpoint
{
    DhtEndpoint()
        : m_port(0)
    {
    }
    DhtEndpoint(const HostAddress &addr, quint16 p)
        : m_address(addr)
        , m_port(p)
    {
    }

    bool isValid() const { return !m_address.isNull() && m_port != 0; }

    HostAddress address() const { return m_address; }
    void setAddress(const HostAddress &addr) { m_address = addr; }
    quint16 port() const { return m_port; }
    void setPort(quint16 p) { m_port = p; }
private:
    HostAddress m_address;
    quint16 m_port;
};

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

struct DhtPeer
{
    DhtPeer()
        : m_port(0)
    {
    }
    DhtPeer(const HostAddress &addr, quint16 p)
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
    quint16 port() const { return m_port; }
    void setPort(quint16 p) { m_port = p; }
private:
    HostAddress m_address;
    quint16 m_port;
};

QByteArray encodeCompactNodes(const QList<DhtNodeInfo> &nodes);
QByteArray encodeCompactNodes6(const QList<DhtNodeInfo> &nodes);
QList<DhtNodeInfo> decodeCompactNodes(const QByteArray &data);
QList<DhtNodeInfo> decodeCompactNodes6(const QByteArray &data);
QByteArray encodeCompactPeers(const QList<DhtPeer> &peers);
QByteArray encodeCompactPeers6(const QList<DhtPeer> &peers);
QList<DhtPeer> decodeCompactPeers(const QByteArray &data);
QList<DhtPeer> decodeCompactPeers6(const QByteArray &data);

class DhtStore
{
public:
    struct StoredPeer {
        StoredPeer() { }

        DhtPeer peer() const { return m_peer; }
        void setPeer(const DhtPeer &p) { m_peer = p; }
        qint64 expireUnix() const { return m_expireUnix; }
        void setExpireUnix(qint64 t) { m_expireUnix = t; }
    private:
        DhtPeer m_peer;
        qint64 m_expireUnix;
    };

    virtual ~DhtStore() { }

    virtual bool loadMeta(NodeId *id, QByteArray *tokenSecret) = 0;
    virtual bool saveMeta(const NodeId &id, const QByteArray &tokenSecret) = 0;

    virtual QList<DhtNodeInfo> loadNodes() = 0;
    virtual bool saveNodes(const QList<DhtNodeInfo> &nodes) = 0;

    virtual QList<StoredPeer> loadPeers(const NodeId &infoHash) = 0;
    virtual bool putPeer(const NodeId &infoHash, const DhtPeer &peer, qint64 expireUnix) = 0;
    virtual bool removeExpiredPeers(qint64 nowUnix) = 0;

    virtual QString errorString() const = 0;
};

class MemoryDhtStorePrivate;
class MemoryDhtStore : public DhtStore
{
public:
    MemoryDhtStore();
    ~MemoryDhtStore() override;

    bool loadMeta(NodeId *id, QByteArray *tokenSecret) override;
    bool saveMeta(const NodeId &id, const QByteArray &tokenSecret) override;
    QList<DhtNodeInfo> loadNodes() override;
    bool saveNodes(const QList<DhtNodeInfo> &nodes) override;
    QList<StoredPeer> loadPeers(const NodeId &infoHash) override;
    bool putPeer(const NodeId &infoHash, const DhtPeer &peer, qint64 expireUnix) override;
    bool removeExpiredPeers(qint64 nowUnix) override;
    QString errorString() const override;

private:
    friend class DhtNode;
    friend class DhtNodePrivate;
    explicit MemoryDhtStore(MemoryDhtStorePrivate *priv);
    MemoryDhtStorePrivate * const d_ptr;
    Q_DECLARE_PRIVATE(MemoryDhtStore)
    Q_DISABLE_COPY(MemoryDhtStore)
};

class LmdbDhtStorePrivate;
class LmdbDhtStore : public DhtStore
{
public:
    explicit LmdbDhtStore(const QString &dirPath);
    ~LmdbDhtStore() override;

    bool isOpen() const;
    bool loadMeta(NodeId *id, QByteArray *tokenSecret) override;
    bool saveMeta(const NodeId &id, const QByteArray &tokenSecret) override;
    QList<DhtNodeInfo> loadNodes() override;
    bool saveNodes(const QList<DhtNodeInfo> &nodes) override;
    QList<StoredPeer> loadPeers(const NodeId &infoHash) override;
    bool putPeer(const NodeId &infoHash, const DhtPeer &peer, qint64 expireUnix) override;
    bool removeExpiredPeers(qint64 nowUnix) override;
    QString errorString() const override;

private:
    LmdbDhtStorePrivate * const d_ptr;
    Q_DECLARE_PRIVATE(LmdbDhtStore)
    Q_DISABLE_COPY(LmdbDhtStore)
};

class DhtNodePrivate;
class DhtNode
{
public:
    explicit DhtNode(const NodeId &id = NodeId());
    ~DhtNode();

    bool open(quint16 bindPort, QSharedPointer<DhtStore> store = QSharedPointer<DhtStore>(),
              HostAddress::NetworkLayerProtocol proto = HostAddress::IPv4Protocol);
    void close();
    bool isOpen() const;

    NodeId id() const;
    quint16 localPort() const;
    QSharedPointer<DhtStore> store() const;

    bool bootstrap(const QList<DhtEndpoint> &seeds);
    QList<DhtNodeInfo> findNode(const NodeId &target);
    QList<DhtPeer> getPeers(const NodeId &infoHash);
    bool announcePeer(const NodeId &infoHash, quint16 peerPort, const QByteArray &token = QByteArray());

    int routingTableSize() const;
    QString errorString() const;

private:
    DhtNodePrivate * const d_ptr;
    Q_DECLARE_PRIVATE(DhtNode)
    Q_DISABLE_COPY(DhtNode)
};

QTNETWORKNG_NAMESPACE_END

#endif  // QTNG_KADEMLIA_H
