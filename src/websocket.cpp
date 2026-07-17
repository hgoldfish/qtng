#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>


#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#include "qtng/http.h"
#include "qtng/websocket.h"
#include "qtng/utils/string_utils.h"
#include "qtng/coroutine_utils.h"
#include "qtng/socket_utils.h"
#include "qtng/random.h"
#include "qtng/utils/logging.h"

using namespace std;

NG_LOGGER("qtng.websocket");
#define DEBUG_PROTOCOL 1

namespace qtng {

class WebSocketConfigurationPrivate
{
public:
    WebSocketConfigurationPrivate();
public:
    vector<string> protocols;
    uint64_t keepaliveInterval;
    uint64_t keepaliveTimeout;
    uint32_t sendingQueueCapacity;
    uint32_t receivingQueueCapacity;
    int32_t maxPayloadSize;
    int32_t outgoingSize;
};

class PacketToRead
{
public:
    PacketToRead()
        : type(WebSocketConnection::Unknown)
    {
    }

    PacketToRead(WebSocketConnection::FrameType type, const string &payload)
        : type(type)
        , payload(payload)
    {
    }
public:
    WebSocketConnection::FrameType type;
    string payload;
public:
    inline bool isValid() const { return type != WebSocketConnection::Unknown; }
};

class PacketToWrite
{
public:
    PacketToWrite()
        : type(WebSocketConnection::Unknown)
    {
    }
    PacketToWrite(const string &data, WebSocketConnection::FrameType frameType,
                  shared_ptr<ValueEvent<bool>> done)
        : type(frameType)
        , payload(data)
        , done(done)
    {
    }
public:
    WebSocketConnection::FrameType type;
    string payload;
    shared_ptr<ValueEvent<bool>> done;
public:
    inline bool isValid() const
    {
        return !payload.empty() && (type == WebSocketConnection::Binary || type == WebSocketConnection::Text);
    }
};

enum FrameType {
    ContinuationFrame = 0x0,
    TextFrame = 0x1,
    BinaryFrame = 0x2,
    CloseFrame = 0x8,
    PingFrame = 0x9,
    PongFrame = 0xa
};

class WebSocketFrame
{
public:
    WebSocketFrame();
public:
    uint8_t fin : 1;
    uint8_t rsv1 : 1;
    uint8_t rsv2 : 1;
    uint8_t rsv3 : 1;
    uint8_t opcode : 4;
    uint32_t maskkey;
    string payload;
public:
    // parse the header and returns the payload size.
    // if the header is valid, it will be removed from the buf
    int64_t feedHeader(char *packet, int &packetSize);

    // apply mask to payload and copy to src[offset: len]. this function also decode payload using mask
    void applyMaskTo(char *dst, int offset, int dst_max_len) const;

    // make frame packet.
    string toByteArray() const;
public:
    inline bool isValid() const { return !rsv1 && !rsv2 && !rsv3 && !payload.empty(); }
};

class WebSocketConnectionPrivate
{
public:
    WebSocketConnectionPrivate(shared_ptr<SocketLike> connection, const string &headBytes, WebSocketConnection::Side side,
                               const WebSocketConfiguration &config, WebSocketConnection *q);
    ~WebSocketConnectionPrivate();
public:
    void doSend();
    void doReceive(const string &headBytes);
    void doKeepalive();
    void setErrorCode(int errorCode, const string &errorString);
    void abort(int errorCode);
    bool close();
private:
    string makeClosePayload(int closeCode, const string &closeReason);
    pair<int, string> parseClosePayload(const string &payload);
    WebSocketFrame makeControlFrame(FrameType type);
    vector<WebSocketFrame> fragmentFrame(const PacketToWrite &writingPacket, const int blockSize);
    uint32_t makeMaskkey();
    bool recvBytes(string &buf, int &usedSize);
    bool sendBytes(const string &packet);
public:
    CoroutineGroup *operations;
    HttpResponse response;
    shared_ptr<SocketLike> const connection;
    string id;
    Queue<PacketToRead> receivingQueue;
    Queue<PacketToWrite> sendingQueue;
    Lock writeLock;
    WebSocketConnection::State state;
    WebSocketConnection::Side side;
    int debugLevel;
    int32_t maxPayloadSize;
    int32_t outgoingSize;
    int64_t lastActiveTimestamp;
    int64_t lastKeepaliveTimestamp;
    int64_t keepaliveTimeout;
    int64_t keepaliveInterval;
    string errorString;
    int errorCode;
    bool mustMask;
private:
    WebSocketConnection * const q_ptr;
    NG_DECLARE_PUBLIC(WebSocketConnection);
};

void setWebSocketConnectionPrivateResponse(WebSocketConnectionPrivate *d, HttpResponse response)
{
    d->response = response;
}

WebSocketConfigurationPrivate::WebSocketConfigurationPrivate()
    : keepaliveInterval(5 * 1000)
    , keepaliveTimeout(60 * 1000)
    , sendingQueueCapacity(256)
    , receivingQueueCapacity(256)
    , maxPayloadSize(INT32_MAX)
    , outgoingSize(1024 * 64)
{
}

WebSocketConfiguration::WebSocketConfiguration()
    : d_ptr(new WebSocketConfigurationPrivate())
{
}

WebSocketConfiguration::~WebSocketConfiguration()
{
    delete d_ptr;
}

void WebSocketConfiguration::setKeepaliveInterval(float keepaliveInterval)
{
    NG_D(WebSocketConfiguration);
    d->keepaliveInterval = static_cast<int64_t>(keepaliveInterval * 1000);
    if (d->keepaliveInterval < 200) {
        d->keepaliveInterval = 200;
    }
}

float WebSocketConfiguration::keepaliveInterval() const
{
    NG_D(const WebSocketConfiguration);
    return static_cast<float>(d->keepaliveInterval) / 1000.0f;
}

void WebSocketConfiguration::setKeepaliveTimeout(float timeout)
{
    NG_D(WebSocketConfiguration);
    if (timeout > 0) {
        d->keepaliveTimeout = static_cast<int64_t>(timeout * 1000);
        if (d->keepaliveTimeout < 1000) {
            d->keepaliveTimeout = 1000;
        }
    }
}

float WebSocketConfiguration::keepaliveTimeout() const
{
    NG_D(const WebSocketConfiguration);
    return static_cast<float>(d->keepaliveTimeout) / 1000.0f;
}

uint32_t WebSocketConfiguration::sendingQueueCapacity() const
{
    NG_D(const WebSocketConfiguration);
    return d->sendingQueueCapacity;
}

void WebSocketConfiguration::setSendingQueueCapacity(uint32_t capacity)
{
    NG_D(WebSocketConfiguration);
    assert(capacity != 0);
    d->sendingQueueCapacity = capacity;
}

uint32_t WebSocketConfiguration::receivingQueueCapacity() const
{
    NG_D(const WebSocketConfiguration);
    return d->receivingQueueCapacity;
}

void WebSocketConfiguration::setReceivingQueueCapacity(uint32_t capacity)
{
    NG_D(WebSocketConfiguration);
    assert(capacity > 0);
    d->receivingQueueCapacity = capacity;
}

int32_t WebSocketConfiguration::maxPayloadSize() const
{
    NG_D(const WebSocketConfiguration);
    return d->maxPayloadSize;
}

void WebSocketConfiguration::setMaxPayloadSize(int32_t size)
{
    NG_D(WebSocketConfiguration);
    if (size >= 1024 && size <= INT32_MAX) {
        d->maxPayloadSize = size;
    }
}

vector<string> WebSocketConfiguration::protocols() const
{
    NG_D(const WebSocketConfiguration);
    return d->protocols;
}

void WebSocketConfiguration::setProtocols(const vector<string> &protocols)
{
    NG_D(WebSocketConfiguration);
    d->protocols = protocols;
}

void WebSocketConfiguration::setOutgoingSize(int32_t size)
{
    NG_D(WebSocketConfiguration);
    d->outgoingSize = size;
}

int32_t WebSocketConfiguration::outgoingSize() const
{
    NG_D(const WebSocketConfiguration);
    return d->outgoingSize;
}

WebSocketFrame::WebSocketFrame()
    : fin(0)
    , rsv1(0)
    , rsv2(0)
    , rsv3(0)
    , opcode(0)
    , maskkey(0)
{
}

int64_t WebSocketFrame::feedHeader(char *packet, int &packetSize)
{
    assert(packetSize >= 2);
    // the ngFromBigEndian<>() only accept uint8_t* in the earlier version of Qt.
    uint8_t *upacket = reinterpret_cast<uint8_t *>(packet);
    unsigned char b0 = upacket[0];
    unsigned char b1 = upacket[1];

    fin = (b0 & 0x80) >> 7;
    rsv1 = (b0 & 0x40) >> 6;
    rsv2 = (b0 & 0x20) >> 5;
    rsv3 = (b0 & 0x10) >> 4;
    opcode = b0 & 0x0f;

    bool has_mask = b1 & 0x80;
    int len = b1 & 0x7f;
    maskkey = 0;

    int headerSize = 2;
    if (len <= 125) {
        // pass
    } else if (len == 126) {
        if (packetSize >= headerSize + 2) {
            len = ngFromBigEndian<uint16_t>(upacket + headerSize);
            headerSize += 2;
        } else {
            return -1;
        }
    } else {
        assert(len == 127);
        if (packetSize >= headerSize + 8) {
            len = ngFromBigEndian<uint64_t>(upacket + headerSize);
            headerSize += 8;
        } else {
            return -1;
        }
    }
    if (has_mask) {
        if (packetSize >= headerSize + 4) {
            maskkey = ngFromBigEndian<uint32_t>(upacket + headerSize);
            headerSize += 4;
        } else {
            return -1;
        }
    }
    memmove(packet, packet + headerSize, packetSize - headerSize);
    packetSize -= headerSize;
    return len;
}

// the length of dst must larger than offset + size!
void WebSocketFrame::applyMaskTo(char *dst, int offset, int size) const
{
    int i = offset;
    int j = 0;
    if (size < payload.size()) {
        ngWarning() << "applyMaskTo() got an dest buffer which is too small.";
    }
    const char *src = payload.data();
    uint8_t maskbuf[4];
    ngToBigEndian<uint32_t>(maskkey, maskbuf);
    int last = offset + min(size, static_cast<int>(payload.size()));
    // XOR the remainder of the input byte by byte.
    for (; i < last; ++i, ++j) {
        dst[i] = src[j] ^ maskbuf[j % 4];
    }
}

/*
// the length of dst must larger than offset + size!
void WebSocketFrame::applyMaskTo(char *dst, int offset, int size) const
{
    int i = offset;
    int j = 0;
    if (size < payload.size()) {
        ngWarning() << "applyMaskTo() got an dest buffer which is too small.";
    }
    int last = offset + min(size, static_cast<int>(payload.size()));
    int next_offset_8 = (offset + 7) / 8 * 8;
    const char *src = payload.data();
    uint8_t maskbuf[4];
    ngToBigEndian<uint32_t>(maskkey, maskbuf);

    // We shoud make sure the memory allocator aligns everything on 8 bytes boundaries.
    // and assume that payload buf is aligns on 8 bytes boundaries.
    for (; i < next_offset_8 && i < last; ++i, ++j) {
        dst[i] = src[j] ^ maskbuf[j % 4];
    }

    // We need a new scope for MSVC 2010 (non C99 friendly)
    {
#if __ARM_NEON
        // With NEON support, XOR by blocks of 16 bytes = 128 bits.

        int last_128 = last & ~15;
        uint8x16_t mask_128 = vreinterpretq_u8_u32(vdupq_n_u32(maskkey));

        for (; i < last_128; i += 16, j += 16) {
            uint8x16_t in_128 = vld1q_u8((uint8_t *) (src + j));
            uint8x16_t out_128 = veorq_u8(in_128, mask_128);
            vst1q_u8((uint8_t *) (dst + i), out_128);
        }
#elif __SSE2__
        // With SSE2 support, XOR by blocks of 16 bytes = 128 bits.
        // we use load/store instead of loadu/storeu

        int last_128 = last & ~15;
        __m128i mask_128 = _mm_set1_epi32(maskkey);

        for (; i < last_128; i += 16, j += 16) {
            __m128i in_128 = _mm_loadu_si128((__m128i *) (src + j));
            __m128i out_128 = _mm_xor_si128(in_128, mask_128);
            _mm_storeu_si128((__m128i *) (dst + i), out_128);
        }
#else
        // Without SSE2 support, XOR by blocks of 8 bytes = 64 bits.

        int last_64 = last & ~7;
        uint64_t mask_64 = ((uint64_t) maskkey << 32) | (uint64_t) maskkey;

        for (; i < last_64; i += 8, j += 8) {
            *(uint64_t *) (dst + i) = *(uint64_t *) (src + j) ^ mask_64;
        }
#endif
    }

    // XOR the remainder of the input byte by byte.
    for (; i < last; ++i, ++j) {
        dst[i] = src[j] ^ maskbuf[j % 4];
    }
}
*/

string WebSocketFrame::toByteArray() const
{
    int len = payload.size();
    string buf(static_cast<size_t>(len + 32), '\0');
    int packetSize = 2;

    if (fin) {
        buf[0] = 0x80;
    } else {
        buf[0] = 0;
    }
    buf[0] = buf[0] | opcode;

    if (maskkey > 0) {
        buf[1] = 0x80;
    } else {
        buf[1] = 0;
    }

    uint8_t *ubuf = reinterpret_cast<uint8_t *>(&buf[0]);
    if (len <= 125) {
        buf[1] = buf[1] | len;
    } else if (len < 65535) {
        buf[1] = buf[1] | 126;
        ngToBigEndian<uint16_t>(len, ubuf + 2);
        packetSize += 2;
    } else {
        buf[1] = buf[1] | 127;
        ngToBigEndian<uint64_t>(len, ubuf + 2);
        packetSize += 8;
    }

    if (maskkey > 0) {
        ngToBigEndian<uint32_t>(this->maskkey, ubuf + packetSize);
        packetSize += 4;
        applyMaskTo(&buf[0], packetSize, buf.size() - packetSize);
    } else {
        memcpy(&buf[packetSize], payload.data(), payload.size());
    }
    packetSize += payload.size();
    return buf.substr(0, packetSize);
}

WebSocketConnectionPrivate::WebSocketConnectionPrivate(shared_ptr<SocketLike> connection,const string &headBytes,
                                                       WebSocketConnection::Side side,
                                                       const WebSocketConfiguration &config, WebSocketConnection *q)
    : operations(new CoroutineGroup())
    , connection(connection)
    , receivingQueue(config.receivingQueueCapacity())
    , sendingQueue(config.sendingQueueCapacity())
    , state(WebSocketConnection::Open)
    , side(side)
    , debugLevel(0)
    , maxPayloadSize(config.maxPayloadSize())
    , outgoingSize(config.outgoingSize())
    , lastActiveTimestamp(utils::DateTime::currentMSecsSinceEpoch())
    , lastKeepaliveTimestamp(lastActiveTimestamp)
    , keepaliveTimeout(config.keepaliveTimeout() * 1000)
    , keepaliveInterval(config.keepaliveInterval() * 1000)
    , errorCode(0)
    , mustMask(side == WebSocketConnection::Client)
    , q_ptr(q)
{
    id = randomBytes(16);
    operations->spawnWithName("send", [this] { doSend(); });
    operations->spawnWithName("receive", [this, headBytes] { doReceive(headBytes); });
    operations->spawnWithName("keepalive", [this] { doKeepalive(); });
}

WebSocketConnectionPrivate::~WebSocketConnectionPrivate()
{
    abort(WebSocketConnection::GoingAway);
    delete operations;
}

uint32_t WebSocketConnectionPrivate::makeMaskkey()
{
}

vector<WebSocketFrame> WebSocketConnectionPrivate::fragmentFrame(const PacketToWrite &writingPacket,
                                                                  const int blockSize)
{
    int nFrames = (writingPacket.payload.size() + blockSize - 1) / blockSize;
    if (nFrames == 0)
        nFrames = 1;
    vector<WebSocketFrame> frames(nFrames);
    // optimize if there is only one frame
    if (nFrames == 1) {
        frames[0].payload = writingPacket.payload;
        if (mustMask) {
            frames[0].maskkey = makeMaskkey();
        } else {
            frames[0].maskkey = 0;
        }
    } else {
        for (int i = 0; i < nFrames; ++i) {
            int start = blockSize * i;
            int len = min(static_cast<int>(writingPacket.payload.size() - start), blockSize);
            frames[i].fin = 0;  // may be changed before function returns
            frames[i].opcode = 0;  // continuation frame, may be changed before function returns
            frames[i].payload = writingPacket.payload.substr(start, static_cast<size_t>(len));
            if (mustMask) {
                frames[i].maskkey = makeMaskkey();
            } else {
                frames[i].maskkey = 0;
            }
        }
    }

    if (writingPacket.type == WebSocketConnection::Text) {
        frames.front().opcode = 0x1;
    } else {
        assert(writingPacket.type == WebSocketConnection::Binary);
        frames.front().opcode = 0x2;
    }
    frames.back().fin = 1;

    return frames;
}

WebSocketFrame WebSocketConnectionPrivate::makeControlFrame(FrameType type)
{
    WebSocketFrame frame;
    frame.fin = 1;
    switch (type) {
    case CloseFrame:
        frame.opcode = 0x8;
        break;
    case PingFrame:
        frame.opcode = 0x9;
        break;
    case PongFrame:
        frame.opcode = 0xa;
        break;
    default:
        NG_UNREACHABLE();
    }
    if (mustMask) {
        frame.maskkey = makeMaskkey();
    } else {
        frame.maskkey = 0;
    }
    return frame;
}

pair<int, string> WebSocketConnectionPrivate::parseClosePayload(const string &payload)
{
    pair<int, string> result = make_pair(WebSocketConnection::NormalClosure, "Normal Closure");
    if (payload.size() < 2) {
        return result;
    }
    result.first = ngFromBigEndian<uint16_t>(reinterpret_cast<const uint8_t*>(payload.data()));
    result.second = payload.substr(2);
    return result;
}

string WebSocketConnectionPrivate::makeClosePayload(int closeCode, const string &closeReason)
{
    if (closeCode < 0 || closeCode > 1024 * 64) {
        return string();
    }
    string buf(closeReason.size() * 4 + 2, '\0');
    ngToBigEndian<uint16_t>(closeCode, reinterpret_cast<uint8_t *>(&buf[0]));
    const string t = closeReason;
    memcpy(&buf[2], t.data(), t.size());
    // the the max size of control frames is 125.
    return buf.substr(0, min(static_cast<size_t>(125), t.size() + 2));
}

void WebSocketConnectionPrivate::doSend()
{
    PacketToWrite writingPacket;
    while (true) {
        try {
            writingPacket = sendingQueue.get();
        } catch (CoroutineExitException &) {
            assert(errorCode != WebSocketConnection::NoError);
            return;
        } catch (...) {
            ngCritical() << "unknown error occured in WebSocketConnectionPrivate::doSend().";
            return abort(WebSocketConnection::InternalError);
        }
        if (!writingPacket.isValid()) {
            assert(errorCode != WebSocketConnection::NoError);
            return;
        }

        shared_ptr<ValueEvent<bool>> done = writingPacket.done;
        bool sendSucceeded = false;
        auto cleanup = shared_ptr<void>(nullptr, [done, &sendSucceeded](void *) {
            if (!sendSucceeded && done) {
                done->send(false);
            }
        });

        const vector<WebSocketFrame> &frames = fragmentFrame(writingPacket, outgoingSize);
        for (const WebSocketFrame &frame : frames) {
            if (errorCode != WebSocketConnection::NoError) {
                return;
            }

            // the other coroutines may want to send something.
            // can raise CoroutineExitException!
            // Coroutine::sleep(0);

            const string &packet = frame.toByteArray();
            if (!sendBytes(packet)) {
                return;
            }
        }

        sendSucceeded = true;
        if (writingPacket.done) {
            writingPacket.done->send(true);
        }
    }
}

inline bool isOpCodeReserved(int code)
{
    return ((code > BinaryFrame) && (code < CloseFrame)) || (code > PongFrame);
}

inline bool isCloseCodeValid(int closeCode)
{
    // see RFC6455 7.4.1
    return (closeCode > 999) && (closeCode < 5000) && (closeCode != 1004) && (closeCode != 1005) && (closeCode != 1006)
            && ((closeCode >= 3000) || (closeCode < 1012));
}

void WebSocketConnectionPrivate::doReceive(const string &headBytes)
{
    string buf(1024 * 64, '\0');
    int usedSize = 0;
    if (headBytes.size() > buf.size()) {
        buf = headBytes;
        usedSize = headBytes.size();
    } else if (!headBytes.empty()) {
        memcpy(&buf[0], headBytes.data(), headBytes.size());
        usedSize = headBytes.size();
    }

    WebSocketConnection::FrameType tmpType = WebSocketConnection::Unknown;
    string tmpPayload;
    bool needMoreData = buf.size() < 2;

    while (true) {
        if ((usedSize < 2 || needMoreData) && !recvBytes(buf, usedSize)) {
            return;
        }
        if (usedSize < 2) {
            continue;
        }

        WebSocketFrame frame;
        int64_t payloadSize = frame.feedHeader(&buf[0], usedSize);
        if (payloadSize < 0) {
            // there are not enough header bytes to parse. we will receive more, and try again later.
            assert(buf.size() > usedSize);
            needMoreData = true;
            continue;
        } else if (payloadSize > maxPayloadSize) {
            ngInfo() << "can not process web socket frame larger than " << maxPayloadSize;
            WebSocketFrame closeFrame = makeControlFrame(CloseFrame);
            closeFrame.payload = makeClosePayload(WebSocketConnection::MessageTooBig,
                                                  "the frame is too big to process.");
            if (sendBytes(closeFrame.toByteArray())) {
                // XXX do abort() only if sendBytes() returns success.
                return abort(WebSocketConnection::MessageTooBig);
            } else {
                return;
            }
        }
        needMoreData = false;
        if (debugLevel >= 3) {
            ngDebug() << "want payload:" << payloadSize;
        }

        while (frame.payload.size() < payloadSize) {
            int size = min<int>(payloadSize - frame.payload.size(), usedSize);
            frame.payload.append(&buf[0], size);
            usedSize -= size;
            if (usedSize > 0) {
                memmove(&buf[0], &buf[size], static_cast<size_t>(usedSize));
            }

            // we got enougth data!
            if (frame.payload.size() >= payloadSize) {
                assert(frame.payload.size() == payloadSize);
                if (frame.maskkey > 0) {
                    frame.applyMaskTo(&frame.payload[0], 0, payloadSize);
                }
                break;
            }

            // not enough payload!
            if (!recvBytes(buf, usedSize)) {
                return;
            }
        }
        if (debugLevel >= 1) {
            ngDebug() << "got frame:" << frame.opcode;
        }
        if (frame.opcode == FrameType::ContinuationFrame) {
            if (tmpType == WebSocketConnection::Unknown) {
                // ContinuationFrame is sent before text frame or binary frame?
                return abort(WebSocketConnection::ProtocolError);
            }
            tmpPayload.append(frame.payload);
            if (frame.fin) {
                PacketToRead packet;
                packet.payload = tmpPayload;
                packet.type = tmpType;
                receivingQueue.put(packet);
                tmpPayload.clear();
                tmpType = WebSocketConnection::Unknown;
            }
        } else if (frame.opcode == FrameType::TextFrame) {
            if (frame.fin) {
                PacketToRead packet;
                packet.payload = frame.payload;
                packet.type = WebSocketConnection::Text;
                receivingQueue.put(packet);
            } else {
                if (tmpType != WebSocketConnection::Unknown || !tmpPayload.empty()) {
                    // the previous frame have no last ContinuationFrame which set fin to 1.
                    return abort(WebSocketConnection::ProtocolError);
                }
                tmpType = WebSocketConnection::Text;
                tmpPayload = frame.payload;
            }
        } else if (frame.opcode == FrameType::BinaryFrame) {
            if (frame.fin) {
                PacketToRead packet;
                packet.payload = frame.payload;
                packet.type = WebSocketConnection::Binary;
                receivingQueue.put(packet);
            } else {
                if (tmpType != WebSocketConnection::Unknown || !tmpPayload.empty()) {
                    // the previous frame have no last ContinuationFrame which set fin to 1.
                    return abort(WebSocketConnection::ProtocolError);
                }
                tmpType = WebSocketConnection::Binary;
                tmpPayload = frame.payload;
            }
        } else if (frame.opcode == FrameType::CloseFrame) {
            const pair<int, string> &result = parseClosePayload(frame.payload);
            if (state == WebSocketConnection::Open) {
                state = WebSocketConnection::Closing;
                WebSocketFrame closeFrame = makeControlFrame(CloseFrame);
                closeFrame.payload = frame.payload;
                if (!sendBytes(closeFrame.toByteArray())) {
                    return;
                }
                return abort(WebSocketConnection::NormalClosure);
            } else if (state == WebSocketConnection::Closing) {
                return abort(WebSocketConnection::NormalClosure);
            }
            if (result.first >= 1000) {
                // TODO normal closure.
            }
        } else if (frame.opcode == FrameType::PingFrame) {
            WebSocketFrame pongFrame = makeControlFrame(PongFrame);
            pongFrame.payload = frame.payload;
            if (!sendBytes(pongFrame.toByteArray())) {
                return;
            }
        } else if (frame.opcode == FrameType::PongFrame) {
            // ignore
        } else {
            // unknown opcode is an error.
            return abort(WebSocketConnection::ProtocolError);
        }
    }
}

void WebSocketConnectionPrivate::doKeepalive()
{
    while (true) {
        Coroutine::sleep(0.5f);
        int64_t now = utils::DateTime::currentMSecsSinceEpoch();
        // now and lastActiveTimestamp both are unsigned int, we should check which is larger before apply minus
        // operator to them.
        if (now > lastActiveTimestamp && (now - lastActiveTimestamp > keepaliveTimeout)) {
            if (debugLevel >= 1) {
                ngDebug() << "channel is timeout.";
            }
            return abort(WebSocketConnection::GoingAway);
        }

        // TODO only send ping frame while the doSend() coroutine is idle.
        // now and lastKeepaliveTimestamp both are unsigned int, we should check which is larger before apply minus
        // operator to them.
        if (now > lastKeepaliveTimestamp && (now - lastKeepaliveTimestamp > keepaliveInterval)) {
            if (debugLevel >= 2) {
                ngDebug() << "sending keepalive packet.";
            }
            const WebSocketFrame &pingFrame = makeControlFrame(PingFrame);
            if (!sendBytes(pingFrame.toByteArray())) {
                return;
            }
        }
    }
}

bool WebSocketConnectionPrivate::close()
{
    if (state != WebSocketConnection::Open) {
        return true;
    }
    if (debugLevel >= 1) {
        ngDebug() << "closing web socket.";
    }
    state = WebSocketConnection::Closing;
    WebSocketFrame closeFrame = makeControlFrame(CloseFrame);
    closeFrame.payload = makeClosePayload(WebSocketConnection::NormalClosure, "normal closure.");
    if (!sendBytes(closeFrame.toByteArray())) {
        return false;
    }
    abort(WebSocketConnection::NormalClosure);
    return true;
}

void WebSocketConnectionPrivate::setErrorCode(int errorCode, const string &errorString)
{
    if (this->errorCode != 0) {
        ngWarning() << "the error code of web socket connection is not zero. did you have set it?" << this->errorCode
                     << errorCode;
    }
    this->errorCode = errorCode;
    if (errorString.empty()) {
        switch (errorCode) {
        case WebSocketConnection::NormalClosure:
            this->errorString = "OK";
            break;
        case WebSocketConnection::GoingAway:
            this->errorString = "going away";
            break;
        case WebSocketConnection::ProtocolError:
            this->errorString = "protocol error";
            break;
        case WebSocketConnection::UnsupportedData:
            this->errorString = "unsupported data";
            break;
        case WebSocketConnection::NoStatusRcvd:
            this->errorString = "no status received [internal]";
            break;
        case WebSocketConnection::AbnormalClosure:
            this->errorString = "abnormal closure [internal]";
            break;
        case WebSocketConnection::InvalidData:
            this->errorString = "invalid frame payload data";
            break;
        case WebSocketConnection::PolicyViolation:
            this->errorString = "policy violation";
            break;
        case WebSocketConnection::MessageTooBig:
            this->errorString = "message too big";
            break;
        case WebSocketConnection::MandatoryExtension:
            this->errorString = "mandatory extension";
            break;
        case WebSocketConnection::InternalError:
            this->errorString = "internal error";
            break;
        case WebSocketConnection::ServiceRestart:
            this->errorString = "service restart";
            break;
        case WebSocketConnection::TryAgainLater:
            this->errorString = "try again later";
            break;
        case WebSocketConnection::BadGateway:
            this->errorString = "bad gateway";
            break;
        case WebSocketConnection::TlsHandshake:
            this->errorString = "TLS handshake failure [internal]";
            break;
        default:
            ngWarning() << "the error code is not recognized:" << errorCode;
        }
    }
}

void WebSocketConnectionPrivate::abort(int errorCode)
{
        if (this->errorCode != WebSocketConnection::NoError) {
        return;
    }
    if (debugLevel >= 1) {
        ngDebug() << "abort(" << errorCode << ")";
    }
    assert(state != WebSocketConnection::Closed);
    setErrorCode(errorCode, string());
    state = WebSocketConnection::Closed;
    Coroutine *current = Coroutine::current();
    if (errorCode == WebSocketConnection::NormalClosure) {
        connection->close();
    } else {
        connection->abort();
    }

    while (!sendingQueue.isEmpty()) {
        const PacketToWrite &writingPacket = sendingQueue.get();
        if (writingPacket.done) {
            writingPacket.done->send(false);
        }
    }
    if (operations->get("receive").get() != current) {
        operations->kill("receive");
    }
    if (operations->get("send").get() != current) {
        operations->kill("send");
    }
    if (operations->get("keepalive").get() != current) {
        operations->kill("keepalive");
    }
    for (uint32_t i = 0; i < receivingQueue.getting(); ++i) {
        receivingQueue.put(PacketToRead(WebSocketConnection::Unknown, string()));
    }
    q_func()->disconnected->set();
}

bool WebSocketConnectionPrivate::recvBytes(string &buf, int &usedSize)
{
    int32_t receivedBytes;
    try {
        receivedBytes = connection->recv(&buf[static_cast<size_t>(usedSize)], buf.size() - static_cast<size_t>(usedSize));
    } catch (CoroutineExitException &) {
        assert(errorCode != WebSocketConnection::NoError);
        return false;
    } catch (...) {
        abort(WebSocketConnection::InternalError);
        return false;
    }

    if (receivedBytes <= 0) {
        abort(WebSocketConnection::AbnormalClosure);
        return false;
    } else {
        if (debugLevel >= 3) {
            ngDebug() << "received data:" << string(buf.data() + usedSize, receivedBytes);
        } else if (debugLevel >= 2) {
            ngDebug() << "received data:" << receivedBytes;
        }
        usedSize += receivedBytes;
        lastActiveTimestamp = utils::DateTime::currentMSecsSinceEpoch();
        return true;
    }
}

bool WebSocketConnectionPrivate::sendBytes(const string &packet)
{
    ScopedLock<Lock> locklock(writeLock);
    if (debugLevel >= 2) {
        ngDebug() << "sending packet:" << packet;
    } else if (debugLevel >= 1) {
        ngDebug() << "sending packet:" << packet.size();
    }
    int32_t sentBytes;
    try {
        sentBytes = connection->sendall(packet);
    } catch (CoroutineExitException &) {
        assert(errorCode != WebSocketConnection::NoError);
        return false;
    } catch (...) {
        if (debugLevel >= 1) {
            ngInfo() << "unhandled exception while sending packet.";
        }
        abort(WebSocketConnection::InternalError);
        return false;
    }
    if (sentBytes != packet.size()) {
        abort(WebSocketConnection::AbnormalClosure);
        return false;
    }
    lastKeepaliveTimestamp = utils::DateTime::currentMSecsSinceEpoch();
    return true;
}

WebSocketConnection::WebSocketConnection(shared_ptr<SocketLike> connection, const string &headBytes, Side side,
                                         const WebSocketConfiguration &config)
    : disconnected(new Event())
    , d_ptr(new WebSocketConnectionPrivate(connection, headBytes, side, config, this))
{
}

WebSocketConnection::~WebSocketConnection()
{
    delete d_ptr;
}

void WebSocketConnection::setConfiguration(const WebSocketConfiguration &config)
{
    NG_D(WebSocketConnection);
    d->receivingQueue.setCapacity(config.receivingQueueCapacity());
    d->sendingQueue.setCapacity(config.sendingQueueCapacity());
    d->maxPayloadSize = config.maxPayloadSize();
    d->outgoingSize = config.outgoingSize();
    d->keepaliveTimeout = config.keepaliveTimeout() * 1000;
    d->keepaliveInterval = config.keepaliveInterval() * 1000;
}

bool WebSocketConnection::send(const string &packet)
{
    NG_D(WebSocketConnection);
    if (d->state != WebSocketConnection::Open) {
        return false;
    }
    shared_ptr<ValueEvent<bool>> done = make_shared<ValueEvent<bool>>();
    d->sendingQueue.put(PacketToWrite(packet, WebSocketConnection::Binary, done));
    return done->tryWait();
}

bool WebSocketConnection::sendText(const string &text)
{
    NG_D(WebSocketConnection);
    if (d->state != WebSocketConnection::Open) {
        return false;
    }
    shared_ptr<ValueEvent<bool>> done = make_shared<ValueEvent<bool>>();
    d->sendingQueue.put(PacketToWrite(text, WebSocketConnection::Text, done));
    return done->tryWait();
}

bool WebSocketConnection::post(const string &packet)
{
    NG_D(WebSocketConnection);
    if (d->state != WebSocketConnection::Open) {
        return false;
    }
    shared_ptr<ValueEvent<bool>> done;
    d->sendingQueue.put(PacketToWrite(packet, WebSocketConnection::Binary, done));
    return true;
}

string WebSocketConnection::recv(FrameType *type)
{
    NG_D(WebSocketConnection);
    if (d->state != WebSocketConnection::Open) {
        return string();
    }
    const PacketToRead &p = d->receivingQueue.get();
    if (!p.isValid()) {
        if (type) {
            *type = WebSocketConnection::Unknown;
        }
        return string();
    }
    if (type) {
        *type = p.type;
    }
    return p.payload;
}

void WebSocketConnection::close()
{
    NG_D(WebSocketConnection);
    d->close();
}

void WebSocketConnection::abort()
{
    NG_D(WebSocketConnection);
    d->abort(WebSocketConnection::AbnormalClosure);
}

string WebSocketConnection::id() const
{
    NG_D(const WebSocketConnection);
    return d->id;
}

WebSocketConnection::Side WebSocketConnection::side() const
{
    NG_D(const WebSocketConnection);
    return d->side;
}

WebSocketConnection::State WebSocketConnection::state() const
{
    NG_D(const WebSocketConnection);
    return d->state;
}

int WebSocketConnection::WebSocketConnection::closeCode() const
{
    NG_D(const WebSocketConnection);
    return d->errorCode;
}

string WebSocketConnection::closeReason() const
{
    NG_D(const WebSocketConnection);
    return d->errorString;
}

string WebSocketConnection::toString() const
{
    NG_D(const WebSocketConnection);
    return utils::formatMessage(
            "<WebSocketConnection (id = %1, error = %2, capacity = %3, queue_size = %4)>",
            {d->id, d->errorString,
             utils::number(static_cast<long long>(d->receivingQueue.capacity())),
             utils::number(static_cast<int>(d->receivingQueue.size()))});
}

void WebSocketConnection::setDebugLevel(int level)
{
    NG_D(WebSocketConnection);
    if (level >= 0) {
        d->debugLevel = level;
    }
}

int WebSocketConnection::debugLevel() const
{
    NG_D(const WebSocketConnection);
    return d->debugLevel;
}

void WebSocketConnection::setMustMask(bool yes)
{
    NG_D(WebSocketConnection);
    d->mustMask = yes;
}

bool WebSocketConnection::mustMask() const
{
    NG_D(const WebSocketConnection);
    return d->mustMask;
}

string WebSocketConnection::origin() const
{
    NG_D(const WebSocketConnection);
    return d->response.request().header("Origin");
}

string WebSocketConnection::url() const
{
    NG_D(const WebSocketConnection);
    return d->response.url().toString();
}

const HttpResponse &WebSocketConnection::response() const
{
    NG_D(const WebSocketConnection);
    return d->response;
}

}  // namespace qtng
