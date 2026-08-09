#ifndef QTNG_BT_H
#define QTNG_BT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qtng/kademlia.h"
#include "qtng/utils/platform.h"

namespace qtng {

// 20-byte SHA-1 infohash; same layout as BitTorrent DHT node ids.
using InfoHash = NodeId;

struct TorrentFileInfo
{
    std::string path;
    std::int64_t length;

    TorrentFileInfo()
        : length(0)
    {
    }
    TorrentFileInfo(const std::string &p, std::int64_t len)
        : path(p)
        , length(len)
    {
    }
};

// Optional peer address from magnet ``x.pe`` (BEP-9).
struct MagnetPeerHint
{
    std::string host;
    std::uint16_t port;

    MagnetPeerHint()
        : port(0)
    {
    }
    MagnetPeerHint(const std::string &h, std::uint16_t p)
        : host(h)
        , port(p)
    {
    }

    bool isValid() const { return !host.empty() && port != 0; }
};

class MagnetLinkPrivate;
// magnet:?xt=urn:btih:<info-hash>&dn=<name>&tr=<tracker>&x.pe=<peer> (BEP-9).
// v1 infohash only (40-char hex or 32-char base32). ``urn:btmh`` (v2) is ignored.
class MagnetLink
{
public:
    MagnetLink();
    MagnetLink(const MagnetLink &other);
    MagnetLink &operator=(const MagnetLink &other);
    ~MagnetLink();

    static MagnetLink parse(const std::string &uri);

    bool isValid() const;
    InfoHash infoHash() const;
    std::string displayName() const;
    std::vector<std::string> trackers() const;
    std::vector<MagnetPeerHint> peers() const;
    std::string errorString() const;
private:
    std::shared_ptr<MagnetLinkPrivate> d;
};

class TorrentMetaPrivate;
class TorrentMeta
{
public:
    TorrentMeta();
    TorrentMeta(const TorrentMeta &other);
    TorrentMeta &operator=(const TorrentMeta &other);
    ~TorrentMeta();

    static TorrentMeta fromFile(const std::string &path);
    static TorrentMeta fromBytes(const std::string &data);
    // Parse a raw bencoded info dictionary (as transferred by BEP-9 ut_metadata).
    static TorrentMeta fromInfoDict(const std::string &infoDict,
                                    const std::vector<std::string> &trackers = std::vector<std::string>());

    bool isValid() const;
    InfoHash infoHash() const;
    std::string name() const;
    std::int64_t totalSize() const;
    std::int32_t pieceLength() const;
    std::int32_t pieceCount() const;
    std::string pieceHash(std::int32_t index) const;  // raw 20 bytes
    std::vector<TorrentFileInfo> files() const;
    std::vector<std::string> trackers() const;
    // Exact bencoded info dict bytes (empty if unavailable); used for BEP-9 serving.
    std::string infoDict() const;
    std::string errorString() const;
private:
    std::shared_ptr<TorrentMetaPrivate> d;
};

struct TorrentStats
{
    enum State { Stopped = 0, Checking, Metadata, Downloading, Seeding, Error };

    std::int64_t downloaded;
    std::int64_t uploaded;
    std::int64_t left;
    double progress;  // 0..1
    double downloadRate;
    double uploadRate;
    int peersConnected;
    int peersTotal;
    State state;
    std::string errorString;

    TorrentStats()
        : downloaded(0)
        , uploaded(0)
        , left(0)
        , progress(0.0)
        , downloadRate(0.0)
        , uploadRate(0.0)
        , peersConnected(0)
        , peersTotal(0)
        , state(Stopped)
    {
    }
};

class TorrentHandlePrivate;
class TorrentHandle
{
public:
    TorrentHandle();
    TorrentHandle(const TorrentHandle &other);
    TorrentHandle &operator=(const TorrentHandle &other);
    ~TorrentHandle();

    bool isValid() const;
    InfoHash infoHash() const;
    TorrentMeta meta() const;
    TorrentStats stats() const;
    bool isFinished() const;

    // Block until finished, error, removed, or timeout (seconds; <0 = forever).
    bool wait(float timeout = -1.0f);
    void pause();
    void resume();
    void remove(bool deleteFiles = false);
    void setProgressCallback(std::function<void(const TorrentStats &)> callback);
private:
    friend class TorrentSession;
    friend class TorrentSessionPrivate;
    explicit TorrentHandle(std::shared_ptr<TorrentHandlePrivate> d);
    std::shared_ptr<TorrentHandlePrivate> d;
};

class TorrentSessionPrivate;
class TorrentSession
{
public:
    // Default: Session creates its own DhtNode on start(). Pass node to share across sessions.
    TorrentSession();
    explicit TorrentSession(std::shared_ptr<DhtNode> node);
    ~TorrentSession();

    void setDownloadDir(const std::string &dir);
    std::string downloadDir() const;
    void setListenPort(std::uint16_t port);
    std::uint16_t listenPort() const;
    void setPeerId(const std::string &peerId20);
    std::string peerId() const;
    void setMaxPeers(int n);
    int maxPeers() const;

    // Inject / replace DHT node (session does not close an injected node on stop()).
    void setDhtNode(std::shared_ptr<DhtNode> node);
    std::shared_ptr<DhtNode> dhtNode() const;
    void setDhtEnabled(bool on);
    bool dhtEnabled() const;
    void setUtpEnabled(bool on);
    bool utpEnabled() const;
    void setDhtBootstrap(const std::vector<DhtEndpoint> &seeds);

    TorrentHandle addTorrent(const TorrentMeta &meta);
    TorrentHandle addTorrentFile(const std::string &path);
    // Join swarm from a magnet link; fetches info dict via BEP-9 (ut_metadata) then downloads.
    TorrentHandle addMagnet(const MagnetLink &magnet);
    TorrentHandle addMagnetUri(const std::string &uri);

    // Start TCP/µTP listeners, DHT open/bootstrap, and maintenance coroutines.
    void start();
    void stop();
    bool isStarted() const;
    std::string errorString() const;
private:
    TorrentSessionPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(TorrentSession)
    NG_DISABLE_COPY(TorrentSession)
};

}  // namespace qtng

#endif  // QTNG_BT_H
