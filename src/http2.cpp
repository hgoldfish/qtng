#include "qtng/private/http2_p.h"
#include "qtng/private/http_p.h"
#include "qtng/private/http_protocol_p.h"
#include "qtng/private/hpack_p.h"
#include "qtng/io_utils.h"
#include "qtng/utils/string_utils.h"
#include "qtng/utils/url.h"
#include "qtng/utils/logging.h"
#include "qtng/coroutine.h"
#include "qtng/eventloop.h"
#include "qtng/locks.h"
#include "qtng/socket.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>

using namespace std;

NG_LOGGER("qtng.http2");

namespace qtng {

namespace {

enum Http2FrameType : uint8_t {
    FrameData = 0x0,
    FrameHeaders = 0x1,
    FramePriority = 0x2,
    FrameRstStream = 0x3,
    FrameSettings = 0x4,
    FramePushPromise = 0x5,
    FramePing = 0x6,
    FrameGoAway = 0x7,
    FrameWindowUpdate = 0x8,
    FrameContinuation = 0x9,
};

enum Http2Flags : uint8_t {
    FlagEndStream = 0x1,
    FlagEndHeaders = 0x4,
    FlagPadded = 0x8,
    FlagPriority = 0x20,
    FlagAck = 0x1,
};

enum Http2ErrorCode : uint32_t {
    ErrorNoError = 0x0,
    ErrorProtocolError = 0x1,
    ErrorInternalError = 0x2,
    ErrorFlowControlError = 0x3,
    ErrorSettingsTimeout = 0x4,
    ErrorStreamClosed = 0x5,
    ErrorFrameSizeError = 0x6,
    ErrorRefusedStream = 0x7,
    ErrorCancel = 0x8,
    ErrorCompressionError = 0x9,
    ErrorEnhanceYourCalm = 0xa,
};

enum Http2SettingsId : uint16_t {
    SettingsHeaderTableSize = 0x1,
    SettingsEnablePush = 0x2,
    SettingsMaxConcurrentStreams = 0x3,
    SettingsInitialWindowSize = 0x4,
    SettingsMaxFrameSize = 0x5,
    SettingsMaxHeaderListSize = 0x6,
};

const char kClientPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
const int32_t kDefaultWindow = 65535;
const uint32_t kDefaultMaxFrameSize = 16384;

string urlResourcePath(const utils::Url &url)
{
    string path = url.path();
    if (path.empty()) {
        path = "/";
    }
    if (!url.query().empty()) {
        path += '?' + url.query();
    }
    return path;
}

void writeUint24(char *p, uint32_t v)
{
    p[0] = static_cast<char>((v >> 16) & 0xff);
    p[1] = static_cast<char>((v >> 8) & 0xff);
    p[2] = static_cast<char>(v & 0xff);
}

void writeUint32(char *p, uint32_t v)
{
    p[0] = static_cast<char>((v >> 24) & 0xff);
    p[1] = static_cast<char>((v >> 16) & 0xff);
    p[2] = static_cast<char>((v >> 8) & 0xff);
    p[3] = static_cast<char>(v & 0xff);
}

uint32_t readUint24(const char *p)
{
    return (static_cast<uint8_t>(p[0]) << 16) | (static_cast<uint8_t>(p[1]) << 8) | static_cast<uint8_t>(p[2]);
}

uint32_t readUint32(const char *p)
{
    return (static_cast<uint8_t>(p[0]) << 24) | (static_cast<uint8_t>(p[1]) << 16)
            | (static_cast<uint8_t>(p[2]) << 8) | static_cast<uint8_t>(p[3]);
}

string buildFrame(uint8_t type, uint8_t flags, uint32_t streamId, const string &payload)
{
    string frame(9 + payload.size(), '\0');
    writeUint24(&frame[0], static_cast<uint32_t>(payload.size()));
    frame[3] = static_cast<char>(type);
    frame[4] = static_cast<char>(flags);
    writeUint32(&frame[5], streamId & 0x7fffffff);
    if (!payload.empty()) {
        memcpy(&frame[9], payload.data(), payload.size());
    }
    return frame;
}

}  // namespace

class Http2ClientSessionPrivate;

struct Http2StreamState
{
    uint32_t id = 0;
    Event headersReady;
    Event dataReady;
    Event windowUpdated;
    Event closed;
    vector<HpackHeader> responseHeaders;
    vector<HpackHeader> trailers;
    string body;
    size_t bodyReadOffset = 0;
    int64_t receivedBodyBytes = 0;
    bool endHeaders = false;
    bool endStream = false;
    bool reset = false;
    bool reserved = false;  // counts toward MAX_CONCURRENT_STREAMS
    uint32_t errorCode = 0;
    int32_t sendWindow = kDefaultWindow;
    int32_t recvWindow = kDefaultWindow;
};

class Http2BodySocketLike : public SocketLike
{
public:
    Http2BodySocketLike(shared_ptr<Http2ClientSessionPrivate> session, shared_ptr<Http2StreamState> stream)
        : session(session)
        , stream(stream)
        , closed(false)
    {
    }

    Socket::SocketError error() const override
    {
        return (stream && stream->reset) ? Socket::RemoteHostClosedError : Socket::NoError;
    }
    string errorString() const override { return stream && stream->reset ? "http2 stream reset" : string(); }
    bool isValid() const override { return !closed && stream && !stream->reset && (!stream->endStream || readable()); }
    HostAddress localAddress() const override { return HostAddress(); }
    uint16_t localPort() const override { return 0; }
    HostAddress peerAddress() const override { return HostAddress(); }
    string peerName() const override { return string(); }
    uint16_t peerPort() const override { return 0; }
    intptr_t fileno() const override { return -1; }
    Socket::SocketType type() const override { return Socket::TcpSocket; }
    Socket::SocketState state() const override
    {
        return isValid() ? Socket::ConnectedState : Socket::UnconnectedState;
    }
    HostAddress::NetworkLayerProtocol protocol() const override { return HostAddress::UnknownNetworkLayerProtocol; }
    string localAddressURI() const override { return string(); }
    string peerAddressURI() const override { return string(); }
    shared_ptr<SocketLike> accept() override { return shared_ptr<SocketLike>(); }
    Socket *acceptRaw() override { return nullptr; }
    bool bind(const HostAddress &, uint16_t, Socket::BindMode) override { return false; }
    bool bind(uint16_t, Socket::BindMode) override { return false; }
    bool connect(const HostAddress &, uint16_t) override { return false; }
    bool connect(const string &, uint16_t, shared_ptr<SocketDnsCache>) override { return false; }
    void close() override;
    void abort() override;
    bool listen(int) override { return false; }
    bool setOption(Socket::SocketOption, int) override { return false; }
    int option(Socket::SocketOption) const override { return -1; }
    int32_t peek(char *data, int32_t size) override;
    int32_t peekRaw(char *data, int32_t size) override { return peek(data, size); }
    int32_t recv(char *data, int32_t size) override;
    int32_t recvall(char *data, int32_t size) override;
    int32_t send(const char *, int32_t) override { return -1; }
    int32_t sendall(const char *, int32_t) override { return -1; }
    string recv(int32_t size) override;
    string recvall(int32_t size) override;
    int32_t send(const string &) override { return -1; }
    int32_t sendall(const string &) override { return -1; }

    bool readable() const
    {
        return stream && stream->bodyReadOffset < stream->body.size();
    }

    shared_ptr<Http2ClientSessionPrivate> session;
    shared_ptr<Http2StreamState> stream;
    bool closed;
};

class Http2ClientSessionPrivate : public enable_shared_from_this<Http2ClientSessionPrivate>
{
public:
    Http2ClientSessionPrivate(shared_ptr<SocketLike> connection, int debugLevel)
        : connection(connection)
        , debugLevel(debugLevel)
        , nextStreamId(1)
        , connectionSendWindow(kDefaultWindow)
        , connectionRecvWindow(kDefaultWindow)
        , remoteInitialWindow(kDefaultWindow)
        , maxFrameSize(kDefaultMaxFrameSize)
        , maxConcurrentStreams(UINT32_MAX)
        , activeStreamCount(0)
        , goAwayLastStreamId(UINT32_MAX)
        , valid(false)
        , goingAway(false)
        , peerSettingsSeen(false)
    {
    }

    bool start();
    void close();
    void exchange(HttpSessionPrivate *session, HttpRequest &request, HttpResponse &response);
    void readerLoop();

    bool sendRaw(const string &data);
    bool sendRawUnlocked(const string &data);
    bool sendFrame(uint8_t type, uint8_t flags, uint32_t streamId, const string &payload);
    bool sendFrameUnlocked(uint8_t type, uint8_t flags, uint32_t streamId, const string &payload);
    bool readFrame(uint8_t *type, uint8_t *flags, uint32_t *streamId, string *payload);
    void handleFrame(uint8_t type, uint8_t flags, uint32_t streamId, const string &payload);

    shared_ptr<Http2StreamState> createStream();
    void releaseStream(uint32_t streamId, bool sendRst = false, uint32_t code = ErrorCancel);
    void finishStreamError(shared_ptr<Http2StreamState> stream, uint32_t code);
    void finishStreamsWhere(uint32_t code, bool onlyAboveLastStreamId, uint32_t lastStreamId);
    bool waitPeerSettings();
    bool waitSendWindow(shared_ptr<Http2StreamState> stream, int32_t needed);
    bool sendHeaderBlock(uint32_t streamId, const string &block, bool endStream);
    bool sendBody(shared_ptr<Http2StreamState> stream, shared_ptr<FileLike> body);
    void sendWindowUpdate(uint32_t streamId, uint32_t increment);
    void applyInitialWindowSize(uint32_t newSize);
    void failConnection(uint32_t code);
    void notifyWindows();
    void consumeRecvWindow(shared_ptr<Http2StreamState> stream, uint32_t frameLength);
    void unreserveStream(shared_ptr<Http2StreamState> stream);
    void wakeStream(shared_ptr<Http2StreamState> stream);

    shared_ptr<SocketLike> connection;
    Lock writeLock;
    shared_ptr<Coroutine> reader;
    HpackEncoder encoder;
    HpackDecoder decoder;
    map<uint32_t, shared_ptr<Http2StreamState>> streams;
    Event peerSettingsEvent;
    Event streamSlotsAvailable;
    int debugLevel;
    uint32_t nextStreamId;
    int32_t connectionSendWindow;
    int32_t connectionRecvWindow;
    int32_t remoteInitialWindow;
    uint32_t maxFrameSize;
    uint32_t maxConcurrentStreams;
    uint32_t activeStreamCount;
    uint32_t goAwayLastStreamId;
    bool valid;
    bool goingAway;
    bool peerSettingsSeen;
    string headerBlockBuffer;
    uint32_t headerBlockStream = 0;
    uint8_t headerBlockFlags = 0;
};

void Http2BodySocketLike::close()
{
    if (closed) {
        return;
    }
    closed = true;
    if (session && stream) {
        session->releaseStream(stream->id, !stream->endStream && !stream->reset, ErrorCancel);
    }
}

void Http2BodySocketLike::abort()
{
    closed = true;
    if (session && stream) {
        session->releaseStream(stream->id, true, ErrorCancel);
    }
}

int32_t Http2BodySocketLike::peek(char *data, int32_t size)
{
    if (!stream || size <= 0) {
        return -1;
    }
    if (stream->bodyReadOffset >= stream->body.size()) {
        return stream->endStream || stream->reset ? 0 : -1;
    }
    size_t avail = stream->body.size() - stream->bodyReadOffset;
    size_t n = min(avail, static_cast<size_t>(size));
    memcpy(data, stream->body.data() + stream->bodyReadOffset, n);
    return static_cast<int32_t>(n);
}

int32_t Http2BodySocketLike::recv(char *data, int32_t size)
{
    if (closed || !stream || size <= 0) {
        return -1;
    }
    while (stream->bodyReadOffset >= stream->body.size() && !stream->endStream && !stream->reset) {
        stream->dataReady.clear();
        if (stream->bodyReadOffset < stream->body.size() || stream->endStream || stream->reset) {
            break;
        }
        if (!stream->dataReady.tryWait()) {
            return -1;
        }
    }
    if (stream->reset && stream->bodyReadOffset >= stream->body.size()) {
        return -1;
    }
    if (stream->bodyReadOffset >= stream->body.size()) {
        return 0;
    }
    size_t avail = stream->body.size() - stream->bodyReadOffset;
    size_t n = min(avail, static_cast<size_t>(size));
    memcpy(data, stream->body.data() + stream->bodyReadOffset, n);
    stream->bodyReadOffset += n;
    if (stream->bodyReadOffset > 64 * 1024) {
        stream->body.erase(0, stream->bodyReadOffset);
        stream->bodyReadOffset = 0;
    }
    return static_cast<int32_t>(n);
}

int32_t Http2BodySocketLike::recvall(char *data, int32_t size)
{
    int32_t total = 0;
    while (total < size) {
        int32_t n = recv(data + total, size - total);
        if (n < 0) {
            return total > 0 ? total : -1;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    return total;
}

string Http2BodySocketLike::recv(int32_t size)
{
    string buf;
    buf.resize(static_cast<size_t>(size));
    int32_t n = recv(&buf[0], size);
    if (n > 0) {
        buf.resize(static_cast<size_t>(n));
        return buf;
    }
    return string();
}

string Http2BodySocketLike::recvall(int32_t size)
{
    string buf;
    buf.resize(static_cast<size_t>(size));
    int32_t n = recvall(&buf[0], size);
    if (n > 0) {
        buf.resize(static_cast<size_t>(n));
        return buf;
    }
    return string();
}

bool Http2ClientSessionPrivate::sendRawUnlocked(const string &data)
{
    return connection && connection->sendall(data) == static_cast<int32_t>(data.size());
}

bool Http2ClientSessionPrivate::sendRaw(const string &data)
{
    ScopedLock<Lock> locker(writeLock);
    if (!locker.isSuccess()) {
        return false;
    }
    return sendRawUnlocked(data);
}

bool Http2ClientSessionPrivate::sendFrameUnlocked(uint8_t type, uint8_t flags, uint32_t streamId,
                                                  const string &payload)
{
    if (payload.size() > maxFrameSize) {
        return false;
    }
    return sendRawUnlocked(buildFrame(type, flags, streamId, payload));
}

bool Http2ClientSessionPrivate::sendFrame(uint8_t type, uint8_t flags, uint32_t streamId, const string &payload)
{
    ScopedLock<Lock> locker(writeLock);
    if (!locker.isSuccess()) {
        return false;
    }
    return sendFrameUnlocked(type, flags, streamId, payload);
}

bool Http2ClientSessionPrivate::readFrame(uint8_t *type, uint8_t *flags, uint32_t *streamId, string *payload)
{
    const string header = connection->recvall(9);
    if (header.size() != 9) {
        return false;
    }
    uint32_t length = readUint24(header.data());
    *type = static_cast<uint8_t>(header[3]);
    *flags = static_cast<uint8_t>(header[4]);
    *streamId = readUint32(header.data() + 5) & 0x7fffffff;
    if (length > maxFrameSize) {
        failConnection(ErrorFrameSizeError);
        return false;
    }
    if (length == 0) {
        payload->clear();
        return true;
    }
    *payload = connection->recvall(static_cast<int32_t>(length));
    return payload->size() == length;
}

void Http2ClientSessionPrivate::notifyWindows()
{
    for (auto &pair : streams) {
        pair.second->windowUpdated.set();
    }
}

void Http2ClientSessionPrivate::unreserveStream(shared_ptr<Http2StreamState> stream)
{
    if (!stream || !stream->reserved) {
        return;
    }
    stream->reserved = false;
    if (activeStreamCount > 0) {
        --activeStreamCount;
    }
    streamSlotsAvailable.set();
}

void Http2ClientSessionPrivate::wakeStream(shared_ptr<Http2StreamState> stream)
{
    if (!stream) {
        return;
    }
    stream->headersReady.set();
    stream->dataReady.set();
    stream->windowUpdated.set();
    stream->closed.set();
}

void Http2ClientSessionPrivate::sendWindowUpdate(uint32_t streamId, uint32_t increment)
{
    if (increment == 0) {
        return;
    }
    string wu(4, '\0');
    writeUint32(&wu[0], increment & 0x7fffffff);
    sendFrame(FrameWindowUpdate, 0, streamId, wu);
}

void Http2ClientSessionPrivate::consumeRecvWindow(shared_ptr<Http2StreamState> stream, uint32_t frameLength)
{
    connectionRecvWindow -= static_cast<int32_t>(frameLength);
    stream->recvWindow -= static_cast<int32_t>(frameLength);
    if (connectionRecvWindow < 0 || stream->recvWindow < 0) {
        failConnection(ErrorFlowControlError);
        return;
    }
    if (connectionRecvWindow < kDefaultWindow / 2) {
        uint32_t incr = static_cast<uint32_t>(kDefaultWindow - connectionRecvWindow);
        connectionRecvWindow = kDefaultWindow;
        sendWindowUpdate(0, incr);
    }
    if (stream->recvWindow < kDefaultWindow / 2) {
        uint32_t incr = static_cast<uint32_t>(kDefaultWindow - stream->recvWindow);
        stream->recvWindow = kDefaultWindow;
        sendWindowUpdate(stream->id, incr);
    }
}

void Http2ClientSessionPrivate::applyInitialWindowSize(uint32_t newSize)
{
    if (newSize > 0x7fffffff) {
        failConnection(ErrorFlowControlError);
        return;
    }
    int32_t delta = static_cast<int32_t>(newSize) - remoteInitialWindow;
    remoteInitialWindow = static_cast<int32_t>(newSize);
    for (auto &pair : streams) {
        int64_t next = static_cast<int64_t>(pair.second->sendWindow) + delta;
        if (next > 0x7fffffff || next < 0) {
            failConnection(ErrorFlowControlError);
            return;
        }
        pair.second->sendWindow = static_cast<int32_t>(next);
    }
    notifyWindows();
}

void Http2ClientSessionPrivate::failConnection(uint32_t code)
{
    if (!valid && goingAway) {
        return;
    }
    goingAway = true;
    valid = false;
    string payload(8, '\0');
    writeUint32(&payload[0], 0);
    writeUint32(&payload[4], code);
    sendFrame(FrameGoAway, 0, 0, payload);
    finishStreamsWhere(code, false, 0);
    streamSlotsAvailable.set();
    peerSettingsEvent.set();
    if (connection) {
        connection->abort();
    }
}

bool Http2ClientSessionPrivate::start()
{
    if (!connection) {
        return false;
    }
    string preface(kClientPreface);
    string settings;
    char buf[6];
    // ENABLE_PUSH = 0
    buf[0] = 0;
    buf[1] = static_cast<char>(SettingsEnablePush);
    writeUint32(buf + 2, 0);
    settings.append(buf, 6);
    // INITIAL_WINDOW_SIZE
    buf[0] = 0;
    buf[1] = static_cast<char>(SettingsInitialWindowSize);
    writeUint32(buf + 2, static_cast<uint32_t>(kDefaultWindow));
    settings.append(buf, 6);
    // MAX_CONCURRENT_STREAMS (advertise a sane client limit)
    buf[0] = 0;
    buf[1] = static_cast<char>(SettingsMaxConcurrentStreams);
    writeUint32(buf + 2, 100);
    settings.append(buf, 6);

    preface += buildFrame(FrameSettings, 0, 0, settings);
    if (!sendRaw(preface)) {
        return false;
    }

    valid = true;
    weak_ptr<Http2ClientSessionPrivate> weak = shared_from_this();
    reader.reset(Coroutine::spawn([weak] {
        shared_ptr<Http2ClientSessionPrivate> self = weak.lock();
        if (self) {
            self->readerLoop();
        }
    }));
    return true;
}

void Http2ClientSessionPrivate::close()
{
    goingAway = true;
    valid = false;
    if (connection && connection->isValid()) {
        string payload(8, '\0');
        writeUint32(&payload[0], nextStreamId > 0 ? nextStreamId - 1 : 0);
        writeUint32(&payload[4], ErrorNoError);
        sendFrame(FrameGoAway, 0, 0, payload);
        connection->abort();
    }
    finishStreamsWhere(ErrorCancel, false, 0);
    notifyWindows();
    streamSlotsAvailable.set();
    peerSettingsEvent.set();
    if (reader && reader->isRunning() && reader.get() != Coroutine::current()) {
        reader->kill();
        reader->join();
    }
    reader.reset();
}

shared_ptr<Http2StreamState> Http2ClientSessionPrivate::createStream()
{
    while (activeStreamCount >= maxConcurrentStreams) {
        streamSlotsAvailable.clear();
        if (activeStreamCount < maxConcurrentStreams) {
            break;
        }
        if (!valid || goingAway) {
            return shared_ptr<Http2StreamState>();
        }
        if (!streamSlotsAvailable.tryWait()) {
            return shared_ptr<Http2StreamState>();
        }
    }
    if (!valid || goingAway) {
        return shared_ptr<Http2StreamState>();
    }
    if (nextStreamId > goAwayLastStreamId) {
        return shared_ptr<Http2StreamState>();
    }

    shared_ptr<Http2StreamState> stream = make_shared<Http2StreamState>();
    stream->id = nextStreamId;
    nextStreamId += 2;
    stream->sendWindow = remoteInitialWindow;
    stream->reserved = true;
    ++activeStreamCount;
    streams[stream->id] = stream;
    return stream;
}

void Http2ClientSessionPrivate::releaseStream(uint32_t streamId, bool sendRst, uint32_t code)
{
    auto it = streams.find(streamId);
    if (it == streams.end()) {
        return;
    }
    shared_ptr<Http2StreamState> stream = it->second;
    if (sendRst && !stream->reset && !stream->endStream) {
        string rst(4, '\0');
        writeUint32(&rst[0], code);
        sendFrame(FrameRstStream, 0, streamId, rst);
    }
    stream->reset = stream->reset || sendRst;
    stream->errorCode = code;
    unreserveStream(stream);
    wakeStream(stream);
    streams.erase(it);
}

void Http2ClientSessionPrivate::finishStreamError(shared_ptr<Http2StreamState> stream, uint32_t code)
{
    if (!stream) {
        return;
    }
    stream->reset = true;
    stream->errorCode = code;
    unreserveStream(stream);
    wakeStream(stream);
    streams.erase(stream->id);
}

void Http2ClientSessionPrivate::finishStreamsWhere(uint32_t code, bool onlyAboveLastStreamId, uint32_t lastStreamId)
{
    // Snapshot first: finishStreamError erases from `streams`.
    vector<shared_ptr<Http2StreamState>> doomed;
    doomed.reserve(streams.size());
    for (auto &pair : streams) {
        if (!onlyAboveLastStreamId || pair.first > lastStreamId) {
            doomed.push_back(pair.second);
        }
    }
    for (auto &stream : doomed) {
        finishStreamError(stream, code);
    }
}

bool Http2ClientSessionPrivate::waitPeerSettings()
{
    if (peerSettingsSeen) {
        return valid;
    }
    // Peer SETTINGS usually arrives immediately; bound wait so we do not hang forever.
    peerSettingsEvent.clear();
    if (peerSettingsSeen) {
        return valid;
    }
    peerSettingsEvent.tryWait(5000);
    return peerSettingsSeen && valid;
}

bool Http2ClientSessionPrivate::waitSendWindow(shared_ptr<Http2StreamState> stream, int32_t needed)
{
    while (stream->sendWindow < needed || connectionSendWindow < needed) {
        if (!valid || stream->reset || goingAway) {
            return false;
        }
        stream->windowUpdated.clear();
        if (stream->sendWindow >= needed && connectionSendWindow >= needed) {
            break;
        }
        // Connection-level WINDOW_UPDATE calls notifyWindows(), which sets every
        // stream's windowUpdated; stream-level updates set this event directly.
        if (!stream->windowUpdated.tryWait()) {
            return false;
        }
    }
    return true;
}

bool Http2ClientSessionPrivate::sendHeaderBlock(uint32_t streamId, const string &block, bool endStream)
{
    // Caller must hold writeLock (HPACK dynamic table + wire order).
    if (block.size() <= maxFrameSize) {
        uint8_t flags = FlagEndHeaders;
        if (endStream) {
            flags |= FlagEndStream;
        }
        return sendFrameUnlocked(FrameHeaders, flags, streamId, block);
    }
    size_t offset = 0;
    string first = block.substr(0, maxFrameSize);
    offset = maxFrameSize;
    // END_STREAM may be on HEADERS even when CONTINUATION follows (RFC 7540 §6.2).
    uint8_t hflags = endStream ? FlagEndStream : 0;
    if (!sendFrameUnlocked(FrameHeaders, hflags, streamId, first)) {
        return false;
    }
    while (offset < block.size()) {
        size_t n = min(static_cast<size_t>(maxFrameSize), block.size() - offset);
        string chunk = block.substr(offset, n);
        offset += n;
        uint8_t flags = (offset >= block.size()) ? FlagEndHeaders : 0;
        if (!sendFrameUnlocked(FrameContinuation, flags, streamId, chunk)) {
            return false;
        }
    }
    return true;
}

bool Http2ClientSessionPrivate::sendBody(shared_ptr<Http2StreamState> stream, shared_ptr<FileLike> body)
{
    const int64_t totalSize = body->size();
    if (totalSize == 0) {
        return sendFrame(FrameData, FlagEndStream, stream->id, string());
    }

    auto sendChunk = [&](const string &chunk, bool last) -> bool {
        size_t offset = 0;
        while (offset < chunk.size()) {
            int32_t allowed = min(stream->sendWindow, connectionSendWindow);
            if (allowed <= 0) {
                if (!waitSendWindow(stream, 1)) {
                    return false;
                }
                continue;
            }
            size_t n = min(min(static_cast<size_t>(maxFrameSize), chunk.size() - offset),
                           static_cast<size_t>(allowed));
            if (!waitSendWindow(stream, static_cast<int32_t>(n))) {
                return false;
            }
            uint8_t flags = 0;
            if (last && offset + n >= chunk.size()) {
                flags = FlagEndStream;
            }
            string piece = chunk.substr(offset, n);
            if (!sendFrame(FrameData, flags, stream->id, piece)) {
                return false;
            }
            stream->sendWindow -= static_cast<int32_t>(n);
            connectionSendWindow -= static_cast<int32_t>(n);
            offset += n;
        }
        return true;
    };

    if (totalSize > 0) {
        bool ok = false;
        const string bodyData = body->readall(&ok);
        if (!ok) {
            return false;
        }
        return sendChunk(bodyData, true);
    }

    // Unknown size: stream until EOF.
    bool any = false;
    while (true) {
        string chunk = body->read(static_cast<int32_t>(maxFrameSize));
        if (chunk.empty()) {
            break;
        }
        any = true;
        if (!sendChunk(chunk, false)) {
            return false;
        }
    }
    return sendFrame(FrameData, FlagEndStream, stream->id, string());
}

void Http2ClientSessionPrivate::handleFrame(uint8_t type, uint8_t flags, uint32_t streamId, const string &payload)
{
    switch (type) {
    case FrameSettings:
        if (flags & FlagAck) {
            if (!payload.empty()) {
                failConnection(ErrorFrameSizeError);
            }
            return;
        }
        if (payload.size() % 6 != 0) {
            failConnection(ErrorFrameSizeError);
            return;
        }
        for (size_t i = 0; i + 6 <= payload.size(); i += 6) {
            uint16_t id = (static_cast<uint8_t>(payload[i]) << 8) | static_cast<uint8_t>(payload[i + 1]);
            uint32_t value = readUint32(payload.data() + i + 2);
            if (id == SettingsInitialWindowSize) {
                applyInitialWindowSize(value);
            } else if (id == SettingsMaxFrameSize) {
                if (value < 16384 || value > 16777215) {
                    failConnection(ErrorProtocolError);
                    return;
                }
                maxFrameSize = value;
            } else if (id == SettingsHeaderTableSize) {
                encoder.setMaxDynamicTableSize(value);
                decoder.setMaxDynamicTableSize(value);
            } else if (id == SettingsMaxConcurrentStreams) {
                maxConcurrentStreams = value;
                streamSlotsAvailable.set();
            }
            // Other settings (incl. ENABLE_PUSH) ignored; PUSH_PROMISE is always refused.
        }
        peerSettingsSeen = true;
        peerSettingsEvent.set();
        sendFrame(FrameSettings, FlagAck, 0, string());
        break;
    case FrameWindowUpdate:
        if (payload.size() != 4) {
            failConnection(ErrorFrameSizeError);
            return;
        }
        {
            uint32_t increment = readUint32(payload.data()) & 0x7fffffff;
            if (increment == 0) {
                if (streamId == 0) {
                    failConnection(ErrorProtocolError);
                } else {
                    auto it = streams.find(streamId);
                    if (it != streams.end()) {
                        string rst(4, '\0');
                        writeUint32(&rst[0], ErrorProtocolError);
                        sendFrame(FrameRstStream, 0, streamId, rst);
                        finishStreamError(it->second, ErrorProtocolError);
                    }
                }
                return;
            }
            if (streamId == 0) {
                int64_t next = static_cast<int64_t>(connectionSendWindow) + increment;
                if (next > 0x7fffffff) {
                    failConnection(ErrorFlowControlError);
                    return;
                }
                connectionSendWindow = static_cast<int32_t>(next);
                notifyWindows();
            } else {
                auto it = streams.find(streamId);
                if (it != streams.end()) {
                    int64_t next = static_cast<int64_t>(it->second->sendWindow) + increment;
                    if (next > 0x7fffffff) {
                        failConnection(ErrorFlowControlError);
                        return;
                    }
                    it->second->sendWindow = static_cast<int32_t>(next);
                    it->second->windowUpdated.set();
                }
            }
        }
        break;
    case FramePing:
        if (payload.size() != 8) {
            failConnection(ErrorFrameSizeError);
            return;
        }
        if (!(flags & FlagAck)) {
            sendFrame(FramePing, FlagAck, 0, payload);
        }
        break;
    case FrameGoAway:
        goingAway = true;
        if (payload.size() >= 8) {
            goAwayLastStreamId = readUint32(payload.data()) & 0x7fffffff;
            uint32_t code = readUint32(payload.data() + 4);
            finishStreamsWhere(code, true, goAwayLastStreamId);
        } else {
            valid = false;
            finishStreamsWhere(ErrorCancel, false, 0);
        }
        streamSlotsAvailable.set();
        break;
    case FrameRstStream:
        if (payload.size() != 4) {
            failConnection(ErrorFrameSizeError);
            return;
        }
        {
            auto it = streams.find(streamId);
            if (it != streams.end()) {
                finishStreamError(it->second, readUint32(payload.data()));
            }
        }
        break;
    case FramePushPromise: {
        uint32_t promised = 0;
        if (payload.size() >= 4) {
            size_t offset = 0;
            if (flags & FlagPadded) {
                if (payload.empty()) {
                    failConnection(ErrorProtocolError);
                    return;
                }
                offset = 1;
            }
            if (payload.size() < offset + 4) {
                failConnection(ErrorProtocolError);
                return;
            }
            promised = readUint32(payload.data() + offset) & 0x7fffffff;
        }
        string rst(4, '\0');
        writeUint32(&rst[0], ErrorRefusedStream);
        if (promised) {
            sendFrame(FrameRstStream, 0, promised, rst);
        }
        break;
    }
    case FrameHeaders:
    case FrameContinuation: {
        string headerPayload = payload;
        uint8_t frameFlags = flags;
        if (type == FrameHeaders) {
            if (streamId == 0 || headerBlockStream != 0) {
                failConnection(ErrorProtocolError);
                return;
            }
            size_t offset = 0;
            if (flags & FlagPadded) {
                if (payload.empty()) {
                    failConnection(ErrorProtocolError);
                    return;
                }
                uint8_t pad = static_cast<uint8_t>(payload[0]);
                offset = 1;
                if (offset + pad > payload.size()) {
                    failConnection(ErrorProtocolError);
                    return;
                }
                headerPayload = payload.substr(offset, payload.size() - offset - pad);
            }
            if (flags & FlagPriority) {
                if (headerPayload.size() < 5) {
                    failConnection(ErrorProtocolError);
                    return;
                }
                headerPayload = headerPayload.substr(5);
            }
            headerBlockStream = streamId;
            headerBlockFlags = flags;
            headerBlockBuffer = headerPayload;
        } else {
            if (streamId == 0 || streamId != headerBlockStream) {
                failConnection(ErrorProtocolError);
                return;
            }
            headerBlockBuffer += payload;
            headerBlockFlags = static_cast<uint8_t>((headerBlockFlags & ~FlagEndHeaders)
                                                    | (flags & FlagEndHeaders));
            if (flags & FlagEndStream) {
                headerBlockFlags |= FlagEndStream;
            }
            frameFlags = headerBlockFlags;
        }
        if (!(frameFlags & FlagEndHeaders)) {
            return;
        }
        auto it = streams.find(headerBlockStream);
        headerBlockStream = 0;
        string block = std::move(headerBlockBuffer);
        headerBlockBuffer.clear();
        if (it == streams.end()) {
            return;
        }
        shared_ptr<Http2StreamState> stream = it->second;
        if (stream->endStream) {
            // Headers/trailers after the stream is already closed.
            string rst(4, '\0');
            writeUint32(&rst[0], ErrorStreamClosed);
            sendFrame(FrameRstStream, 0, stream->id, rst);
            finishStreamError(stream, ErrorStreamClosed);
            return;
        }
        vector<HpackHeader> decoded;
        if (!decoder.decode(block, &decoded)) {
            failConnection(ErrorCompressionError);
            return;
        }
        if (!stream->endHeaders) {
            stream->responseHeaders = decoded;
            stream->endHeaders = true;
            stream->headersReady.set();
        } else {
            // Trailers after DATA.
            stream->trailers.insert(stream->trailers.end(), decoded.begin(), decoded.end());
        }
        if (frameFlags & FlagEndStream) {
            stream->endStream = true;
            stream->closed.set();
            stream->dataReady.set();
            unreserveStream(stream);
        }
        break;
    }
    case FrameData: {
        if (streamId == 0) {
            failConnection(ErrorProtocolError);
            return;
        }
        auto it = streams.find(streamId);
        if (it == streams.end()) {
            // Ignore DATA for unknown/closed streams but still enforce connection window.
            connectionRecvWindow -= static_cast<int32_t>(payload.size());
            if (connectionRecvWindow < 0) {
                failConnection(ErrorFlowControlError);
                return;
            }
            if (connectionRecvWindow < kDefaultWindow / 2) {
                uint32_t incr = static_cast<uint32_t>(kDefaultWindow - connectionRecvWindow);
                connectionRecvWindow = kDefaultWindow;
                sendWindowUpdate(0, incr);
            }
            return;
        }
        shared_ptr<Http2StreamState> stream = it->second;
        if (stream->endStream) {
            // Still charge the connection window before rejecting the stream frame.
            connectionRecvWindow -= static_cast<int32_t>(payload.size());
            if (connectionRecvWindow < 0) {
                failConnection(ErrorFlowControlError);
                return;
            }
            string rst(4, '\0');
            writeUint32(&rst[0], ErrorStreamClosed);
            sendFrame(FrameRstStream, 0, streamId, rst);
            finishStreamError(stream, ErrorStreamClosed);
            return;
        }
        string data = payload;
        if (flags & FlagPadded) {
            if (payload.empty()) {
                failConnection(ErrorProtocolError);
                return;
            }
            uint8_t pad = static_cast<uint8_t>(payload[0]);
            if (1 + pad > payload.size()) {
                failConnection(ErrorProtocolError);
                return;
            }
            data = payload.substr(1, payload.size() - 1 - pad);
        }
        stream->body.append(data);
        stream->receivedBodyBytes += static_cast<int64_t>(data.size());
        consumeRecvWindow(stream, static_cast<uint32_t>(payload.size()));
        if (flags & FlagEndStream) {
            stream->endStream = true;
            stream->closed.set();
            unreserveStream(stream);
        }
        stream->dataReady.set();
        break;
    }
    case FramePriority:
        break;
    default:
        // Ignore unknown frame types (extension frames).
        break;
    }
}

void Http2ClientSessionPrivate::readerLoop()
{
    try {
        while (valid && connection && connection->isValid()) {
            uint8_t type = 0;
            uint8_t flags = 0;
            uint32_t streamId = 0;
            string payload;
            if (!readFrame(&type, &flags, &streamId, &payload)) {
                break;
            }
            if (debugLevel > 1) {
                ngDebug() << "http2 frame type" << static_cast<int>(type) << "stream" << streamId << "len"
                          << payload.size();
            }
            handleFrame(type, flags, streamId, payload);
        }
    } catch (CoroutineException &) {
    }
    valid = false;
    finishStreamsWhere(ErrorCancel, false, 0);
    notifyWindows();
    streamSlotsAvailable.set();
    peerSettingsEvent.set();
}

void Http2ClientSessionPrivate::exchange(HttpSessionPrivate *session, HttpRequest &request, HttpResponse &response)
{
    if (!valid) {
        response.setError(new ConnectionError());
        return;
    }
    if (!waitPeerSettings()) {
        response.setError(new ConnectionError());
        return;
    }

    shared_ptr<Http2StreamState> stream = createStream();
    if (!stream) {
        response.setError(new ConnectionError());
        return;
    }

    utils::Url url = request.d->url;
    vector<HpackHeader> headers;
    headers.push_back(HpackHeader{":method", utils::toUpper(request.d->method)});
    headers.push_back(HpackHeader{":scheme", url.scheme() == "wss" ? string("https") : url.scheme()});
    headers.push_back(HpackHeader{":path", urlResourcePath(url)});
    string authority = url.host();
    if (url.port() != -1) {
        authority += ":" + utils::number(url.port());
    }
    headers.push_back(HpackHeader{":authority", authority});

    if (!request.hasHeader("user-agent")) {
        string ua = request.userAgent().empty() ? session->defaultUserAgent : request.userAgent();
        headers.push_back(HpackHeader{"user-agent", ua});
    }
    if (!request.hasHeader("accept")) {
        headers.push_back(HpackHeader{"accept", "*/*"});
    }
    if (!request.hasHeader("accept-encoding")) {
#ifdef QTNG_HAVE_ZLIB
        headers.push_back(HpackHeader{"accept-encoding", "gzip, deflate"});
#else
        headers.push_back(HpackHeader{"accept-encoding", "identity"});
#endif
    }
    if (!request.d->cookies.empty() && !request.hasHeader("cookie")) {
        string cookieHeader;
        bool first = true;
        for (const HttpCookie &cookie : request.d->cookies) {
            if (!first) {
                cookieHeader += "; ";
            }
            first = false;
            cookieHeader += cookie.toRawForm(HttpCookie::NameAndValueOnly);
        }
        headers.push_back(HpackHeader{"cookie", cookieHeader});
    }
    for (const HttpHeader &h : request.allHeaders()) {
        string name = utils::toLower(h.name);
        if (name == "connection" || name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding"
            || name == "upgrade" || name == "host" || name == "http2-settings") {
            continue;
        }
        headers.push_back(HpackHeader{name, h.value});
    }
    if (request.d->body && request.d->body->size() >= 0 && !request.hasHeader("content-length")) {
        headers.push_back(
                HpackHeader{"content-length", utils::number(static_cast<long long>(request.d->body->size()))});
    }

    const bool hasBody = request.d->body && request.d->body->size() != 0;
    bool headersSent = false;
    {
        // Serialize HPACK encode with header send so dynamic table stays consistent.
        ScopedLock<Lock> locker(writeLock);
        if (!locker.isSuccess()) {
            response.setError(new ConnectionError());
            releaseStream(stream->id);
            return;
        }
        const string block = encoder.encode(headers);
        headersSent = sendHeaderBlock(stream->id, block, !hasBody);
    }
    if (!headersSent) {
        response.setError(new ConnectionError());
        releaseStream(stream->id, true, ErrorCancel);
        return;
    }

    if (hasBody) {
        if (!sendBody(stream, request.d->body)) {
            response.setError(new ConnectionError());
            releaseStream(stream->id, true, ErrorCancel);
            return;
        }
    }

    stream->headersReady.tryWait();
    if (stream->reset || !stream->endHeaders) {
        response.setError(new ConnectionError());
        releaseStream(stream->id);
        return;
    }

    response.d->version = Http2_0;
    vector<HttpHeader> respHeaders;
    for (const HpackHeader &h : stream->responseHeaders) {
        if (h.name == ":status") {
            bool ok = false;
            response.d->statusCode = utils::parseInt(h.value, &ok);
            if (!ok) {
                response.setError(new InvalidHeader());
                releaseStream(stream->id, true, ErrorProtocolError);
                return;
            }
            response.d->statusText = h.value;
        } else if (!h.name.empty() && h.name[0] != ':') {
            respHeaders.push_back(HttpHeader(h.name, h.value));
        }
    }
    response.setHeaders(respHeaders);

    if (session->managingCookies && response.hasHeader("set-cookie")) {
        for (const string &value : response.multiHeader("set-cookie")) {
            const vector<HttpCookie> &cookies = HttpCookie::parseCookies(value);
            response.d->cookies.insert(response.d->cookies.end(), cookies.begin(), cookies.end());
        }
        session->cookieJar.setCookiesFromUrl(response.d->cookies, response.d->url.toString());
    }

    if (utils::toUpper(request.method()) == "HEAD") {
        if (!stream->endStream) {
            stream->closed.tryWait();
        }
        response.d->consumed = true;
        response.d->body.clear();
        releaseStream(stream->id);
    } else if (request.streamResponse()) {
        // Expose a SocketLike over the HTTP/2 stream body (prefix already buffered).
        response.d->body = stream->body.substr(stream->bodyReadOffset);
        stream->bodyReadOffset = stream->body.size();
        response.d->stream = make_shared<Http2BodySocketLike>(shared_from_this(), stream);
        response.d->consumed = false;
        // Keep stream in map until body socket closes / finishes.
    } else {
        if (!stream->endStream) {
            stream->closed.tryWait();
        }
        if (stream->reset && stream->body.empty() && response.d->statusCode == 0) {
            response.setError(new ConnectionError());
            releaseStream(stream->id);
            return;
        }
        if (request.d->maxBodySize >= 0 && stream->receivedBodyBytes > request.d->maxBodySize) {
            response.setError(new UnrewindableBodyError());
            releaseStream(stream->id, true, ErrorCancel);
            return;
        }
        response.d->body = stream->body;
        response.d->consumed = true;
        if (session->debugLevel == 1 && !response.d->body.empty()) {
            ngDebug() << "receiving body:" << response.d->body.size();
        } else if (session->debugLevel > 1 && !response.d->body.empty()) {
            ngDebug() << "receiving body:" << response.d->body;
        }
        releaseStream(stream->id);
    }

    if (response.d->statusCode >= 400) {
        response.setError(new HTTPError(response.d->statusCode));
    } else if (session->cacheManager && !request.streamResponse()) {
        const string rm = utils::toUpper(request.method());
        if (rm == "GET" || rm == "HEAD" || rm == "OPTIONS") {
            bool doCache = true;
            const string &requestHeader = utils::toLower(request.header(KnownHeader::CacheControlHeader));
            if (requestHeader.find("no-cache") != string::npos || requestHeader.find("no-store") != string::npos) {
                doCache = false;
            } else {
                const string &responseHeader = utils::toLower(response.header(KnownHeader::CacheControlHeader));
                if (responseHeader.find("public") == string::npos && responseHeader.find("private") == string::npos) {
                    doCache = false;
                }
                if (responseHeader.find("no-cache") != string::npos
                    || responseHeader.find("no-store") != string::npos) {
                    doCache = false;
                }
            }
            if (doCache) {
                session->cacheManager->addResponse(response);
            }
        }
    }
}

Http2ClientSession::Http2ClientSession(shared_ptr<SocketLike> connection, int debugLevel)
    : d(make_shared<Http2ClientSessionPrivate>(connection, debugLevel))
{
}

Http2ClientSession::~Http2ClientSession()
{
    if (d) {
        d->close();
    }
}

bool Http2ClientSession::start()
{
    return d->start();
}

bool Http2ClientSession::isValid() const
{
    return d && d->valid && d->connection && d->connection->isValid() && !d->goingAway;
}

void Http2ClientSession::close()
{
    d->close();
}

void Http2ClientSession::exchange(HttpSessionPrivate *session, HttpRequest &request, HttpResponse &response)
{
    d->exchange(session, request, response);
}

void Http2Protocol::exchange(HttpSessionPrivate *, HttpRequest &, HttpResponse &response, shared_ptr<SocketLike>,
                             unique_ptr<ScopedLock<Semaphore>> &)
{
    response.setError(new UnsupportedVersion());
}

}  // namespace qtng
