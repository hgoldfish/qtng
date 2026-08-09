#ifndef QTNG_BT_P_H
#define QTNG_BT_P_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "qtng/bt.h"
#include "qtng/coroutine_utils.h"
#include "qtng/http.h"
#include "qtng/locks.h"
#include "qtng/socket.h"
#include "qtng/socket_utils.h"
#include "qtng/udp.h"

namespace qtng {

static const char kBtProtocol[] = "BitTorrent protocol";
static const std::int32_t kBtBlockSize = 16 * 1024;
static const float kBtPeerConnectTimeout = 12.0f;
static const float kBtTrackerIntervalDefault = 1800.0f;

struct BtPeerAddr
{
    HostAddress address;
    std::uint16_t port;

    BtPeerAddr()
        : port(0)
    {
    }
    BtPeerAddr(const HostAddress &a, std::uint16_t p)
        : address(a)
        , port(p)
    {
    }

    bool isValid() const { return !address.isNull() && port != 0; }
    bool operator<(const BtPeerAddr &o) const
    {
        if (address.toString() != o.address.toString()) {
            return address.toString() < o.address.toString();
        }
        return port < o.port;
    }
    bool operator==(const BtPeerAddr &o) const { return address == o.address && port == o.port; }
};

class TorrentMetaPrivate
{
public:
    bool valid;
    InfoHash infoHash;
    std::string name;
    std::int64_t totalSize;
    std::int32_t pieceLength;
    std::vector<std::string> pieceHashes;  // each 20 bytes
    std::vector<TorrentFileInfo> files;
    std::vector<std::string> trackers;
    std::string infoDict;  // raw bencoded info dictionary
    std::string errorString;

    TorrentMetaPrivate()
        : valid(false)
        , totalSize(0)
        , pieceLength(0)
    {
    }
};

class MagnetLinkPrivate
{
public:
    bool valid;
    InfoHash infoHash;
    std::string displayName;
    std::vector<std::string> trackers;
    std::vector<MagnetPeerHint> peers;
    std::string errorString;

    MagnetLinkPrivate()
        : valid(false)
    {
    }
};

class PieceStorage
{
public:
    PieceStorage();
    ~PieceStorage();

    bool open(const TorrentMeta &meta, const std::string &downloadDir);
    void close();
    bool isOpen() const { return m_open; }

    std::int32_t pieceLength(std::int32_t index) const;
    std::int32_t pieceCount() const { return m_pieceCount; }
    bool hasPiece(std::int32_t index) const;
    std::string bitfield() const;
    std::int64_t bytesLeft() const;
    std::int64_t bytesHave() const;

    bool writeBlock(std::int32_t piece, std::int32_t offset, const std::string &data);
    bool readBlock(std::int32_t piece, std::int32_t offset, std::int32_t length, std::string *out);
    bool commitPiece(std::int32_t piece, const std::string &data);
    bool checkAll();

    std::string errorString() const { return m_error; }
private:
    struct FileMap {
        std::string path;
        std::int64_t offset;  // global offset
        std::int64_t length;
        int fd;
        FileMap()
            : offset(0)
            , length(0)
            , fd(-1)
        {
        }
    };

    bool mapPiece(std::int32_t piece, std::int32_t offset, std::int32_t length,
                  std::vector<std::pair<int, std::pair<std::int64_t, std::int32_t>>> *spans) const;
    bool ioWrite(int fd, std::int64_t fileOff, const char *data, std::int32_t len);
    bool ioRead(int fd, std::int64_t fileOff, char *data, std::int32_t len);

    bool m_open;
    TorrentMeta m_meta;
    std::string m_root;
    std::vector<FileMap> m_files;
    std::vector<bool> m_have;
    std::int32_t m_pieceCount;
    std::int64_t m_haveBytes;
    std::string m_error;
};

struct BlockRequest
{
    std::int32_t piece;
    std::int32_t offset;
    std::int32_t length;

    BlockRequest()
        : piece(0)
        , offset(0)
        , length(0)
    {
    }
    BlockRequest(std::int32_t p, std::int32_t o, std::int32_t l)
        : piece(p)
        , offset(o)
        , length(l)
    {
    }

    bool operator<(const BlockRequest &o) const
    {
        if (piece != o.piece) {
            return piece < o.piece;
        }
        if (offset != o.offset) {
            return offset < o.offset;
        }
        return length < o.length;
    }
};

class PiecePicker
{
public:
    void reset(std::int32_t pieceCount, PieceStorage *storage);
    void addPeerBitfield(const std::string &bitfield);
    void addPeerHave(std::int32_t piece);
    void removePeerBitfield(const std::string &bitfield);
    bool nextRequest(const std::string &peerBitfield, bool endgame, BlockRequest *out);
    void markRequested(const BlockRequest &req);
    void markReceived(const BlockRequest &req);
    void markFailed(const BlockRequest &req);
    bool hasBlock(std::int32_t piece, std::int32_t offset) const;
    bool isPieceDataComplete(std::int32_t piece) const;
    void abandonPiece(std::int32_t piece);
    std::string *pieceBuffer(std::int32_t piece);
private:
    PieceStorage *m_storage;
    std::int32_t m_pieceCount;
    std::vector<int> m_availability;
    std::set<BlockRequest> m_inflight;
    std::map<std::int32_t, std::string> m_buffers;
    std::map<std::int32_t, std::set<std::int32_t>> m_gotOffsets;
};

class TorrentHandlePrivate : public std::enable_shared_from_this<TorrentHandlePrivate>
{
public:
    TorrentHandlePrivate();
    ~TorrentHandlePrivate();

    TorrentStats statsSnapshot() const;
    void setState(TorrentStats::State st, const std::string &err = std::string());
    void notifyProgress();
    InfoHash effectiveInfoHash() const;
    std::vector<std::string> effectiveTrackers() const;

    std::weak_ptr<class TorrentSessionPrivate> session;
    TorrentMeta meta;
    PieceStorage storage;
    PiecePicker picker;
    Event finishedEvent;
    Event removedEvent;
    Event metadataEvent;
    mutable std::mutex mutex_;
    TorrentStats stats;
    std::function<void(const TorrentStats &)> progressCallback;
    std::set<BtPeerAddr> peers;
    std::set<BtPeerAddr> connectedPeers;
    std::int64_t lastDownloaded;
    std::int64_t lastUploaded;
    double lastRateSampleSecs;
    bool paused;
    bool removed;
    bool started;
    bool needsMetadata;
    InfoHash magnetInfoHash;
    std::vector<std::string> magnetTrackers;
    std::string magnetDisplayName;
    std::string infoDictRaw;
    std::int64_t metadataSize;
    std::string metadataBuf;
    std::vector<bool> metadataHave;
};

class TorrentSessionPrivate
{
public:
    explicit TorrentSessionPrivate(TorrentSession *q, std::shared_ptr<DhtNode> node = std::shared_ptr<DhtNode>());
    ~TorrentSessionPrivate();

    void start();
    void stop();
    TorrentHandle addTorrent(const TorrentMeta &meta);
    TorrentHandle addMagnet(const MagnetLink &magnet);

    void ensurePeerId();
    std::uint16_t effectiveListenPort() const;

    void acceptTcpLoop();
    void acceptUtpLoop();
    void maintainTorrent(std::shared_ptr<TorrentHandlePrivate> h);
    void trackerLoop(std::shared_ptr<TorrentHandlePrivate> h);
    void dhtLoop(std::shared_ptr<TorrentHandlePrivate> h);
    void connectPeer(std::shared_ptr<TorrentHandlePrivate> h, BtPeerAddr addr, bool preferUtp);
    void runPeer(std::shared_ptr<TorrentHandlePrivate> h, std::shared_ptr<SocketLike> sock, bool incoming,
                 bool handshakeDone = false);
    void runMetadataPeer(std::shared_ptr<TorrentHandlePrivate> h, std::shared_ptr<SocketLike> sock, bool incoming,
                         bool handshakeDone = false);
    bool exchangeExtendedHandshake(std::shared_ptr<TorrentHandlePrivate> h, std::shared_ptr<SocketLike> sock,
                                    std::uint8_t *peerUtMetadata, std::int64_t *peerMetadataSize);
    bool fetchMetadataFromPeer(std::shared_ptr<TorrentHandlePrivate> h, std::shared_ptr<SocketLike> sock,
                               std::uint8_t peerUtMetadata, std::int64_t peerMetadataSize);
    bool tryCommitMetadata(std::shared_ptr<TorrentHandlePrivate> h);
    void serveMetadataRequest(std::shared_ptr<TorrentHandlePrivate> h, std::shared_ptr<SocketLike> sock,
                              std::uint8_t peerUtMetadata, std::int32_t piece);
    void addPeers(std::shared_ptr<TorrentHandlePrivate> h, const std::vector<BtPeerAddr> &list);

    std::vector<BtPeerAddr> announceHttp(std::shared_ptr<TorrentHandlePrivate> h, const std::string &url,
                                         const std::string &event);
    std::vector<BtPeerAddr> announceUdp(std::shared_ptr<TorrentHandlePrivate> h, const std::string &url,
                                        const std::string &event);

    TorrentSession *q_ptr;
    std::string downloadDir;
    std::uint16_t listenPort;
    std::string peerId;
    int maxPeers;
    bool dhtEnabled;
    bool utpEnabled;
    bool started;
    std::string errorString;
    std::vector<DhtEndpoint> dhtBootstrap;
    std::shared_ptr<DhtNode> dht;
    bool ownDht;
    std::shared_ptr<Socket> tcpListener;
    std::shared_ptr<UtpSocket> utpListener;
    CoroutineGroup operations;
    std::vector<std::shared_ptr<TorrentHandlePrivate>> torrents;
    std::mutex torrentsMutex;
    HttpSession http;
};

// Wire helpers (also used by unit tests via friendship / linkage in same TU).
std::string btEncodeHandshake(const InfoHash &infoHash, const std::string &peerId);
bool btDecodeHandshake(const std::string &data, InfoHash *infoHash, std::string *peerId, std::string *reserved);
bool btHandshakeSupportsExtension(const std::string &reserved);
std::string btEncodeMessage(std::uint8_t id, const std::string &payload = std::string());
std::string btEncodeExtended(std::uint8_t extId, const std::string &payload);
bool btReadMessage(std::shared_ptr<SocketLike> sock, std::uint8_t *id, std::string *payload, float timeoutSecs);
std::string btPercentEncode(const std::string &raw);
std::string btBase32Decode(const std::string &in);  // RFC 4648; empty on failure
std::string btExtractInfoDict(const std::string &torrentBytes);
std::vector<BtPeerAddr> btDecodeCompactPeerList(const std::string &data, bool ipv6);

constexpr std::uint8_t kBtExtMessageId = 20;
constexpr std::uint8_t kBtExtHandshakeId = 0;
constexpr std::uint8_t kBtLocalUtMetadataId = 1;
constexpr std::int32_t kBtMetadataPieceSize = 16 * 1024;

}  // namespace qtng

#endif  // QTNG_BT_P_H
