#include "qtng/private/bt_p.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef NG_OS_WIN
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#include "qtng/bencode.h"
#include "qtng/hostaddress.h"
#include "qtng/io_utils.h"
#include "qtng/md.h"
#include "qtng/random.h"
#include "qtng/utils/url.h"

#include <climits>

using namespace std;

namespace qtng {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::uint32_t btReadBe32(const char *p)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 8)
            | static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
}

static void btWriteBe32(char *p, std::uint32_t v)
{
    p[0] = static_cast<char>((v >> 24) & 0xff);
    p[1] = static_cast<char>((v >> 16) & 0xff);
    p[2] = static_cast<char>((v >> 8) & 0xff);
    p[3] = static_cast<char>(v & 0xff);
}

static std::uint64_t btReadBe64(const char *p)
{
    std::uint64_t hi = btReadBe32(p);
    std::uint64_t lo = btReadBe32(p + 4);
    return (hi << 32) | lo;
}

static void btWriteBe64(char *p, std::uint64_t v)
{
    btWriteBe32(p, static_cast<std::uint32_t>(v >> 32));
    btWriteBe32(p + 4, static_cast<std::uint32_t>(v & 0xffffffffu));
}

static std::uint16_t btReadBe16(const char *p)
{
    return static_cast<std::uint16_t>((static_cast<unsigned char>(p[0]) << 8)
                                     | static_cast<unsigned char>(p[1]));
}

static void btWriteBe16(char *p, std::uint16_t v)
{
    p[0] = static_cast<char>((v >> 8) & 0xff);
    p[1] = static_cast<char>(v & 0xff);
}

static bool btBitTest(const string &bf, int index)
{
    if (index < 0) {
        return false;
    }
    const size_t byte = static_cast<size_t>(index) / 8;
    if (byte >= bf.size()) {
        return false;
    }
    return (static_cast<unsigned char>(bf[byte]) & (0x80 >> (index % 8))) != 0;
}

static void btBitSet(string *bf, int index)
{
    if (!bf || index < 0) {
        return;
    }
    const size_t byte = static_cast<size_t>(index) / 8;
    if (byte >= bf->size()) {
        bf->resize(byte + 1, '\0');
    }
    (*bf)[byte] = static_cast<char>(static_cast<unsigned char>((*bf)[byte]) | (0x80 >> (index % 8)));
}

string btPercentEncode(const string &raw)
{
    static const char *hex = "0123456789ABCDEF";
    string out;
    out.reserve(raw.size() * 3);
    for (unsigned char c : raw) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
            || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

static bool btSkipBencodeValue(const string &data, size_t *pos)
{
    if (!pos || *pos >= data.size()) {
        return false;
    }
    const char t = data[*pos];
    if (t == 'i') {
        size_t e = data.find('e', *pos + 1);
        if (e == string::npos) {
            return false;
        }
        *pos = e + 1;
        return true;
    }
    if (t >= '0' && t <= '9') {
        size_t colon = data.find(':', *pos);
        if (colon == string::npos) {
            return false;
        }
        int len = 0;
        for (size_t i = *pos; i < colon; ++i) {
            if (data[i] < '0' || data[i] > '9') {
                return false;
            }
            len = len * 10 + (data[i] - '0');
        }
        *pos = colon + 1 + static_cast<size_t>(len);
        return *pos <= data.size();
    }
    if (t == 'l' || t == 'd') {
        ++(*pos);
        while (*pos < data.size() && data[*pos] != 'e') {
            if (t == 'd') {
                if (!btSkipBencodeValue(data, pos)) {
                    return false;
                }
            }
            if (!btSkipBencodeValue(data, pos)) {
                return false;
            }
        }
        if (*pos >= data.size() || data[*pos] != 'e') {
            return false;
        }
        ++(*pos);
        return true;
    }
    return false;
}

string btExtractInfoDict(const string &torrentBytes)
{
    if (torrentBytes.empty() || torrentBytes[0] != 'd') {
        return string();
    }
    size_t pos = 1;
    while (pos < torrentBytes.size() && torrentBytes[pos] != 'e') {
        size_t keyStart = pos;
        if (!btSkipBencodeValue(torrentBytes, &pos)) {
            return string();
        }
        string key = torrentBytes.substr(keyStart, pos - keyStart);
        // key is bencoded string like "4:info"
        string keyText;
        {
            size_t colon = key.find(':');
            if (colon != string::npos) {
                keyText = key.substr(colon + 1);
            }
        }
        size_t valueStart = pos;
        if (!btSkipBencodeValue(torrentBytes, &pos)) {
            return string();
        }
        if (keyText == "info") {
            return torrentBytes.substr(valueStart, pos - valueStart);
        }
    }
    return string();
}

vector<BtPeerAddr> btDecodeCompactPeerList(const string &data, bool ipv6)
{
    vector<BtPeerAddr> out;
    const size_t stride = ipv6 ? 18 : 6;
    if (data.size() % stride != 0) {
        return out;
    }
    for (size_t i = 0; i + stride <= data.size(); i += stride) {
        HostAddress addr;
        std::uint16_t port = 0;
        if (ipv6) {
            addr.setAddress(reinterpret_cast<const std::uint8_t *>(data.data() + i));
            port = btReadBe16(data.data() + i + 16);
        } else {
            const unsigned char *p = reinterpret_cast<const unsigned char *>(data.data() + i);
            std::uint32_t ip = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16)
                    | (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
            addr.setAddress(ip);
            port = btReadBe16(data.data() + i + 4);
        }
        if (!addr.isNull() && port != 0) {
            out.push_back(BtPeerAddr(addr, port));
        }
    }
    return out;
}

string btEncodeHandshake(const InfoHash &infoHash, const string &peerId)
{
    string out;
    out.push_back(static_cast<char>(19));
    out.append(kBtProtocol, 19);
    out.append(8, '\0');
    // BEP-10: extension protocol bit 20 from the right → reserved[5] & 0x10
    out[25] = static_cast<char>(0x10);
    out.append(infoHash.toBytes());
    string pid = peerId;
    if (pid.size() != 20) {
        pid.resize(20, '-');
    }
    out.append(pid);
    return out;
}

bool btDecodeHandshake(const string &data, InfoHash *infoHash, string *peerId, string *reserved)
{
    if (data.size() < 68) {
        return false;
    }
    if (static_cast<unsigned char>(data[0]) != 19) {
        return false;
    }
    if (data.compare(1, 19, kBtProtocol) != 0) {
        return false;
    }
    if (reserved) {
        *reserved = data.substr(20, 8);
    }
    if (infoHash) {
        *infoHash = InfoHash::fromBytes(data.substr(28, 20));
    }
    if (peerId) {
        *peerId = data.substr(48, 20);
    }
    return infoHash && infoHash->isValid();
}

bool btHandshakeSupportsExtension(const string &reserved)
{
    return reserved.size() >= 6 && (static_cast<unsigned char>(reserved[5]) & 0x10) != 0;
}

string btEncodeMessage(std::uint8_t id, const string &payload)
{
    string out(4 + 1 + payload.size(), '\0');
    btWriteBe32(&out[0], static_cast<std::uint32_t>(1 + payload.size()));
    out[4] = static_cast<char>(id);
    if (!payload.empty()) {
        memcpy(&out[5], payload.data(), payload.size());
    }
    return out;
}

string btEncodeExtended(std::uint8_t extId, const string &payload)
{
    string body;
    body.reserve(1 + payload.size());
    body.push_back(static_cast<char>(extId));
    body.append(payload);
    return btEncodeMessage(kBtExtMessageId, body);
}

string btBase32Decode(const string &in)
{
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    int table[256];
    for (int &v : table) {
        v = -1;
    }
    for (int i = 0; alphabet[i]; ++i) {
        table[static_cast<unsigned char>(alphabet[i])] = i;
        if (alphabet[i] >= 'A' && alphabet[i] <= 'Z') {
            table[static_cast<unsigned char>(alphabet[i] - 'A' + 'a')] = i;
        }
    }
    string filtered;
    filtered.reserve(in.size());
    for (unsigned char c : in) {
        if (c == '=' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        if (table[c] < 0) {
            return string();
        }
        filtered.push_back(static_cast<char>(c));
    }
    if (filtered.empty()) {
        return string();
    }
    string out;
    out.reserve((filtered.size() * 5) / 8);
    int buffer = 0;
    int bitsLeft = 0;
    for (unsigned char c : filtered) {
        buffer = (buffer << 5) | table[c];
        bitsLeft += 5;
        if (bitsLeft >= 8) {
            bitsLeft -= 8;
            out.push_back(static_cast<char>((buffer >> bitsLeft) & 0xff));
        }
    }
    return out;
}

bool btReadMessage(shared_ptr<SocketLike> sock, std::uint8_t *id, string *payload, float timeoutSecs)
{
    if (!sock) {
        return false;
    }
    Timeout t(timeoutSecs);
    string header = sock->recvall(4);
    if (header.size() != 4) {
        return false;
    }
    std::uint32_t len = btReadBe32(header.data());
    if (len == 0) {
        // keep-alive
        if (id) {
            *id = 255;
        }
        if (payload) {
            payload->clear();
        }
        return true;
    }
    if (len > 16 * 1024 * 1024) {
        return false;
    }
    string body = sock->recvall(static_cast<std::int32_t>(len));
    if (body.size() != len) {
        return false;
    }
    if (id) {
        *id = static_cast<std::uint8_t>(body[0]);
    }
    if (payload) {
        *payload = body.substr(1);
    }
    return true;
}

static string makePeerId()
{
    // Azureus-style: -QT0001- + 12 random bytes printable
    string id = "-QT0001-";
    string rnd = randomBytes(12);
    for (char &c : rnd) {
        unsigned char u = static_cast<unsigned char>(c);
        c = static_cast<char>('0' + (u % 10));
    }
    id.append(rnd);
    return id;
}

// ---------------------------------------------------------------------------
// TorrentMeta
// ---------------------------------------------------------------------------

static void collectTrackers(const Bencode &root, vector<string> *out)
{
    if (!out) {
        return;
    }
    if (root.isDict()) {
        const map<string, Bencode> &m = root.toMap();
        auto it = m.find("announce");
        if (it != m.end() && it->second.isString()) {
            out->push_back(it->second.toString());
        }
        it = m.find("announce-list");
        if (it != m.end() && it->second.isList()) {
            for (const Bencode &tier : it->second.toList()) {
                if (tier.isList()) {
                    for (const Bencode &u : tier.toList()) {
                        if (u.isString()) {
                            out->push_back(u.toString());
                        }
                    }
                } else if (tier.isString()) {
                    out->push_back(tier.toString());
                }
            }
        }
    }
    // unique preserve order
    vector<string> uniq;
    set<string> seen;
    for (const string &u : *out) {
        if (seen.insert(u).second) {
            uniq.push_back(u);
        }
    }
    *out = uniq;
}

static bool parseInfoDictFields(const Bencode &info, shared_ptr<TorrentMetaPrivate> d)
{
    if (!info.isDict()) {
        d->errorString = "info is not a dict";
        return false;
    }
    const map<string, Bencode> &im = info.toMap();
    auto nameIt = im.find("name");
    d->name = (nameIt != im.end() && nameIt->second.isString()) ? nameIt->second.toString() : "torrent";
    auto plIt = im.find("piece length");
    if (plIt == im.end() || !plIt->second.isInteger() || plIt->second.toInteger() <= 0) {
        d->errorString = "missing piece length";
        return false;
    }
    d->pieceLength = static_cast<std::int32_t>(plIt->second.toInteger());
    auto piecesIt = im.find("pieces");
    if (piecesIt == im.end() || !piecesIt->second.isString()) {
        d->errorString = "missing pieces";
        return false;
    }
    const string &pieces = piecesIt->second.toString();
    if (pieces.size() % 20 != 0 || pieces.empty()) {
        d->errorString = "invalid pieces length";
        return false;
    }
    d->pieceHashes.clear();
    for (size_t i = 0; i < pieces.size(); i += 20) {
        d->pieceHashes.push_back(pieces.substr(i, 20));
    }

    d->files.clear();
    d->totalSize = 0;
    auto filesIt = im.find("files");
    if (filesIt != im.end() && filesIt->second.isList()) {
        for (const Bencode &f : filesIt->second.toList()) {
            if (!f.isDict()) {
                continue;
            }
            const map<string, Bencode> &fm = f.toMap();
            auto lenIt = fm.find("length");
            auto pathIt = fm.find("path");
            if (lenIt == fm.end() || !lenIt->second.isInteger() || pathIt == fm.end() || !pathIt->second.isList()) {
                d->errorString = "invalid file entry";
                return false;
            }
            string path;
            for (const Bencode &part : pathIt->second.toList()) {
                if (!part.isString()) {
                    continue;
                }
                if (!path.empty()) {
                    path.push_back('/');
                }
                path.append(part.toString());
            }
            TorrentFileInfo fi(d->name + "/" + path, lenIt->second.toInteger());
            d->totalSize += fi.length();
            d->files.push_back(fi);
        }
    } else {
        auto lenIt = im.find("length");
        if (lenIt == im.end() || !lenIt->second.isInteger()) {
            d->errorString = "missing length";
            return false;
        }
        TorrentFileInfo fi(d->name, lenIt->second.toInteger());
        d->totalSize = fi.length();
        d->files.push_back(fi);
    }

    const std::int64_t expectedPieces = (d->totalSize + d->pieceLength - 1) / d->pieceLength;
    if (static_cast<std::int64_t>(d->pieceHashes.size()) != expectedPieces) {
        d->errorString = "piece count mismatch";
        return false;
    }
    return true;
}

static bool parseInfoDictBytes(const string &infoRaw, shared_ptr<TorrentMetaPrivate> d,
                               const vector<string> &trackers)
{
    d->valid = false;
    d->errorString.clear();
    if (infoRaw.empty() || infoRaw[0] != 'd') {
        d->errorString = "invalid info dict";
        return false;
    }
    string digest = MessageDigest::digest(infoRaw, MessageDigest::Sha1);
    d->infoHash = InfoHash::fromBytes(digest);
    if (!d->infoHash.isValid()) {
        d->errorString = "bad infohash";
        return false;
    }
    string err;
    Bencode info = Bencode::decode(infoRaw, &err);
    if (!parseInfoDictFields(info, d)) {
        if (d->errorString.empty()) {
            d->errorString = err.empty() ? "invalid info dict" : err;
        }
        return false;
    }
    d->infoDict = infoRaw;
    d->trackers = trackers;
    d->valid = true;
    return true;
}

static bool parseTorrentMeta(const string &data, shared_ptr<TorrentMetaPrivate> d)
{
    d->valid = false;
    d->errorString.clear();
    string err;
    Bencode root = Bencode::decode(data, &err);
    if (!root.isDict()) {
        d->errorString = err.empty() ? "invalid torrent" : err;
        return false;
    }
    string infoRaw = btExtractInfoDict(data);
    if (infoRaw.empty()) {
        // fallback: re-encode info dict
        auto it = root.toMap().find("info");
        if (it == root.toMap().end() || !it->second.isDict()) {
            d->errorString = "missing info dict";
            return false;
        }
        infoRaw = it->second.encode();
    }
    vector<string> trackers;
    collectTrackers(root, &trackers);
    return parseInfoDictBytes(infoRaw, d, trackers);
}

TorrentMeta::TorrentMeta()
    : d(make_shared<TorrentMetaPrivate>())
{
}

TorrentMeta::TorrentMeta(const TorrentMeta &other)
    : d(other.d)
{
}

TorrentMeta &TorrentMeta::operator=(const TorrentMeta &other)
{
    d = other.d;
    return *this;
}

TorrentMeta::~TorrentMeta() { }

TorrentMeta TorrentMeta::fromBytes(const string &data)
{
    TorrentMeta m;
    parseTorrentMeta(data, m.d);
    return m;
}

TorrentMeta TorrentMeta::fromFile(const string &path)
{
    bool ok = false;
    string data = PosixPath(path).readall(&ok);
    if (!ok) {
        TorrentMeta m;
        m.d->errorString = "cannot read torrent file";
        return m;
    }
    return fromBytes(data);
}

TorrentMeta TorrentMeta::fromInfoDict(const string &infoDict, const vector<string> &trackers)
{
    TorrentMeta m;
    parseInfoDictBytes(infoDict, m.d, trackers);
    return m;
}

bool TorrentMeta::isValid() const
{
    return d && d->valid;
}

InfoHash TorrentMeta::infoHash() const
{
    return d ? d->infoHash : InfoHash();
}

string TorrentMeta::name() const
{
    return d ? d->name : string();
}

std::int64_t TorrentMeta::totalSize() const
{
    return d ? d->totalSize : 0;
}

std::int32_t TorrentMeta::pieceLength() const
{
    return d ? d->pieceLength : 0;
}

std::int32_t TorrentMeta::pieceCount() const
{
    return d ? static_cast<std::int32_t>(d->pieceHashes.size()) : 0;
}

string TorrentMeta::pieceHash(std::int32_t index) const
{
    if (!d || index < 0 || index >= static_cast<std::int32_t>(d->pieceHashes.size())) {
        return string();
    }
    return d->pieceHashes[static_cast<size_t>(index)];
}

vector<TorrentFileInfo> TorrentMeta::files() const
{
    return d ? d->files : vector<TorrentFileInfo>();
}

vector<string> TorrentMeta::trackers() const
{
    return d ? d->trackers : vector<string>();
}

string TorrentMeta::infoDict() const
{
    return d ? d->infoDict : string();
}

string TorrentMeta::errorString() const
{
    return d ? d->errorString : string();
}

// ---------------------------------------------------------------------------
// MagnetLink
// ---------------------------------------------------------------------------

static string btToLowerAscii(string s)
{
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

static bool btParseMagnetPeer(const string &value, MagnetPeerHint *out)
{
    if (!out || value.empty()) {
        return false;
    }
    string host;
    string portStr;
    if (!value.empty() && value[0] == '[') {
        size_t close = value.find(']');
        if (close == string::npos || close + 1 >= value.size() || value[close + 1] != ':') {
            return false;
        }
        host = value.substr(1, close - 1);
        portStr = value.substr(close + 2);
    } else {
        size_t colon = value.rfind(':');
        if (colon == string::npos || colon == 0 || colon + 1 >= value.size()) {
            return false;
        }
        host = value.substr(0, colon);
        portStr = value.substr(colon + 1);
    }
    if (host.empty() || portStr.empty()) {
        return false;
    }
    int port = 0;
    for (char c : portStr) {
        if (c < '0' || c > '9') {
            return false;
        }
        port = port * 10 + (c - '0');
        if (port > 65535) {
            return false;
        }
    }
    if (port <= 0) {
        return false;
    }
    *out = MagnetPeerHint(host, static_cast<std::uint16_t>(port));
    return true;
}

MagnetLink::MagnetLink()
    : d(make_shared<MagnetLinkPrivate>())
{
}

MagnetLink::MagnetLink(const MagnetLink &other)
    : d(other.d)
{
}

MagnetLink &MagnetLink::operator=(const MagnetLink &other)
{
    d = other.d;
    return *this;
}

MagnetLink::~MagnetLink() { }

MagnetLink MagnetLink::parse(const string &uri)
{
    MagnetLink m;
    string s = uri;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
        ++start;
    }
    s = s.substr(start);
    if (s.size() < 7 || btToLowerAscii(s.substr(0, 7)) != "magnet:") {
        m.d->errorString = "not a magnet URI";
        return m;
    }
    size_t q = s.find('?');
    if (q == string::npos || q + 1 >= s.size()) {
        m.d->errorString = "missing magnet query";
        return m;
    }
    string query = s.substr(q + 1);
    vector<pair<string, string>> items;
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        string pair = (amp == string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
        pos = (amp == string::npos) ? query.size() : amp + 1;
        if (pair.empty()) {
            continue;
        }
        size_t eq = pair.find('=');
        string key = utils::Url::fromEncodedComponent(eq == string::npos ? pair : pair.substr(0, eq));
        string val = utils::Url::fromEncodedComponent(eq == string::npos ? string() : pair.substr(eq + 1));
        items.push_back(make_pair(key, val));
    }

    InfoHash hash;
    for (const auto &it : items) {
        const string keyLower = btToLowerAscii(it.first);
        if (keyLower == "xt") {
            string xt = btToLowerAscii(it.second);
            const string prefix = "urn:btih:";
            if (xt.compare(0, prefix.size(), prefix) != 0) {
                continue;  // ignore urn:btmh (v2) and unknown topics
            }
            string id = it.second.substr(prefix.size());
            // strip optional whitespace
            while (!id.empty() && (id.back() == ' ' || id.back() == '\t')) {
                id.pop_back();
            }
            if (id.size() == 40) {
                hash = InfoHash::fromHex(btToLowerAscii(id));
            } else if (id.size() == 32) {
                string raw = btBase32Decode(id);
                if (raw.size() == 20) {
                    hash = InfoHash::fromBytes(raw);
                }
            }
        } else if (keyLower == "dn") {
            m.d->displayName = it.second;
        } else if (keyLower == "tr") {
            if (!it.second.empty()) {
                m.d->trackers.push_back(it.second);
            }
        } else if (keyLower == "x.pe") {
            MagnetPeerHint peer;
            if (btParseMagnetPeer(it.second, &peer)) {
                m.d->peers.push_back(peer);
            }
        }
    }
    if (!hash.isValid()) {
        m.d->errorString = "missing or invalid xt=urn:btih infohash";
        return m;
    }
    // unique trackers preserve order
    vector<string> uniq;
    set<string> seen;
    for (const string &t : m.d->trackers) {
        if (seen.insert(t).second) {
            uniq.push_back(t);
        }
    }
    m.d->trackers = uniq;
    m.d->infoHash = hash;
    m.d->valid = true;
    return m;
}

bool MagnetLink::isValid() const
{
    return d && d->valid;
}

InfoHash MagnetLink::infoHash() const
{
    return d ? d->infoHash : InfoHash();
}

string MagnetLink::displayName() const
{
    return d ? d->displayName : string();
}

vector<string> MagnetLink::trackers() const
{
    return d ? d->trackers : vector<string>();
}

vector<MagnetPeerHint> MagnetLink::peers() const
{
    return d ? d->peers : vector<MagnetPeerHint>();
}

string MagnetLink::errorString() const
{
    return d ? d->errorString : string();
}

// ---------------------------------------------------------------------------
// PieceStorage
// ---------------------------------------------------------------------------

PieceStorage::PieceStorage()
    : m_open(false)
    , m_pieceCount(0)
    , m_haveBytes(0)
{
}

PieceStorage::~PieceStorage()
{
    close();
}

bool PieceStorage::open(const TorrentMeta &meta, const string &downloadDir)
{
    close();
    if (!meta.isValid()) {
        m_error = "invalid meta";
        return false;
    }
    m_meta = meta;
    m_root = downloadDir.empty() ? "." : downloadDir;
    PosixPath root(m_root);
    if (!root.exists()) {
        if (!root.mkdir(true)) {
            m_error = "cannot create download dir";
            return false;
        }
    }
    m_pieceCount = meta.pieceCount();
    m_have.assign(static_cast<size_t>(m_pieceCount), false);
    m_haveBytes = 0;
    m_files.clear();
    std::int64_t off = 0;
    for (const TorrentFileInfo &fi : meta.files()) {
        FileMap fm;
        fm.path = m_root + "/" + fi.path();
        fm.offset = off;
        fm.length = fi.length();
        off += fi.length();
        PosixPath fp(fm.path);
        PosixPath parent = fp.parentPath();
        if (!parent.exists()) {
            parent.mkdir(true);
        }
#ifdef NG_OS_WIN
        fm.fd = _open(fm.path.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fm.fd = ::open(fm.path.c_str(), O_RDWR | O_CREAT, 0644);
#endif
        if (fm.fd < 0) {
            m_error = "cannot open " + fm.path;
            close();
            return false;
        }
#ifdef NG_OS_WIN
        _chsize_s(fm.fd, fi.length());
#else
        if (ftruncate(fm.fd, fi.length()) != 0) {
            // ignore sparse failure
        }
#endif
        m_files.push_back(fm);
    }
    m_open = true;
    checkAll();
    return true;
}

void PieceStorage::close()
{
    for (FileMap &fm : m_files) {
        if (fm.fd >= 0) {
#ifdef NG_OS_WIN
            _close(fm.fd);
#else
            ::close(fm.fd);
#endif
            fm.fd = -1;
        }
    }
    m_files.clear();
    m_open = false;
}

std::int32_t PieceStorage::pieceLength(std::int32_t index) const
{
    if (index < 0 || index >= m_pieceCount) {
        return 0;
    }
    const std::int64_t start = static_cast<std::int64_t>(index) * m_meta.pieceLength();
    const std::int64_t end = min(start + m_meta.pieceLength(), m_meta.totalSize());
    return static_cast<std::int32_t>(end - start);
}

bool PieceStorage::hasPiece(std::int32_t index) const
{
    if (index < 0 || index >= m_pieceCount) {
        return false;
    }
    return m_have[static_cast<size_t>(index)];
}

string PieceStorage::bitfield() const
{
    string bf((static_cast<size_t>(m_pieceCount) + 7) / 8, '\0');
    for (int i = 0; i < m_pieceCount; ++i) {
        if (m_have[static_cast<size_t>(i)]) {
            btBitSet(&bf, i);
        }
    }
    return bf;
}

std::int64_t PieceStorage::bytesLeft() const
{
    return m_meta.totalSize() - m_haveBytes;
}

std::int64_t PieceStorage::bytesHave() const
{
    return m_haveBytes;
}

bool PieceStorage::mapPiece(std::int32_t piece, std::int32_t offset, std::int32_t length,
                            vector<pair<int, pair<std::int64_t, std::int32_t>>> *spans) const
{
    if (!spans || piece < 0 || piece >= m_pieceCount || offset < 0 || length <= 0) {
        return false;
    }
    if (offset + length > pieceLength(piece)) {
        return false;
    }
    std::int64_t global = static_cast<std::int64_t>(piece) * m_meta.pieceLength() + offset;
    std::int32_t remain = length;
    spans->clear();
    for (size_t i = 0; i < m_files.size() && remain > 0; ++i) {
        const FileMap &fm = m_files[i];
        if (global >= fm.offset + fm.length) {
            continue;
        }
        if (global + remain <= fm.offset) {
            break;
        }
        const std::int64_t fileOff = global - fm.offset;
        const std::int32_t take = static_cast<std::int32_t>(min<std::int64_t>(remain, fm.length - fileOff));
        spans->push_back(make_pair(static_cast<int>(i), make_pair(fileOff, take)));
        global += take;
        remain -= take;
    }
    return remain == 0;
}

bool PieceStorage::ioWrite(int fd, std::int64_t fileOff, const char *data, std::int32_t len)
{
#ifdef NG_OS_WIN
    if (_lseeki64(fd, fileOff, SEEK_SET) < 0) {
        return false;
    }
    return _write(fd, data, len) == len;
#else
    std::int32_t done = 0;
    while (done < len) {
        ssize_t n = ::pwrite(fd, data + done, static_cast<size_t>(len - done), fileOff + done);
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::int32_t>(n);
    }
    return true;
#endif
}

bool PieceStorage::ioRead(int fd, std::int64_t fileOff, char *data, std::int32_t len)
{
#ifdef NG_OS_WIN
    if (_lseeki64(fd, fileOff, SEEK_SET) < 0) {
        return false;
    }
    return _read(fd, data, len) == len;
#else
    std::int32_t done = 0;
    while (done < len) {
        ssize_t n = ::pread(fd, data + done, static_cast<size_t>(len - done), fileOff + done);
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::int32_t>(n);
    }
    return true;
#endif
}

bool PieceStorage::writeBlock(std::int32_t piece, std::int32_t offset, const string &data)
{
    vector<pair<int, pair<std::int64_t, std::int32_t>>> spans;
    if (!mapPiece(piece, offset, static_cast<std::int32_t>(data.size()), &spans)) {
        return false;
    }
    std::int32_t pos = 0;
    for (const auto &sp : spans) {
        if (!ioWrite(m_files[static_cast<size_t>(sp.first)].fd, sp.second.first, data.data() + pos, sp.second.second)) {
            m_error = "write failed";
            return false;
        }
        pos += sp.second.second;
    }
    return true;
}

bool PieceStorage::readBlock(std::int32_t piece, std::int32_t offset, std::int32_t length, string *out)
{
    if (!out) {
        return false;
    }
    vector<pair<int, pair<std::int64_t, std::int32_t>>> spans;
    if (!mapPiece(piece, offset, length, &spans)) {
        return false;
    }
    out->assign(static_cast<size_t>(length), '\0');
    std::int32_t pos = 0;
    for (const auto &sp : spans) {
        if (!ioRead(m_files[static_cast<size_t>(sp.first)].fd, sp.second.first, &(*out)[static_cast<size_t>(pos)],
                    sp.second.second)) {
            m_error = "read failed";
            return false;
        }
        pos += sp.second.second;
    }
    return true;
}

bool PieceStorage::commitPiece(std::int32_t piece, const string &data)
{
    if (piece < 0 || piece >= m_pieceCount) {
        return false;
    }
    if (static_cast<std::int32_t>(data.size()) != pieceLength(piece)) {
        return false;
    }
    string dig = MessageDigest::digest(data, MessageDigest::Sha1);
    if (dig != m_meta.pieceHash(piece)) {
        m_error = "piece hash mismatch";
        return false;
    }
    if (!writeBlock(piece, 0, data)) {
        return false;
    }
    if (!m_have[static_cast<size_t>(piece)]) {
        m_have[static_cast<size_t>(piece)] = true;
        m_haveBytes += data.size();
    }
    return true;
}

bool PieceStorage::checkAll()
{
    m_haveBytes = 0;
    for (int i = 0; i < m_pieceCount; ++i) {
        string data;
        if (!readBlock(i, 0, pieceLength(i), &data)) {
            m_have[static_cast<size_t>(i)] = false;
            continue;
        }
        string dig = MessageDigest::digest(data, MessageDigest::Sha1);
        bool ok = dig == m_meta.pieceHash(i);
        m_have[static_cast<size_t>(i)] = ok;
        if (ok) {
            m_haveBytes += data.size();
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// PiecePicker
// ---------------------------------------------------------------------------

void PiecePicker::reset(std::int32_t pieceCount, PieceStorage *storage)
{
    m_storage = storage;
    m_pieceCount = pieceCount;
    m_availability.assign(static_cast<size_t>(pieceCount), 0);
    m_inflight.clear();
    m_buffers.clear();
    m_gotOffsets.clear();
}

void PiecePicker::addPeerBitfield(const string &bitfield)
{
    for (int i = 0; i < m_pieceCount; ++i) {
        if (btBitTest(bitfield, i)) {
            ++m_availability[static_cast<size_t>(i)];
        }
    }
}

void PiecePicker::addPeerHave(std::int32_t piece)
{
    if (piece >= 0 && piece < m_pieceCount) {
        ++m_availability[static_cast<size_t>(piece)];
    }
}

void PiecePicker::removePeerBitfield(const string &bitfield)
{
    for (int i = 0; i < m_pieceCount; ++i) {
        if (btBitTest(bitfield, i) && m_availability[static_cast<size_t>(i)] > 0) {
            --m_availability[static_cast<size_t>(i)];
        }
    }
}

string *PiecePicker::pieceBuffer(std::int32_t piece)
{
    auto it = m_buffers.find(piece);
    if (it == m_buffers.end()) {
        string buf(static_cast<size_t>(m_storage->pieceLength(piece)), '\0');
        it = m_buffers.insert(make_pair(piece, buf)).first;
    }
    return &it->second;
}

bool PiecePicker::hasBlock(std::int32_t piece, std::int32_t offset) const
{
    auto it = m_gotOffsets.find(piece);
    return it != m_gotOffsets.end() && it->second.count(offset) > 0;
}

bool PiecePicker::isPieceDataComplete(std::int32_t piece) const
{
    if (!m_storage || piece < 0 || piece >= m_pieceCount) {
        return false;
    }
    const std::int32_t need = m_storage->pieceLength(piece);
    for (std::int32_t o = 0; o < need; o += kBtBlockSize) {
        if (!hasBlock(piece, o)) {
            return false;
        }
    }
    return true;
}

void PiecePicker::abandonPiece(std::int32_t piece)
{
    m_buffers.erase(piece);
    m_gotOffsets.erase(piece);
    for (auto it = m_inflight.begin(); it != m_inflight.end();) {
        if (it->piece == piece) {
            it = m_inflight.erase(it);
        } else {
            ++it;
        }
    }
}

bool PiecePicker::nextRequest(const string &peerBitfield, bool endgame, BlockRequest *out)
{
    if (!out || !m_storage) {
        return false;
    }
    int best = -1;
    int bestAvail = INT32_MAX;
    for (int i = 0; i < m_pieceCount; ++i) {
        if (m_storage->hasPiece(i)) {
            continue;
        }
        if (!btBitTest(peerBitfield, i)) {
            continue;
        }
        int avail = m_availability[static_cast<size_t>(i)];
        if (avail < bestAvail || (avail == bestAvail && (best < 0 || i < best))) {
            // prefer pieces already partially downloaded
            if (m_buffers.count(i) || best < 0 || !m_buffers.count(best) || avail < bestAvail) {
                best = i;
                bestAvail = avail;
            }
        }
    }
    if (best < 0) {
        return false;
    }
    const std::int32_t plen = m_storage->pieceLength(best);
    set<std::int32_t> &got = m_gotOffsets[best];
    for (std::int32_t off = 0; off < plen; off += kBtBlockSize) {
        std::int32_t len = min(kBtBlockSize, plen - off);
        BlockRequest req(best, off, len);
        if (got.count(off)) {
            continue;
        }
        if (!endgame && m_inflight.count(req)) {
            continue;
        }
        *out = req;
        return true;
    }
    return false;
}

void PiecePicker::markRequested(const BlockRequest &req)
{
    m_inflight.insert(req);
}

void PiecePicker::markReceived(const BlockRequest &req)
{
    m_inflight.erase(req);
    m_gotOffsets[req.piece].insert(req.offset);
}

void PiecePicker::markFailed(const BlockRequest &req)
{
    m_inflight.erase(req);
}

// ---------------------------------------------------------------------------
// TorrentHandle
// ---------------------------------------------------------------------------

TorrentHandlePrivate::TorrentHandlePrivate()
    : lastDownloaded(0)
    , lastUploaded(0)
    , lastRateSampleSecs(0.0)
    , paused(false)
    , removed(false)
    , started(false)
    , needsMetadata(false)
    , metadataSize(-1)
{
    stats.setState(TorrentStats::Stopped);
}

TorrentHandlePrivate::~TorrentHandlePrivate() { }

InfoHash TorrentHandlePrivate::effectiveInfoHash() const
{
    if (meta.isValid()) {
        return meta.infoHash();
    }
    return magnetInfoHash;
}

vector<string> TorrentHandlePrivate::effectiveTrackers() const
{
    if (meta.isValid()) {
        return meta.trackers();
    }
    return magnetTrackers;
}

TorrentStats TorrentHandlePrivate::statsSnapshot() const
{
    lock_guard<std::mutex> lock(mutex_);
    TorrentStats s = stats;
    s.setLeft(storage.isOpen() ? storage.bytesLeft() : (meta.isValid() ? meta.totalSize() : 0));
    s.setProgress(meta.totalSize() > 0 ? static_cast<double>(meta.totalSize() - s.left()) / meta.totalSize() : 0.0);
    s.setPeersTotal(static_cast<int>(peers.size()));
    s.setPeersConnected(static_cast<int>(connectedPeers.size()));
    const double now = static_cast<double>(time(nullptr));
    if (lastRateSampleSecs > 0.0 && now > lastRateSampleSecs) {
        const double dt = now - lastRateSampleSecs;
        s.setDownloadRate(static_cast<double>(s.downloaded() - lastDownloaded) / dt);
        s.setUploadRate(static_cast<double>(s.uploaded() - lastUploaded) / dt);
    }
    // Cast away const for sample bookkeeping — statsSnapshot is logically const for callers.
    TorrentHandlePrivate *self = const_cast<TorrentHandlePrivate *>(this);
    self->lastDownloaded = s.downloaded();
    self->lastUploaded = s.uploaded();
    self->lastRateSampleSecs = now;
    return s;
}

void TorrentHandlePrivate::setState(TorrentStats::State st, const string &err)
{
    lock_guard<std::mutex> lock(mutex_);
    stats.setState(st);
    if (!err.empty()) {
        stats.setErrorString(err);
    }
}

void TorrentHandlePrivate::notifyProgress()
{
    TorrentStats s = statsSnapshot();
    function<void(const TorrentStats &)> cb;
    {
        lock_guard<std::mutex> lock(mutex_);
        cb = progressCallback;
        stats = s;
    }
    if (cb) {
        cb(s);
    }
    if (s.left() == 0 && s.state() != TorrentStats::Error) {
        setState(TorrentStats::Seeding);
        finishedEvent.set();
    }
}

TorrentHandle::TorrentHandle() { }

TorrentHandle::TorrentHandle(shared_ptr<TorrentHandlePrivate> priv)
    : d(std::move(priv))
{
}

TorrentHandle::TorrentHandle(const TorrentHandle &other)
    : d(other.d)
{
}

TorrentHandle &TorrentHandle::operator=(const TorrentHandle &other)
{
    d = other.d;
    return *this;
}

TorrentHandle::~TorrentHandle() { }

bool TorrentHandle::isValid() const
{
    return static_cast<bool>(d);
}

InfoHash TorrentHandle::infoHash() const
{
    return d ? d->effectiveInfoHash() : InfoHash();
}

TorrentMeta TorrentHandle::meta() const
{
    return d ? d->meta : TorrentMeta();
}

TorrentStats TorrentHandle::stats() const
{
    return d ? d->statsSnapshot() : TorrentStats();
}

bool TorrentHandle::isFinished() const
{
    if (!d) {
        return false;
    }
    return d->storage.isOpen() && d->storage.bytesLeft() == 0;
}

bool TorrentHandle::wait(float timeout)
{
    if (!d) {
        return false;
    }
    if (isFinished()) {
        return true;
    }
    if (timeout < 0) {
        return d->finishedEvent.tryWait() || isFinished();
    }
    return d->finishedEvent.tryWait(static_cast<std::uint32_t>(timeout * 1000.0f)) || isFinished();
}

void TorrentHandle::pause()
{
    if (d) {
        d->paused = true;
        d->setState(TorrentStats::Stopped);
    }
}

void TorrentHandle::resume()
{
    if (!d) {
        return;
    }
    d->paused = false;
    if (d->needsMetadata) {
        d->setState(TorrentStats::Metadata);
    } else if (!d->storage.isOpen()) {
        d->setState(TorrentStats::Checking);
    } else {
        d->setState(d->storage.bytesLeft() == 0 ? TorrentStats::Seeding : TorrentStats::Downloading);
    }
}

void TorrentHandle::remove(bool deleteFiles)
{
    if (!d) {
        return;
    }
    d->removed = true;
    d->removedEvent.set();
    d->finishedEvent.set();
    if (deleteFiles && d->storage.isOpen()) {
        // best-effort: close handles; leave files (full delete omitted for safety)
        d->storage.close();
    }
    (void) deleteFiles;
}

void TorrentHandle::setProgressCallback(function<void(const TorrentStats &)> callback)
{
    if (d) {
        lock_guard<std::mutex> lock(d->mutex_);
        d->progressCallback = std::move(callback);
    }
}

// ---------------------------------------------------------------------------
// Trackers
// ---------------------------------------------------------------------------

vector<BtPeerAddr> TorrentSessionPrivate::announceHttp(shared_ptr<TorrentHandlePrivate> h, const string &url,
                                                       const string &event)
{
    vector<BtPeerAddr> peers;
    utils::Url u(url);
    if (!u.isValid()) {
        return peers;
    }
    string scheme = u.scheme();
    if (scheme != "http" && scheme != "https") {
        return peers;
    }
    TorrentStats st = h->statsSnapshot();
    utils::UrlQuery q;
    q.addQueryItem("info_hash", btPercentEncode(h->effectiveInfoHash().toBytes()));
    q.addQueryItem("peer_id", btPercentEncode(peerId));
    q.addQueryItem("port", to_string(effectiveListenPort()));
    q.addQueryItem("uploaded", to_string(st.uploaded()));
    q.addQueryItem("downloaded", to_string(st.downloaded()));
    q.addQueryItem("left", to_string(st.left()));
    q.addQueryItem("compact", "1");
    q.addQueryItem("numwant", "50");
    if (!event.empty()) {
        q.addQueryItem("event", event);
    }
    // Build URL carefully: info_hash/peer_id already percent-encoded; UrlQuery may encode again.
    // Append raw query to avoid double-encoding binary fields.
    string base = u.scheme() + "://" + u.host();
    int port = u.port();
    if (port > 0) {
        base += ":" + to_string(port);
    }
    base += u.path().empty() ? "/" : u.path();
    string query = "info_hash=" + btPercentEncode(h->effectiveInfoHash().toBytes()) + "&peer_id="
            + btPercentEncode(peerId) + "&port=" + to_string(effectiveListenPort())
            + "&uploaded=" + to_string(st.uploaded()) + "&downloaded=" + to_string(st.downloaded())
            + "&left=" + to_string(st.left()) + "&compact=1&numwant=50";
    if (!event.empty()) {
        query += "&event=" + event;
    }
    string full = base + "?" + query;
    HttpResponse resp = http.get(full);
    if (!resp.isOk()) {
        return peers;
    }
    string err;
    Bencode body = Bencode::decode(resp.body(), &err);
    if (!body.isDict()) {
        return peers;
    }
    const map<string, Bencode> &m = body.toMap();
    auto it = m.find("peers");
    if (it != m.end()) {
        if (it->second.isString()) {
            peers = btDecodeCompactPeerList(it->second.toString(), false);
        } else if (it->second.isList()) {
            for (const Bencode &p : it->second.toList()) {
                if (!p.isDict()) {
                    continue;
                }
                const map<string, Bencode> &pm = p.toMap();
                auto ipIt = pm.find("ip");
                auto portIt = pm.find("port");
                if (ipIt == pm.end() || portIt == pm.end()) {
                    continue;
                }
                HostAddress addr;
                addr.setAddress(ipIt->second.toString());
                BtPeerAddr pa(addr, static_cast<std::uint16_t>(portIt->second.toInteger()));
                if (pa.isValid()) {
                    peers.push_back(pa);
                }
            }
        }
    }
    it = m.find("peers6");
    if (it != m.end() && it->second.isString()) {
        vector<BtPeerAddr> v6 = btDecodeCompactPeerList(it->second.toString(), true);
        peers.insert(peers.end(), v6.begin(), v6.end());
    }
    return peers;
}

vector<BtPeerAddr> TorrentSessionPrivate::announceUdp(shared_ptr<TorrentHandlePrivate> h, const string &url,
                                                      const string &event)
{
    vector<BtPeerAddr> peers;
    utils::Url u(url);
    if (!u.isValid() || u.scheme() != "udp") {
        return peers;
    }
    int port = u.port() > 0 ? u.port() : 80;
    shared_ptr<Socket> sock = make_shared<Socket>(HostAddress::IPv4Protocol, Socket::UdpSocket);
    if (!sock->bind(0)) {
        return peers;
    }
    HostAddress trackerAddr;
    {
        shared_ptr<Socket> resolver = make_shared<Socket>(HostAddress::IPv4Protocol, Socket::UdpSocket);
        (void) resolver;
    }
    if (!sock->connect(u.host(), static_cast<std::uint16_t>(port))) {
        // UDP connect sets default peer for send/recv convenience on some stacks;
        // fall back to sendto via connected false path — use HostAddress resolve
    }
    // Resolve host
    vector<HostAddress> addrs = Socket::resolve(u.host());
    if (addrs.empty()) {
        return peers;
    }
    trackerAddr = addrs.front();

    auto udpExchange = [&](const string &req, string *resp, size_t minLen) -> bool {
        if (sock->sendto(req, trackerAddr, static_cast<std::uint16_t>(port))
            != static_cast<std::int32_t>(req.size())) {
            return false;
        }
        HostAddress from;
        std::uint16_t fromPort = 0;
        Timeout t(8.0f);
        string buf(4096, '\0');
        std::int32_t n = sock->recvfrom(&buf[0], static_cast<std::int32_t>(buf.size()), &from, &fromPort);
        if (n < static_cast<std::int32_t>(minLen)) {
            return false;
        }
        buf.resize(static_cast<size_t>(n));
        *resp = buf;
        return true;
    };

    std::uint32_t tx = RandomGenerator::global().generate();
    string creq(16, '\0');
    btWriteBe64(&creq[0], 0x41727101980ULL);
    btWriteBe32(&creq[8], 0);  // connect
    btWriteBe32(&creq[12], tx);
    string cresp;
    if (!udpExchange(creq, &cresp, 16)) {
        return peers;
    }
    if (btReadBe32(cresp.data()) != 0 || btReadBe32(cresp.data() + 4) != tx) {
        return peers;
    }
    std::uint64_t connId = btReadBe64(cresp.data() + 8);

    std::uint32_t eventCode = 0;
    if (event == "completed") {
        eventCode = 1;
    } else if (event == "started") {
        eventCode = 2;
    } else if (event == "stopped") {
        eventCode = 3;
    }
    TorrentStats st = h->statsSnapshot();
    tx = RandomGenerator::global().generate();
    string areq(98, '\0');
    btWriteBe64(&areq[0], connId);
    btWriteBe32(&areq[8], 1);  // announce
    btWriteBe32(&areq[12], tx);
    memcpy(&areq[16], h->effectiveInfoHash().toBytes().data(), 20);
    memcpy(&areq[36], peerId.data(), 20);
    btWriteBe64(&areq[56], static_cast<std::uint64_t>(st.downloaded()));
    btWriteBe64(&areq[64], static_cast<std::uint64_t>(st.left()));
    btWriteBe64(&areq[72], static_cast<std::uint64_t>(st.uploaded()));
    btWriteBe32(&areq[80], eventCode);
    btWriteBe32(&areq[84], 0);  // ip
    btWriteBe32(&areq[88], tx);
    btWriteBe32(&areq[92], 50);  // num want
    btWriteBe16(&areq[96], effectiveListenPort());
    string aresp;
    if (!udpExchange(areq, &aresp, 20)) {
        return peers;
    }
    if (btReadBe32(aresp.data()) != 1 || btReadBe32(aresp.data() + 4) != tx) {
        return peers;
    }
    if (aresp.size() > 20) {
        peers = btDecodeCompactPeerList(aresp.substr(20), false);
    }
    return peers;
}

void TorrentSessionPrivate::addPeers(shared_ptr<TorrentHandlePrivate> h, const vector<BtPeerAddr> &list)
{
    lock_guard<std::mutex> lock(h->mutex_);
    for (const BtPeerAddr &p : list) {
        if (p.isValid()) {
            h->peers.insert(p);
        }
    }
}

// ---------------------------------------------------------------------------
// Peer connection / BEP-10 + BEP-9
// ---------------------------------------------------------------------------

static string btBuildExtendedHandshake(std::int64_t metadataSize, std::uint16_t listenPort)
{
    map<string, Bencode> m;
    m["ut_metadata"] = Bencode(static_cast<std::int64_t>(kBtLocalUtMetadataId));
    map<string, Bencode> root;
    root["m"] = Bencode(m);
    root["v"] = Bencode(string("qtng"));
    if (listenPort != 0) {
        root["p"] = Bencode(static_cast<std::int64_t>(listenPort));
    }
    if (metadataSize > 0) {
        root["metadata_size"] = Bencode(metadataSize);
    }
    return Bencode(root).encode();
}

static bool btParseExtendedHandshake(const string &payload, std::uint8_t *utMetadataId, std::int64_t *metadataSize)
{
    if (utMetadataId) {
        *utMetadataId = 0;
    }
    if (metadataSize) {
        *metadataSize = -1;
    }
    string err;
    Bencode root = Bencode::decode(payload, &err);
    if (!root.isDict()) {
        return false;
    }
    const map<string, Bencode> &rm = root.toMap();
    auto mIt = rm.find("m");
    if (mIt != rm.end() && mIt->second.isDict()) {
        const map<string, Bencode> &mm = mIt->second.toMap();
        auto uIt = mm.find("ut_metadata");
        if (uIt != mm.end() && uIt->second.isInteger() && utMetadataId) {
            std::int64_t id = uIt->second.toInteger();
            if (id > 0 && id <= 255) {
                *utMetadataId = static_cast<std::uint8_t>(id);
            }
        }
    }
    auto sIt = rm.find("metadata_size");
    if (sIt != rm.end() && sIt->second.isInteger() && metadataSize) {
        *metadataSize = sIt->second.toInteger();
    }
    return true;
}

bool TorrentSessionPrivate::exchangeExtendedHandshake(shared_ptr<TorrentHandlePrivate> h, shared_ptr<SocketLike> sock,
                                                       std::uint8_t *peerUtMetadata, std::int64_t *peerMetadataSize)
{
    // Only *sends* our BEP-10 handshake. Peer handshake is parsed from later messages
    // (callers may wait for it in metadata mode). Optional out-params are cleared.
    if (peerUtMetadata) {
        *peerUtMetadata = 0;
    }
    if (peerMetadataSize) {
        *peerMetadataSize = -1;
    }
    std::int64_t ourSize = 0;
    {
        lock_guard<std::mutex> lock(h->mutex_);
        if (!h->infoDictRaw.empty()) {
            ourSize = static_cast<std::int64_t>(h->infoDictRaw.size());
        } else if (h->meta.isValid() && !h->meta.infoDict().empty()) {
            h->infoDictRaw = h->meta.infoDict();
            ourSize = static_cast<std::int64_t>(h->infoDictRaw.size());
        }
    }
    string hsPayload = btBuildExtendedHandshake(ourSize, effectiveListenPort());
    return sock->sendall(btEncodeExtended(kBtExtHandshakeId, hsPayload)) > 0;
}

bool TorrentSessionPrivate::tryCommitMetadata(shared_ptr<TorrentHandlePrivate> h)
{
    lock_guard<std::mutex> lock(h->mutex_);
    if (!h->needsMetadata) {
        return true;
    }
    if (h->metadataSize <= 0 || h->metadataBuf.size() != static_cast<size_t>(h->metadataSize)) {
        return false;
    }
    for (bool have : h->metadataHave) {
        if (!have) {
            return false;
        }
    }
    string digest = MessageDigest::digest(h->metadataBuf, MessageDigest::Sha1);
    if (InfoHash::fromBytes(digest) != h->magnetInfoHash) {
        h->metadataBuf.clear();
        h->metadataHave.clear();
        h->metadataSize = -1;
        return false;
    }
    TorrentMeta parsed = TorrentMeta::fromInfoDict(h->metadataBuf, h->magnetTrackers);
    if (!parsed.isValid()) {
        h->metadataBuf.clear();
        h->metadataHave.clear();
        h->metadataSize = -1;
        return false;
    }
    if (!h->magnetDisplayName.empty() && parsed.name() == "torrent") {
        // keep magnet display name only as fallback; parsed name preferred
    }
    h->meta = parsed;
    h->infoDictRaw = h->metadataBuf;
    h->needsMetadata = false;
    h->metadataEvent.set();
    return true;
}

void TorrentSessionPrivate::serveMetadataRequest(shared_ptr<TorrentHandlePrivate> h, shared_ptr<SocketLike> sock,
                                                 std::uint8_t peerUtMetadata, std::int32_t piece)
{
    if (peerUtMetadata == 0 || piece < 0) {
        return;
    }
    string infoRaw;
    {
        lock_guard<std::mutex> lock(h->mutex_);
        infoRaw = h->infoDictRaw;
        if (infoRaw.empty() && h->meta.isValid()) {
            infoRaw = h->meta.infoDict();
            h->infoDictRaw = infoRaw;
        }
    }
    map<string, Bencode> msg;
    msg["msg_type"] = Bencode(static_cast<std::int64_t>(2));  // reject
    msg["piece"] = Bencode(static_cast<std::int64_t>(piece));
    if (infoRaw.empty()) {
        sock->sendall(btEncodeExtended(peerUtMetadata, Bencode(msg).encode()));
        return;
    }
    const std::int64_t total = static_cast<std::int64_t>(infoRaw.size());
    const std::int32_t pieceCount =
            static_cast<std::int32_t>((total + kBtMetadataPieceSize - 1) / kBtMetadataPieceSize);
    if (piece >= pieceCount) {
        sock->sendall(btEncodeExtended(peerUtMetadata, Bencode(msg).encode()));
        return;
    }
    const size_t off = static_cast<size_t>(piece) * static_cast<size_t>(kBtMetadataPieceSize);
    const size_t len = min(static_cast<size_t>(kBtMetadataPieceSize), infoRaw.size() - off);
    msg["msg_type"] = Bencode(static_cast<std::int64_t>(1));  // data
    msg["total_size"] = Bencode(total);
    string payload = Bencode(msg).encode();
    payload.append(infoRaw.data() + off, len);
    sock->sendall(btEncodeExtended(peerUtMetadata, payload));
}

bool TorrentSessionPrivate::fetchMetadataFromPeer(shared_ptr<TorrentHandlePrivate> h, shared_ptr<SocketLike> sock,
                                                  std::uint8_t peerUtMetadata, std::int64_t peerMetadataSize)
{
    if (!h->needsMetadata || peerUtMetadata == 0 || peerMetadataSize <= 0) {
        return false;
    }
    {
        lock_guard<std::mutex> lock(h->mutex_);
        if (h->metadataSize < 0) {
            h->metadataSize = peerMetadataSize;
            h->metadataBuf.assign(static_cast<size_t>(peerMetadataSize), '\0');
            const size_t n = static_cast<size_t>((peerMetadataSize + kBtMetadataPieceSize - 1) / kBtMetadataPieceSize);
            h->metadataHave.assign(n, false);
        } else if (h->metadataSize != peerMetadataSize) {
            return false;
        }
    }

    auto nextPiece = [&]() -> int {
        lock_guard<std::mutex> lock(h->mutex_);
        for (size_t i = 0; i < h->metadataHave.size(); ++i) {
            if (!h->metadataHave[i]) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    while (!h->removed && h->needsMetadata) {
        int piece = nextPiece();
        if (piece < 0) {
            return tryCommitMetadata(h);
        }
        map<string, Bencode> req;
        req["msg_type"] = Bencode(static_cast<std::int64_t>(0));
        req["piece"] = Bencode(static_cast<std::int64_t>(piece));
        if (sock->sendall(btEncodeExtended(peerUtMetadata, Bencode(req).encode())) <= 0) {
            return false;
        }
        bool got = false;
        for (int attempt = 0; attempt < 16 && !got; ++attempt) {
            std::uint8_t msgId = 0;
            string payload;
            if (!btReadMessage(sock, &msgId, &payload, 20.0f)) {
                return false;
            }
            if (msgId == 255) {
                continue;
            }
            if (msgId != kBtExtMessageId || payload.empty()) {
                continue;
            }
            std::uint8_t extId = static_cast<std::uint8_t>(payload[0]);
            string body = payload.substr(1);
            if (extId == kBtExtHandshakeId) {
                continue;
            }
            if (extId != kBtLocalUtMetadataId) {
                continue;
            }
            size_t dictEnd = 0;
            if (!btSkipBencodeValue(body, &dictEnd)) {
                return false;
            }
            string err;
            Bencode dict = Bencode::decode(body.substr(0, dictEnd), &err);
            if (!dict.isDict()) {
                return false;
            }
            const map<string, Bencode> &dm = dict.toMap();
            auto typeIt = dm.find("msg_type");
            auto pieceIt = dm.find("piece");
            if (typeIt == dm.end() || pieceIt == dm.end() || !typeIt->second.isInteger()
                || !pieceIt->second.isInteger()) {
                continue;
            }
            const int msgType = static_cast<int>(typeIt->second.toInteger());
            const int gotPiece = static_cast<int>(pieceIt->second.toInteger());
            if (msgType == 2) {
                return false;  // reject
            }
            if (msgType != 1 || gotPiece != piece) {
                continue;
            }
            string pieceData = body.substr(dictEnd);
            {
                lock_guard<std::mutex> lock(h->mutex_);
                if (gotPiece < 0 || gotPiece >= static_cast<int>(h->metadataHave.size())) {
                    return false;
                }
                const size_t off = static_cast<size_t>(gotPiece) * static_cast<size_t>(kBtMetadataPieceSize);
                size_t expect = static_cast<size_t>(kBtMetadataPieceSize);
                if (off + expect > h->metadataBuf.size()) {
                    expect = h->metadataBuf.size() - off;
                }
                if (pieceData.size() != expect) {
                    return false;
                }
                memcpy(&h->metadataBuf[off], pieceData.data(), pieceData.size());
                h->metadataHave[static_cast<size_t>(gotPiece)] = true;
            }
            got = true;
        }
        if (!got) {
            return false;
        }
    }
    return tryCommitMetadata(h);
}

void TorrentSessionPrivate::runMetadataPeer(shared_ptr<TorrentHandlePrivate> h, shared_ptr<SocketLike> sock,
                                            bool incoming, bool handshakeDone)
{
    if (!h || !sock || h->removed) {
        return;
    }
    BtPeerAddr remote(sock->peerAddress(), sock->peerPort());
    {
        lock_guard<std::mutex> lock(h->mutex_);
        h->connectedPeers.insert(remote);
    }
    struct Guard {
        shared_ptr<TorrentHandlePrivate> h;
        BtPeerAddr remote;
        ~Guard()
        {
            if (h) {
                lock_guard<std::mutex> lock(h->mutex_);
                h->connectedPeers.erase(remote);
            }
        }
    } guard{h, remote};

    try {
        string reserved;
        if (!handshakeDone) {
            Timeout hsTimeout(kBtPeerConnectTimeout);
            if (!incoming) {
                string hs = btEncodeHandshake(h->effectiveInfoHash(), peerId);
                if (sock->sendall(hs) != static_cast<std::int32_t>(hs.size())) {
                    return;
                }
            }
            string theirHs = sock->recvall(68);
            InfoHash theirHash;
            if (!btDecodeHandshake(theirHs, &theirHash, nullptr, &reserved)) {
                return;
            }
            if (theirHash != h->effectiveInfoHash()) {
                return;
            }
            if (incoming) {
                string hs = btEncodeHandshake(h->effectiveInfoHash(), peerId);
                if (sock->sendall(hs) != static_cast<std::int32_t>(hs.size())) {
                    return;
                }
            }
        } else {
            reserved.assign(8, '\0');
            reserved[5] = static_cast<char>(0x10);
        }
        if (!handshakeDone && !btHandshakeSupportsExtension(reserved)) {
            return;
        }
        if (!exchangeExtendedHandshake(h, sock, nullptr, nullptr)) {
            return;
        }

        std::uint8_t peerUt = 0;
        std::int64_t peerSize = -1;
        bool fetchStarted = false;

        while (!h->removed && h->needsMetadata) {
            std::uint8_t msgId = 0;
            string payload;
            if (!btReadMessage(sock, &msgId, &payload, 30.0f)) {
                break;
            }
            if (msgId == 255) {
                continue;
            }
            if (msgId != kBtExtMessageId || payload.empty()) {
                continue;
            }
            std::uint8_t extId = static_cast<std::uint8_t>(payload[0]);
            string body = payload.substr(1);
            if (extId == kBtExtHandshakeId) {
                btParseExtendedHandshake(body, &peerUt, &peerSize);
                if (!fetchStarted && peerUt != 0 && peerSize > 0) {
                    fetchStarted = true;
                    // Drive requests on this connection; returns when complete or rejected.
                    fetchMetadataFromPeer(h, sock, peerUt, peerSize);
                    break;
                }
                continue;
            }
            if (extId == kBtLocalUtMetadataId) {
                size_t dictEnd = 0;
                if (!btSkipBencodeValue(body, &dictEnd)) {
                    break;
                }
                string err;
                Bencode dict = Bencode::decode(body.substr(0, dictEnd), &err);
                if (!dict.isDict()) {
                    continue;
                }
                const map<string, Bencode> &dm = dict.toMap();
                auto typeIt = dm.find("msg_type");
                auto pieceIt = dm.find("piece");
                if (typeIt != dm.end() && pieceIt != dm.end() && typeIt->second.isInteger()
                    && pieceIt->second.isInteger() && typeIt->second.toInteger() == 0) {
                    serveMetadataRequest(h, sock, peerUt,
                                         static_cast<std::int32_t>(pieceIt->second.toInteger()));
                }
            }
        }
    } catch (...) {
    }
}

void TorrentSessionPrivate::runPeer(shared_ptr<TorrentHandlePrivate> h, shared_ptr<SocketLike> sock, bool incoming,
                                    bool handshakeDone)
{
    if (!h || !sock || h->removed) {
        return;
    }
    if (h->needsMetadata || !h->meta.isValid() || !h->storage.isOpen()) {
        runMetadataPeer(h, sock, incoming, handshakeDone);
        return;
    }
    BtPeerAddr remote(sock->peerAddress(), sock->peerPort());
    {
        lock_guard<std::mutex> lock(h->mutex_);
        h->connectedPeers.insert(remote);
    }
    struct Guard {
        shared_ptr<TorrentHandlePrivate> h;
        BtPeerAddr remote;
        string bitfield;
        PiecePicker *picker;
        ~Guard()
        {
            if (h) {
                lock_guard<std::mutex> lock(h->mutex_);
                h->connectedPeers.erase(remote);
                if (picker && !bitfield.empty()) {
                    picker->removePeerBitfield(bitfield);
                }
            }
        }
    } guard{h, remote, string(), &h->picker};

    try {
        string reserved;
        if (!handshakeDone) {
            Timeout hsTimeout(kBtPeerConnectTimeout);
            if (!incoming) {
                string hs = btEncodeHandshake(h->effectiveInfoHash(), peerId);
                if (sock->sendall(hs) != static_cast<std::int32_t>(hs.size())) {
                    return;
                }
            }
            string theirHs = sock->recvall(68);
            InfoHash theirHash;
            string theirId;
            if (!btDecodeHandshake(theirHs, &theirHash, &theirId, &reserved)) {
                return;
            }
            if (theirHash != h->effectiveInfoHash()) {
                return;
            }
            if (incoming) {
                string hs = btEncodeHandshake(h->effectiveInfoHash(), peerId);
                if (sock->sendall(hs) != static_cast<std::int32_t>(hs.size())) {
                    return;
                }
            }
        } else {
            reserved.assign(8, '\0');
            reserved[5] = static_cast<char>(0x10);
        }

        std::uint8_t peerUt = 0;
        std::int64_t peerSize = -1;
        (void) peerSize;
        if (btHandshakeSupportsExtension(reserved) || handshakeDone) {
            exchangeExtendedHandshake(h, sock, nullptr, nullptr);
        }

        {
            lock_guard<std::mutex> lock(h->mutex_);
            if (h->infoDictRaw.empty() && h->meta.isValid()) {
                h->infoDictRaw = h->meta.infoDict();
            }
        }

        string bf = h->storage.bitfield();
        sock->sendall(btEncodeMessage(5, bf));
        sock->sendall(btEncodeMessage(2));
        sock->sendall(btEncodeMessage(1));

        bool amChoked = true;
        bool peerInterested = false;
        (void) peerInterested;
        string peerBf((static_cast<size_t>(h->meta.pieceCount()) + 7) / 8, '\0');
        int outstanding = 0;
        const int maxOutstanding = 8;

        while (!h->removed && !h->paused) {
            if (h->storage.bytesLeft() == 0) {
                h->notifyProgress();
                Coroutine::sleep(2.0f);
            }
            std::uint8_t msgId = 0;
            string payload;
            if (!btReadMessage(sock, &msgId, &payload, 30.0f)) {
                break;
            }
            if (msgId == 255) {
                continue;
            }
            if (msgId == 0) {
                amChoked = true;
            } else if (msgId == 1) {
                amChoked = false;
            } else if (msgId == 2) {
                peerInterested = true;
            } else if (msgId == 3) {
                peerInterested = false;
            } else if (msgId == 4 && payload.size() >= 4) {
                std::int32_t piece = static_cast<std::int32_t>(btReadBe32(payload.data()));
                btBitSet(&peerBf, piece);
                h->picker.addPeerHave(piece);
            } else if (msgId == 5) {
                if (!guard.bitfield.empty()) {
                    h->picker.removePeerBitfield(guard.bitfield);
                }
                peerBf = payload;
                if (peerBf.size() < (static_cast<size_t>(h->meta.pieceCount()) + 7) / 8) {
                    peerBf.resize((static_cast<size_t>(h->meta.pieceCount()) + 7) / 8, '\0');
                }
                guard.bitfield = peerBf;
                h->picker.addPeerBitfield(peerBf);
            } else if (msgId == 6 && payload.size() >= 12) {
                std::int32_t piece = static_cast<std::int32_t>(btReadBe32(payload.data()));
                std::int32_t begin = static_cast<std::int32_t>(btReadBe32(payload.data() + 4));
                std::int32_t length = static_cast<std::int32_t>(btReadBe32(payload.data() + 8));
                if (h->storage.hasPiece(piece) && length > 0 && length <= kBtBlockSize * 2) {
                    string block;
                    if (h->storage.readBlock(piece, begin, length, &block)) {
                        string pl(8, '\0');
                        btWriteBe32(&pl[0], static_cast<std::uint32_t>(piece));
                        btWriteBe32(&pl[4], static_cast<std::uint32_t>(begin));
                        pl.append(block);
                        sock->sendall(btEncodeMessage(7, pl));
                        lock_guard<std::mutex> lock(h->mutex_);
                        h->stats.setUploaded(h->stats.uploaded() + block.size());
                    }
                }
            } else if (msgId == 7 && payload.size() >= 8) {
                std::int32_t piece = static_cast<std::int32_t>(btReadBe32(payload.data()));
                std::int32_t begin = static_cast<std::int32_t>(btReadBe32(payload.data() + 4));
                string block = payload.substr(8);
                BlockRequest req(piece, begin, static_cast<std::int32_t>(block.size()));
                h->picker.markReceived(req);
                --outstanding;
                string *buf = h->picker.pieceBuffer(piece);
                if (begin >= 0
                    && begin + static_cast<std::int32_t>(block.size()) <= static_cast<std::int32_t>(buf->size())) {
                    memcpy(&(*buf)[static_cast<size_t>(begin)], block.data(), block.size());
                }
                {
                    lock_guard<std::mutex> lock(h->mutex_);
                    h->stats.setDownloaded(h->stats.downloaded() + block.size());
                }
                if (h->picker.isPieceDataComplete(piece)) {
                    if (h->storage.commitPiece(piece, *buf)) {
                        h->picker.abandonPiece(piece);
                        string havePl(4, '\0');
                        btWriteBe32(&havePl[0], static_cast<std::uint32_t>(piece));
                        sock->sendall(btEncodeMessage(4, havePl));
                        h->notifyProgress();
                    } else {
                        h->picker.abandonPiece(piece);
                    }
                }
            } else if (msgId == 8 && payload.size() >= 12) {
                // cancel — ignore
            } else if (msgId == kBtExtMessageId && payload.size() >= 1) {
                std::uint8_t extId = static_cast<std::uint8_t>(payload[0]);
                string body = payload.substr(1);
                if (extId == kBtExtHandshakeId) {
                    btParseExtendedHandshake(body, &peerUt, &peerSize);
                } else if (extId == kBtLocalUtMetadataId) {
                    size_t dictEnd = 0;
                    if (btSkipBencodeValue(body, &dictEnd)) {
                        string err;
                        Bencode dict = Bencode::decode(body.substr(0, dictEnd), &err);
                        if (dict.isDict()) {
                            const map<string, Bencode> &dm = dict.toMap();
                            auto typeIt = dm.find("msg_type");
                            auto pieceIt = dm.find("piece");
                            if (typeIt != dm.end() && pieceIt != dm.end() && typeIt->second.isInteger()
                                && pieceIt->second.isInteger() && typeIt->second.toInteger() == 0) {
                                serveMetadataRequest(h, sock, peerUt,
                                                     static_cast<std::int32_t>(pieceIt->second.toInteger()));
                            }
                        }
                    }
                }
            }

            bool endgame = h->storage.bytesLeft() < static_cast<std::int64_t>(h->meta.pieceLength()) * 2;
            while (!amChoked && outstanding < maxOutstanding && h->storage.bytesLeft() > 0) {
                BlockRequest req;
                if (!h->picker.nextRequest(peerBf, endgame, &req)) {
                    break;
                }
                h->picker.markRequested(req);
                string pl(12, '\0');
                btWriteBe32(&pl[0], static_cast<std::uint32_t>(req.piece));
                btWriteBe32(&pl[4], static_cast<std::uint32_t>(req.offset));
                btWriteBe32(&pl[8], static_cast<std::uint32_t>(req.length));
                if (sock->sendall(btEncodeMessage(6, pl)) <= 0) {
                    return;
                }
                ++outstanding;
            }
        }
    } catch (...) {
    }
}

void TorrentSessionPrivate::connectPeer(shared_ptr<TorrentHandlePrivate> h, BtPeerAddr addr, bool preferUtp)
{
    if (!h || h->removed || h->paused) {
        return;
    }
    {
        lock_guard<std::mutex> lock(h->mutex_);
        if (static_cast<int>(h->connectedPeers.size()) >= maxPeers) {
            return;
        }
        if (h->connectedPeers.count(addr)) {
            return;
        }
    }
    shared_ptr<SocketLike> sock;
    if (preferUtp && utpEnabled) {
        shared_ptr<UtpSocket> utp = make_shared<UtpSocket>(addr.address.protocol());
        Timeout t(kBtPeerConnectTimeout);
        if (utp->connect(addr.address, addr.port)) {
            sock = asSocketLike(utp);
        }
    }
    if (!sock) {
        shared_ptr<Socket> tcp = make_shared<Socket>(addr.address.protocol(), Socket::TcpSocket);
        Timeout t(kBtPeerConnectTimeout);
        if (!tcp->connect(addr.address, addr.port)) {
            return;
        }
        sock = asSocketLike(tcp);
    }
    if (h->needsMetadata) {
        runMetadataPeer(h, sock, false, false);
    } else {
        runPeer(h, sock, false, false);
    }
}

void TorrentSessionPrivate::trackerLoop(shared_ptr<TorrentHandlePrivate> h)
{
    string event = "started";
    while (!h->removed && started) {
        if (!h->paused) {
            for (const string &url : h->effectiveTrackers()) {
                vector<BtPeerAddr> peers;
                utils::Url u(url);
                if (u.scheme() == "udp") {
                    peers = announceUdp(h, url, event);
                } else if (u.scheme() == "http" || u.scheme() == "https") {
                    peers = announceHttp(h, url, event);
                }
                addPeers(h, peers);
            }
            event.clear();
        }
        Coroutine::sleep(60.0f);
    }
}

void TorrentSessionPrivate::dhtLoop(shared_ptr<TorrentHandlePrivate> h)
{
    if (!dhtEnabled || !dht) {
        return;
    }
    bool announced = false;
    while (!h->removed && started) {
        if (!h->paused && dht->isOpen()) {
            vector<DhtPeer> found = dht->getPeers(h->effectiveInfoHash());
            vector<BtPeerAddr> peers;
            for (const DhtPeer &p : found) {
                peers.push_back(BtPeerAddr(p.address, p.port));
            }
            addPeers(h, peers);
            if (!announced) {
                dht->announcePeer(h->effectiveInfoHash(), effectiveListenPort());
                announced = true;
            }
        }
        Coroutine::sleep(90.0f);
    }
}

void TorrentSessionPrivate::maintainTorrent(shared_ptr<TorrentHandlePrivate> h)
{
    operations.spawn([this, h] { trackerLoop(h); });
    operations.spawn([this, h] { dhtLoop(h); });

    if (h->needsMetadata) {
        h->setState(TorrentStats::Metadata);
        h->notifyProgress();
        while (!h->removed && started && h->needsMetadata) {
            if (!h->paused) {
                vector<BtPeerAddr> toTry;
                {
                    lock_guard<std::mutex> lock(h->mutex_);
                    for (const BtPeerAddr &p : h->peers) {
                        if (!h->connectedPeers.count(p)) {
                            toTry.push_back(p);
                        }
                        if (static_cast<int>(toTry.size()) + static_cast<int>(h->connectedPeers.size())
                            >= maxPeers) {
                            break;
                        }
                    }
                }
                for (const BtPeerAddr &p : toTry) {
                    if (h->removed || !h->needsMetadata) {
                        break;
                    }
                    bool preferUtp = utpEnabled;
                    operations.spawn([this, h, p, preferUtp] { connectPeer(h, p, preferUtp); });
                }
            }
            if (h->metadataEvent.tryWait(2000)) {
                break;
            }
        }
        if (h->removed || !started) {
            return;
        }
        if (h->needsMetadata || !h->meta.isValid()) {
            h->setState(TorrentStats::Error, "failed to fetch magnet metadata");
            return;
        }
    }

    h->setState(TorrentStats::Checking);
    if (!h->storage.open(h->meta, downloadDir)) {
        h->setState(TorrentStats::Error, h->storage.errorString());
        return;
    }
    h->picker.reset(h->meta.pieceCount(), &h->storage);
    if (h->storage.bytesLeft() == 0) {
        h->setState(TorrentStats::Seeding);
        h->finishedEvent.set();
    } else {
        h->setState(TorrentStats::Downloading);
    }
    h->started = true;
    h->notifyProgress();

    while (!h->removed && started) {
        if (!h->paused) {
            vector<BtPeerAddr> toTry;
            {
                lock_guard<std::mutex> lock(h->mutex_);
                for (const BtPeerAddr &p : h->peers) {
                    if (!h->connectedPeers.count(p)) {
                        toTry.push_back(p);
                    }
                    if (static_cast<int>(toTry.size()) + static_cast<int>(h->connectedPeers.size()) >= maxPeers) {
                        break;
                    }
                }
            }
            for (const BtPeerAddr &p : toTry) {
                if (h->removed) {
                    break;
                }
                bool preferUtp = utpEnabled;
                operations.spawn([this, h, p, preferUtp] { connectPeer(h, p, preferUtp); });
            }
        }
        Coroutine::sleep(2.0f);
    }
}

void TorrentSessionPrivate::acceptTcpLoop()
{
    while (started && tcpListener) {
        Socket *raw = tcpListener->accept();
        if (!raw) {
            Coroutine::msleep(50);
            continue;
        }
        shared_ptr<SocketLike> sock = asSocketLike(shared_ptr<Socket>(raw));
        operations.spawn([this, sock] {
            try {
                Timeout t(kBtPeerConnectTimeout);
                string hs = sock->recvall(68);
                InfoHash hash;
                if (!btDecodeHandshake(hs, &hash, nullptr, nullptr)) {
                    return;
                }
                shared_ptr<TorrentHandlePrivate> target;
                {
                    lock_guard<std::mutex> lock(torrentsMutex);
                    for (auto &th : torrents) {
                        if (th->effectiveInfoHash() == hash) {
                            target = th;
                            break;
                        }
                    }
                }
                if (!target) {
                    return;
                }
                string reply = btEncodeHandshake(hash, peerId);
                if (sock->sendall(reply) != static_cast<std::int32_t>(reply.size())) {
                    return;
                }
                if (target->needsMetadata) {
                    runMetadataPeer(target, sock, true, true);
                } else {
                    runPeer(target, sock, true, true);
                }
            } catch (...) {
            }
        });
    }
}

void TorrentSessionPrivate::acceptUtpLoop()
{
    while (started && utpListener) {
        UtpSocket *raw = utpListener->accept();
        if (!raw) {
            Coroutine::msleep(50);
            continue;
        }
        shared_ptr<SocketLike> sock = asSocketLike(shared_ptr<UtpSocket>(raw));
        operations.spawn([this, sock] {
            try {
                Timeout t(kBtPeerConnectTimeout);
                string hs = sock->recvall(68);
                InfoHash hash;
                if (!btDecodeHandshake(hs, &hash, nullptr, nullptr)) {
                    return;
                }
                shared_ptr<TorrentHandlePrivate> target;
                {
                    lock_guard<std::mutex> lock(torrentsMutex);
                    for (auto &th : torrents) {
                        if (th->effectiveInfoHash() == hash) {
                            target = th;
                            break;
                        }
                    }
                }
                if (!target) {
                    return;
                }
                string reply = btEncodeHandshake(hash, peerId);
                if (sock->sendall(reply) != static_cast<std::int32_t>(reply.size())) {
                    return;
                }
                if (target->needsMetadata) {
                    runMetadataPeer(target, sock, true, true);
                } else {
                    runPeer(target, sock, true, true);
                }
            } catch (...) {
            }
        });
    }
}

TorrentSessionPrivate::TorrentSessionPrivate(TorrentSession *q, shared_ptr<DhtNode> node)
    : q_ptr(q)
    , listenPort(0)
    , maxPeers(50)
    , dhtEnabled(true)
    , utpEnabled(true)
    , started(false)
    , dht(node)
    , ownDht(false)
{
    downloadDir = ".";
    // Public DHT bootstrap nodes (router.bittorrent.com etc.)
    dhtBootstrap.push_back(DhtEndpoint(HostAddress("87.98.162.88"), 6881));
    dhtBootstrap.push_back(DhtEndpoint(HostAddress("67.215.246.10"), 6881));
    dhtBootstrap.push_back(DhtEndpoint(HostAddress("82.221.103.244"), 6881));
}

TorrentSessionPrivate::~TorrentSessionPrivate()
{
    stop();
}

void TorrentSessionPrivate::ensurePeerId()
{
    if (peerId.size() != 20) {
        peerId = makePeerId();
    }
}

std::uint16_t TorrentSessionPrivate::effectiveListenPort() const
{
    if (tcpListener && tcpListener->localPort() != 0) {
        return tcpListener->localPort();
    }
    if (utpListener && utpListener->localPort() != 0) {
        return utpListener->localPort();
    }
    return listenPort;
}

void TorrentSessionPrivate::start()
{
    if (started) {
        return;
    }
    ensurePeerId();
    errorString.clear();

    tcpListener = make_shared<Socket>(HostAddress::IPv4Protocol, Socket::TcpSocket);
    if (!tcpListener->bind(listenPort) || !tcpListener->listen(64)) {
        errorString = "tcp listen failed: " + tcpListener->errorString();
        tcpListener.reset();
        return;
    }

    if (utpEnabled) {
        utpListener = make_shared<UtpSocket>(HostAddress::IPv4Protocol);
        std::uint16_t utpPort = tcpListener->localPort();
        if (!utpListener->bind(utpPort) || !utpListener->listen(64)) {
            // µTP may fail to share port on some platforms — keep TCP only
            utpListener.reset();
        }
    }

    if (dhtEnabled) {
        if (!dht) {
            dht = make_shared<DhtNode>();
            ownDht = true;
        }
        if (!dht->isOpen()) {
            // Prefer a distinct UDP port for DHT if µTP took listen port
            std::uint16_t dhtPort = 0;
            if (!dht->open(dhtPort)) {
                errorString = "dht open failed: " + dht->errorString();
            } else {
                dht->bootstrap(dhtBootstrap);
            }
        }
    }

    started = true;
    operations.spawn([this] { acceptTcpLoop(); });
    if (utpListener) {
        operations.spawn([this] { acceptUtpLoop(); });
    }

    lock_guard<std::mutex> lock(torrentsMutex);
    for (auto &h : torrents) {
        if (!h->started) {
            operations.spawn([this, h] { maintainTorrent(h); });
        }
    }
}

void TorrentSessionPrivate::stop()
{
    if (!started) {
        return;
    }
    started = false;
    {
        lock_guard<std::mutex> lock(torrentsMutex);
        for (auto &h : torrents) {
            h->removed = true;
            h->finishedEvent.set();
        }
    }
    if (tcpListener) {
        tcpListener->abort();
        tcpListener.reset();
    }
    if (utpListener) {
        utpListener->abort();
        utpListener.reset();
    }
    operations.killall();
    if (ownDht && dht) {
        dht->close();
    }
}

TorrentHandle TorrentSessionPrivate::addTorrent(const TorrentMeta &meta)
{
    TorrentHandle invalid;
    if (!meta.isValid()) {
        errorString = meta.errorString();
        return invalid;
    }
    auto h = make_shared<TorrentHandlePrivate>();
    h->meta = meta;
    h->infoDictRaw = meta.infoDict();
    h->magnetInfoHash = meta.infoHash();
    h->needsMetadata = false;
    h->session = shared_ptr<TorrentSessionPrivate>();  // not owned via shared_from_this
    {
        lock_guard<std::mutex> lock(torrentsMutex);
        torrents.push_back(h);
    }
    if (started) {
        operations.spawn([this, h] { maintainTorrent(h); });
    }
    return TorrentHandle(h);
}

TorrentHandle TorrentSessionPrivate::addMagnet(const MagnetLink &magnet)
{
    TorrentHandle invalid;
    if (!magnet.isValid()) {
        errorString = magnet.errorString();
        return invalid;
    }
    auto h = make_shared<TorrentHandlePrivate>();
    h->needsMetadata = true;
    h->magnetInfoHash = magnet.infoHash();
    h->magnetTrackers = magnet.trackers();
    h->magnetDisplayName = magnet.displayName();
    h->session = shared_ptr<TorrentSessionPrivate>();
    vector<BtPeerAddr> hints;
    for (const MagnetPeerHint &hint : magnet.peers()) {
        if (!hint.isValid()) {
            continue;
        }
        HostAddress addr;
        if (addr.setAddress(hint.host())) {
            hints.push_back(BtPeerAddr(addr, hint.port()));
        } else {
            vector<HostAddress> resolved = Socket::resolve(hint.host());
            for (const HostAddress &a : resolved) {
                if (!a.isNull()) {
                    hints.push_back(BtPeerAddr(a, hint.port()));
                }
            }
        }
    }
    addPeers(h, hints);
    {
        lock_guard<std::mutex> lock(torrentsMutex);
        torrents.push_back(h);
    }
    if (started) {
        operations.spawn([this, h] { maintainTorrent(h); });
    }
    return TorrentHandle(h);
}

// ---------------------------------------------------------------------------
// TorrentSession public API
// ---------------------------------------------------------------------------

TorrentSession::TorrentSession()
    : d_ptr(new TorrentSessionPrivate(this))
{
}

TorrentSession::TorrentSession(shared_ptr<DhtNode> node)
    : d_ptr(new TorrentSessionPrivate(this, node))
{
}

TorrentSession::~TorrentSession()
{
    delete d_ptr;
}

void TorrentSession::setDownloadDir(const string &dir)
{
    d_ptr->downloadDir = dir;
}

string TorrentSession::downloadDir() const
{
    return d_ptr->downloadDir;
}

void TorrentSession::setListenPort(std::uint16_t port)
{
    d_ptr->listenPort = port;
}

std::uint16_t TorrentSession::listenPort() const
{
    return d_ptr->effectiveListenPort();
}

void TorrentSession::setPeerId(const string &peerId20)
{
    d_ptr->peerId = peerId20;
}

string TorrentSession::peerId() const
{
    return d_ptr->peerId;
}

void TorrentSession::setMaxPeers(int n)
{
    d_ptr->maxPeers = n > 0 ? n : 1;
}

int TorrentSession::maxPeers() const
{
    return d_ptr->maxPeers;
}

void TorrentSession::setDhtNode(shared_ptr<DhtNode> node)
{
    d_ptr->dht = node;
    d_ptr->ownDht = false;
}

shared_ptr<DhtNode> TorrentSession::dhtNode() const
{
    return d_ptr->dht;
}

void TorrentSession::setDhtEnabled(bool on)
{
    d_ptr->dhtEnabled = on;
}

bool TorrentSession::dhtEnabled() const
{
    return d_ptr->dhtEnabled;
}

void TorrentSession::setUtpEnabled(bool on)
{
    d_ptr->utpEnabled = on;
}

bool TorrentSession::utpEnabled() const
{
    return d_ptr->utpEnabled;
}

void TorrentSession::setDhtBootstrap(const vector<DhtEndpoint> &seeds)
{
    d_ptr->dhtBootstrap = seeds;
}

TorrentHandle TorrentSession::addTorrent(const TorrentMeta &meta)
{
    return d_ptr->addTorrent(meta);
}

TorrentHandle TorrentSession::addTorrentFile(const string &path)
{
    return d_ptr->addTorrent(TorrentMeta::fromFile(path));
}

TorrentHandle TorrentSession::addMagnet(const MagnetLink &magnet)
{
    return d_ptr->addMagnet(magnet);
}

TorrentHandle TorrentSession::addMagnetUri(const string &uri)
{
    return d_ptr->addMagnet(MagnetLink::parse(uri));
}

void TorrentSession::start()
{
    d_ptr->start();
}

void TorrentSession::stop()
{
    d_ptr->stop();
}

bool TorrentSession::isStarted() const
{
    return d_ptr->started;
}

string TorrentSession::errorString() const
{
    return d_ptr->errorString;
}

}  // namespace qtng
