#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "qtng/bencode.h"
#include "qtng/bt.h"
#include "qtng/md.h"
#include "qtng/private/bt_p.h"

using namespace std;
using namespace qtng;

namespace {

string makeSingleFileTorrent(const string &payload, const string &announce = "http://127.0.0.1:6969/announce")
{
    const int32_t pieceLength = 16384;
    string pieces;
    for (size_t off = 0; off < payload.size(); off += static_cast<size_t>(pieceLength)) {
        size_t len = min(static_cast<size_t>(pieceLength), payload.size() - off);
        pieces += MessageDigest::digest(payload.substr(off, len), MessageDigest::Sha1);
    }

    map<string, Bencode> info;
    info["name"] = Bencode("hello.txt");
    info["piece length"] = Bencode(static_cast<int64_t>(pieceLength));
    info["pieces"] = Bencode(pieces);
    info["length"] = Bencode(static_cast<int64_t>(payload.size()));

    map<string, Bencode> root;
    root["announce"] = Bencode(announce);
    root["info"] = Bencode(info);
    return Bencode(root).encode();
}

}  // namespace

TEST_CASE("bt percent encode", "[bt]")
{
    string raw;
    raw.push_back(static_cast<char>(0x12));
    raw.push_back(static_cast<char>(0x34));
    raw.push_back(static_cast<char>(0x56));
    raw.push_back(static_cast<char>(0x78));
    raw.push_back(static_cast<char>(0x9a));
    string enc = btPercentEncode(raw);
    REQUIRE(enc == "%124Vx%9A");
}

TEST_CASE("bt handshake encode/decode", "[bt]")
{
    InfoHash hash = InfoHash::fromBytes(string(20, 'a'));
    string peerId(20, 'b');
    string hs = btEncodeHandshake(hash, peerId);
    REQUIRE(hs.size() == 68);

    InfoHash outHash;
    string outId;
    string reserved;
    REQUIRE(btDecodeHandshake(hs, &outHash, &outId, &reserved));
    REQUIRE(outHash == hash);
    REQUIRE(outId == peerId);
    REQUIRE(reserved.size() == 8);
    REQUIRE(btHandshakeSupportsExtension(reserved));
}

TEST_CASE("bt base32 decode infohash", "[bt]")
{
    // 20 zero bytes → base32 "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    string raw = btBase32Decode("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    REQUIRE(raw.size() == 20);
    REQUIRE(raw == string(20, '\0'));
}

TEST_CASE("bt message framing", "[bt]")
{
    string msg = btEncodeMessage(4, string("\x00\x00\x00\x01", 4));
    REQUIRE(msg.size() == 9);
    REQUIRE(static_cast<unsigned char>(msg[4]) == 4);
}

TEST_CASE("magnet parse hex and trackers", "[bt]")
{
    string uri = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"
                 "&dn=Test%20Name"
                 "&tr=http%3A%2F%2Ftracker.example%2Fannounce"
                 "&tr=udp%3A%2F%2Ftracker2.example%3A80"
                 "&x.pe=1.2.3.4:6881";
    MagnetLink m = MagnetLink::parse(uri);
    REQUIRE(m.isValid());
    REQUIRE(m.infoHash().toHex() == "0123456789abcdef0123456789abcdef01234567");
    REQUIRE(m.displayName() == "Test Name");
    REQUIRE(m.trackers().size() == 2);
    REQUIRE(m.trackers()[0] == "http://tracker.example/announce");
    REQUIRE(m.trackers()[1] == "udp://tracker2.example:80");
    REQUIRE(m.peers().size() == 1);
    REQUIRE(m.peers()[0].host() == "1.2.3.4");
    REQUIRE(m.peers()[0].port() == 6881);
}

TEST_CASE("magnet parse base32", "[bt]")
{
    string uri = "magnet:?xt=urn:btih:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    MagnetLink m = MagnetLink::parse(uri);
    REQUIRE(m.isValid());
    REQUIRE(m.infoHash().toBytes() == string(20, '\0'));
}

TEST_CASE("magnet reject invalid", "[bt]")
{
    REQUIRE_FALSE(MagnetLink::parse("http://example").isValid());
    REQUIRE_FALSE(MagnetLink::parse("magnet:?dn=only-name").isValid());
}

TEST_CASE("torrent meta from info dict", "[bt]")
{
    string payload(1000, 'x');
    string torrent = makeSingleFileTorrent(payload);
    string infoRaw = btExtractInfoDict(torrent);
    REQUIRE(!infoRaw.empty());
    TorrentMeta meta = TorrentMeta::fromInfoDict(infoRaw, {"http://tr.example/announce"});
    REQUIRE(meta.isValid());
    REQUIRE(meta.name() == "hello.txt");
    REQUIRE(meta.totalSize() == 1000);
    REQUIRE(meta.infoDict() == infoRaw);
    REQUIRE(meta.trackers().size() == 1);
}

TEST_CASE("bt compact peer list", "[bt]")
{
    string compact;
    // 1.2.3.4:6881
    compact.push_back(1);
    compact.push_back(2);
    compact.push_back(3);
    compact.push_back(4);
    compact.push_back(static_cast<char>(0x1a));
    compact.push_back(static_cast<char>(0xe1));
    vector<BtPeerAddr> peers = btDecodeCompactPeerList(compact, false);
    REQUIRE(peers.size() == 1);
    REQUIRE(peers[0].port() == 6881);
    REQUIRE(peers[0].address.toString() == "1.2.3.4");
}

TEST_CASE("torrent meta parse single file", "[bt]")
{
    string payload(1000, 'x');
    string torrent = makeSingleFileTorrent(payload);
    TorrentMeta meta = TorrentMeta::fromBytes(torrent);
    REQUIRE(meta.isValid());
    REQUIRE(meta.name() == "hello.txt");
    REQUIRE(meta.totalSize() == 1000);
    REQUIRE(meta.pieceCount() == 1);
    REQUIRE(meta.files().size() == 1);
    REQUIRE(meta.trackers().size() == 1);
    REQUIRE(meta.infoHash().isValid());

    string infoRaw = btExtractInfoDict(torrent);
    REQUIRE(!infoRaw.empty());
    REQUIRE(MessageDigest::digest(infoRaw, MessageDigest::Sha1) == meta.infoHash().toBytes());
}

TEST_CASE("piece storage write and verify", "[bt]")
{
    string payload = "abcdefghijklmnopqrstuvwxyz0123456789";
    // tiny piece length via custom torrent
    map<string, Bencode> info;
    info["name"] = Bencode("tiny.bin");
    info["piece length"] = Bencode(static_cast<int64_t>(16));
    string pieces;
    for (size_t off = 0; off < payload.size(); off += 16) {
        size_t len = min<size_t>(16, payload.size() - off);
        pieces += MessageDigest::digest(payload.substr(off, len), MessageDigest::Sha1);
    }
    info["pieces"] = Bencode(pieces);
    info["length"] = Bencode(static_cast<int64_t>(payload.size()));
    map<string, Bencode> root;
    root["announce"] = Bencode("http://example/announce");
    root["info"] = Bencode(info);
    TorrentMeta meta = TorrentMeta::fromBytes(Bencode(root).encode());
    REQUIRE(meta.isValid());

    PieceStorage storage;
    REQUIRE(storage.open(meta, "build-bt/bt-test-dl"));
    for (int i = 0; i < meta.pieceCount(); ++i) {
        int32_t plen = storage.pieceLength(i);
        string piece = payload.substr(static_cast<size_t>(i) * 16, static_cast<size_t>(plen));
        REQUIRE(storage.commitPiece(i, piece));
    }
    REQUIRE(storage.bytesLeft() == 0);
    storage.close();
}
