#include "qtng/http3.h"

#include <algorithm>

#include "qtng/private/quic_p.h"
#include "qtng/quic.h"

using namespace std;

namespace qtng {

namespace {

// QPACK static table subset (RFC 9204 Appendix A). Entries: (name, value).
const vector<pair<string, string>> kQpackStaticTable = {
    {":authority", ""},          // 0
    {":path", "/"},              // 1
    {"age", "0"},                // 2
    {"content-disposition", ""}, // 3
    {"content-length", "0"},     // 4
    {"cookie", ""},              // 5
    {"date", ""},                // 6
    {"etag", ""},                // 7
    {"if-modified-since", ""},   // 8
    {"if-none-match", ""},       // 9
    {"last-modified", ""},       // 10
    {"link", ""},                // 11
    {"location", ""},            // 12
    {"referer", ""},             // 13
    {"set-cookie", ""},          // 14
    {":method", "CONNECT"},      // 15
    {":method", "DELETE"},       // 16
    {":method", "GET"},          // 17
    {":method", "HEAD"},         // 18
    {":method", "OPTIONS"},      // 19
    {":method", "POST"},         // 20
    {":method", "PUT"},          // 21
    {":scheme", "http"},         // 22
    {":scheme", "https"},        // 23
    {":status", "103"},          // 24
    {":status", "200"},          // 25
    {":status", "304"},          // 26
    {":status", "404"},          // 27
    {":status", "503"},          // 28
    {"accept", "*/*"},           // 29
    {"accept", "application/dns-message"},  // 30
    {"accept-encoding", "gzip, deflate, br"},  // 31
    {"accept-ranges", "bytes"},  // 32
    {"access-control-allow-headers", "cache-control"},  // 33
    {"access-control-allow-headers", "content-type"},   // 34
    {"access-control-allow-origin", "*"},               // 35
    {"cache-control", "max-age=0"},                     // 36
    {"cache-control", "max-age=2592000"},               // 37
    {"cache-control", "max-age=604800"},                // 38
    {"cache-control", "no-cache"},                      // 39
    {"cache-control", "no-store"},                      // 40
    {"cache-control", "public, max-age=31536000"},      // 41
    {"content-encoding", "br"},                         // 42
    {"content-encoding", "gzip"},                       // 43
    {"content-type", "application/dns-message"},        // 44
    {"content-type", "application/javascript"},         // 45
    {"content-type", "application/json"},               // 46
    {"content-type", "application/x-www-form-urlencoded"},  // 47
    {"content-type", "image/gif"},                      // 48
    {"content-type", "image/jpeg"},                     // 49
    {"content-type", "image/png"},                      // 50
    {"content-type", "text/css"},                       // 51
    {"content-type", "text/html; charset=utf-8"},       // 52
    {"content-type", "text/plain"},                     // 53
    {"content-type", "text/plain;charset=utf-8"},       // 54
    {"range", "bytes=0-"},                              // 55
    {"strict-transport-security", "max-age=31536000"},  // 56
    {"strict-transport-security", "max-age=31536000; includesubdomains"},  // 57
    {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},  // 58
    {"vary", "accept-encoding"},                        // 59
    {"vary", "origin"},                                 // 60
    {"x-content-type-options", "nosniff"},              // 61
    {"x-xss-protection", "1; mode=block"},              // 62
};

// Encode a QPACK prefixed integer (RFC 9204 §4.1.1).
void qpackEncodeInt(uint64_t value, int prefixBits, uint8_t prefixValue, string *out)
{
    const uint64_t maxPrefix = (1ull << prefixBits) - 1;
    if (value < maxPrefix) {
        out->push_back(static_cast<char>(prefixValue | value));
        return;
    }
    out->push_back(static_cast<char>(prefixValue | maxPrefix));
    value -= maxPrefix;
    while (value >= 128) {
        out->push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out->push_back(static_cast<char>(value));
}

// Decode a QPACK prefixed integer; advances *offset.
bool qpackDecodeInt(const char *data, size_t size, size_t *offset, int prefixBits, uint64_t *out)
{
    if (*offset >= size) {
        return false;
    }
    const uint64_t maxPrefix = (1ull << prefixBits) - 1;
    uint8_t first = static_cast<uint8_t>(data[(*offset)++]);
    uint64_t value = first & maxPrefix;
    if (value < maxPrefix) {
        *out = value;
        return true;
    }
    uint64_t shift = 0;
    while (true) {
        if (*offset >= size) {
            return false;
        }
        uint8_t b = static_cast<uint8_t>(data[(*offset)++]);
        value += static_cast<uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            break;
        }
        shift += 7;
        if (shift > 63) {
            return false;
        }
    }
    *out = value;
    return true;
}

// QPACK string literal: 1-bit Huffman (0 = off) + 7-bit prefixed length.
void qpackEncodeString(string *out, const string &s)
{
    qpackEncodeInt(s.size(), 7, 0, out);
    out->append(s);
}

bool qpackDecodeString(const char *data, size_t size, size_t *offset, string *out)
{
    uint64_t len = 0;
    if (!qpackDecodeInt(data, size, offset, 7, &len) || *offset + len > size) {
        return false;
    }
    out->assign(data + *offset, static_cast<size_t>(len));
    *offset += static_cast<size_t>(len);
    return true;
}

}  // namespace

int qpackStaticTableIndex(const string &name)
{
    for (size_t i = 0; i < kQpackStaticTable.size(); ++i) {
        if (kQpackStaticTable[i].first == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

string qpackEncodeHeaders(const vector<pair<string, string>> &headers)
{
    string out;
    for (const auto &kv : headers) {
        // Exact static-table match: indexed field line.
        bool indexed = false;
        for (size_t i = 0; i < kQpackStaticTable.size(); ++i) {
            if (kQpackStaticTable[i] == kv) {
                qpackEncodeInt(i, 6, 0xc0, &out);
                indexed = true;
                break;
            }
        }
        if (indexed) {
            continue;
        }
        const int nameIdx = qpackStaticTableIndex(kv.first);
        if (nameIdx >= 0) {
            // Literal field line with static name reference.
            qpackEncodeInt(static_cast<uint64_t>(nameIdx), 6, 0x40, &out);
            qpackEncodeString(&out, kv.second);
        } else {
            // Literal field line with literal name (5-bit name length prefix).
            qpackEncodeInt(kv.first.size(), 5, 0x20, &out);
            out.append(kv.first);
            qpackEncodeString(&out, kv.second);
        }
    }
    return out;
}

bool qpackDecodeHeaders(const string &data, vector<pair<string, string>> *headers)
{
    size_t off = 0;
    while (off < data.size()) {
        const uint8_t first = static_cast<uint8_t>(data[off]);
        if (first & 0x80) {
            // Indexed field line (static table).
            uint64_t idx = 0;
            if (!qpackDecodeInt(data.data(), data.size(), &off, 6, &idx) || idx >= kQpackStaticTable.size()) {
                return false;
            }
            headers->push_back(kQpackStaticTable[idx]);
        } else if (first & 0x40) {
            // Literal field line with name reference.
            uint64_t idx = 0;
            if (!qpackDecodeInt(data.data(), data.size(), &off, 6, &idx) || idx >= kQpackStaticTable.size()) {
                return false;
            }
            string value;
            if (!qpackDecodeString(data.data(), data.size(), &off, &value)) {
                return false;
            }
            headers->push_back({kQpackStaticTable[idx].first, std::move(value)});
        } else if (first & 0x20) {
            // Literal field line with literal name (5-bit length prefix).
            uint64_t nameLen = 0;
            if (!qpackDecodeInt(data.data(), data.size(), &off, 5, &nameLen) || off + nameLen > data.size()) {
                return false;
            }
            string name = data.substr(off, static_cast<size_t>(nameLen));
            off += static_cast<size_t>(nameLen);
            string value;
            if (!qpackDecodeString(data.data(), data.size(), &off, &value)) {
                return false;
            }
            headers->push_back({std::move(name), std::move(value)});
        } else {
            // Unsupported instruction.
            return false;
        }
    }
    return true;
}

class Http3StreamPrivate
{
public:
    explicit Http3StreamPrivate(std::shared_ptr<QuicStream> s);

    std::shared_ptr<QuicStream> stream;
};

class Http3ConnectionPrivate
{
public:
    explicit Http3ConnectionPrivate(std::shared_ptr<QuicConnection> conn);

    std::shared_ptr<QuicConnection> conn;
    bool initialized = false;

    void ensureControlStreams();
};

Http3StreamPrivate::Http3StreamPrivate(std::shared_ptr<QuicStream> s)
    : stream(std::move(s))
{
}

Http3ConnectionPrivate::Http3ConnectionPrivate(std::shared_ptr<QuicConnection> c)
    : conn(std::move(c))
{
}

void Http3ConnectionPrivate::ensureControlStreams()
{
    if (initialized || !conn) {
        return;
    }
    initialized = true;
    // Control stream (RFC 9114 §6.2.1): stream type 0x00, SETTINGS frame, and
    // MUST stay open for the lifetime of the connection.
    std::shared_ptr<QuicStream> control = conn->openUniStream();
    if (control) {
        string type;
        quicEncodeVarint(0x0, &type);
        control->sendall(type);
        string body;
        quicEncodeVarint(0x2, &body);  // QPACK_MAX_TABLE_CAPACITY
        quicEncodeVarint(0, &body);
        quicEncodeVarint(0x6, &body);  // QPACK_BLOCKED_STREAMS
        quicEncodeVarint(0, &body);
        string frame;
        quicEncodeVarint(H3Settings, &frame);
        quicEncodeVarint(body.size(), &frame);
        frame.append(body);
        control->sendall(frame);
        // No close(): the control stream lives until the connection ends.
    }
    // QPACK encoder and decoder streams (RFC 9204 §4.2): each endpoint creates
    // both, since both directions may encode HEADERS. With only the static table
    // in use no instructions are ever sent, but the stream types still declare
    // the streams so a peer can locate them.
    for (uint64_t qpackType : {uint64_t(0x2), uint64_t(0x3)}) {
        std::shared_ptr<QuicStream> qpack = conn->openUniStream();
        if (qpack) {
            string type;
            quicEncodeVarint(qpackType, &type);
            qpack->sendall(type);
        }
    }
}

// --- Http3Stream ---

Http3Stream::Http3Stream(Http3StreamPrivate *d)
    : d_ptr(d)
{
}

Http3Stream::~Http3Stream()
{
    delete d_ptr;
}

uint64_t Http3Stream::streamId() const
{
    const NG_D(Http3Stream);
    return d->stream->streamId();
}

bool Http3Stream::isValid() const
{
    const NG_D(Http3Stream);
    return d->stream && d->stream->isValid();
}

int32_t Http3Stream::sendData(const char *data, int32_t size)
{
    NG_D(Http3Stream);
    string frame;
    quicEncodeVarint(H3Data, &frame);
    quicEncodeVarint(static_cast<uint64_t>(size), &frame);
    frame.append(data, static_cast<size_t>(size));
    return d->stream->sendall(frame) == static_cast<int32_t>(frame.size()) ? size : -1;
}

int32_t Http3Stream::sendData(const string &data)
{
    return sendData(data.data(), static_cast<int32_t>(data.size()));
}

bool Http3Stream::sendHeaders(const vector<pair<string, string>> &headers)
{
    NG_D(Http3Stream);
    const string qp = qpackEncodeHeaders(headers);
    string frame;
    quicEncodeVarint(H3Headers, &frame);
    quicEncodeVarint(qp.size(), &frame);
    frame.append(qp);
    return d->stream->sendall(frame) == static_cast<int32_t>(frame.size());
}

namespace {

// Read one varint from the stream into a small internal buffer.
bool readVarintFromStream(std::shared_ptr<QuicStream> stream, uint64_t *out)
{
    uint8_t first = 0;
    const int32_t n = stream->recv(reinterpret_cast<char *>(&first), 1);
    if (n != 1) {
        return false;
    }
    const size_t len = 1u << (first >> 6);
    if (len == 1) {
        *out = first & 0x3f;
        return true;
    }
    string rest(static_cast<size_t>(len - 1), '\0');
    if (stream->recvall(&rest[0], static_cast<int32_t>(len - 1)) != static_cast<int32_t>(len - 1)) {
        return false;
    }
    uint64_t value = first & 0x3f;
    for (size_t i = 0; i + 1 < len; ++i) {
        value = (value << 8) | static_cast<unsigned char>(rest[i]);
    }
    *out = value;
    return true;
}

}  // namespace

int32_t Http3Stream::recvFrame(uint8_t *frameType, string *payload)
{
    NG_D(Http3Stream);
    uint64_t type = 0, len = 0;
    if (!readVarintFromStream(d->stream, &type) || !readVarintFromStream(d->stream, &len)) {
        return -1;
    }
    if (len > 16u * 1024 * 1024) {
        return -1;
    }
    string body(static_cast<size_t>(len), '\0');
    const int32_t got = d->stream->recvall(&body[0], static_cast<int32_t>(len));
    if (got != static_cast<int32_t>(len)) {
        return -1;
    }
    if (frameType) {
        *frameType = static_cast<uint8_t>(type);
    }
    if (payload) {
        *payload = std::move(body);
    }
    return static_cast<int32_t>(len);
}

void Http3Stream::close()
{
    NG_D(Http3Stream);
    d->stream->close();
}

// --- Http3Connection ---

Http3Connection::Http3Connection(std::shared_ptr<QuicConnection> conn)
    : d_ptr(new Http3ConnectionPrivate(std::move(conn)))
{
}

Http3Connection::~Http3Connection()
{
    delete d_ptr;
}

bool Http3Connection::isValid() const
{
    const NG_D(Http3Connection);
    return d->conn && d->conn->isValid();
}

bool Http3Connection::isServer() const
{
    const NG_D(Http3Connection);
    return !d->conn->isClientSide();
}

std::shared_ptr<Http3Stream> Http3Connection::openStream()
{
    NG_D(Http3Connection);
    d->ensureControlStreams();
    std::shared_ptr<QuicStream> s = d->conn->openStream();
    if (!s) {
        return std::shared_ptr<Http3Stream>();
    }
    Http3StreamPrivate *sd = new Http3StreamPrivate(s);
    return std::shared_ptr<Http3Stream>(new Http3Stream(sd));
}

std::shared_ptr<Http3Stream> Http3Connection::acceptStream()
{
    NG_D(Http3Connection);
    d->ensureControlStreams();
    std::shared_ptr<QuicStream> s = d->conn->acceptStream();
    if (!s) {
        return std::shared_ptr<Http3Stream>();
    }
    Http3StreamPrivate *sd = new Http3StreamPrivate(s);
    return std::shared_ptr<Http3Stream>(new Http3Stream(sd));
}

std::string Http3Connection::encodeHeaders(
        const std::vector<std::pair<std::string, std::string>> &headers) const
{
    return qpackEncodeHeaders(headers);
}

bool Http3Connection::decodeHeaders(const string &payload,
                                    vector<pair<string, string>> *headers) const
{
    return qpackDecodeHeaders(payload, headers);
}

void Http3Connection::close()
{
    NG_D(Http3Connection);
    if (d->conn) {
        d->conn->close();
    }
}

}  // namespace qtng
