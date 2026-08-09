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
    HostAddress address;
    quint16 port;

    DhtEndpoint()
        : port(0)
    {
    }
    DhtEndpoint(const HostAddress &addr, quint16 p)
        : address(addr)
        , port(p)
    {
    }

    bool isValid() const { return !address.isNull() && port != 0; }
};

struct DhtNodeInfo
{
    NodeId id;
    DhtEndpoint endpoint;

    DhtNodeInfo() { }
    DhtNodeInfo(const NodeId &nid, const DhtEndpoint &ep)
        : id(nid)
        , endpoint(ep)
    {
    }

    bool isValid() const { return id.isValid() && endpoint.isValid(); }
};

struct DhtPeer
{
    HostAddress address;
    quint16 port;

    DhtPeer()
        : port(0)
    {
    }
    DhtPeer(const HostAddress &addr, quint16 p)
        : address(addr)
        , port(p)
    {
    }

    bool isValid() const { return !address.isNull() && port != 0; }
    bool operator==(const DhtPeer &other) const
    {
        return address == other.address && port == other.port;
    }
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
        DhtPeer peer;
        qint64 expireUnix;
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
