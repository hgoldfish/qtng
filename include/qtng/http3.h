#ifndef QTNG_HTTP3_H
#define QTNG_HTTP3_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qtng/utils/platform.h"

namespace qtng {

class QuicConnection;
class QuicStream;
class Http3ConnectionPrivate;
class Http3StreamPrivate;

// HTTP/3 frame types (RFC 9114 §7.2.1).
enum Http3FrameType {
    H3Data = 0x00,
    H3Headers = 0x01,
    H3CancelPush = 0x03,
    H3Settings = 0x04,
    H3PushPromise = 0x05,
    H3Goaway = 0x07,
    H3MaxPushId = 0x0d,
    H3DuplicatePush = 0x0e,
};

// QPACK static table index for a header field name, or -1.
int qpackStaticTableIndex(const std::string &name);
// QPACK encode/decode of a header field section (RFC 9204, static table + literal only).
std::string qpackEncodeHeaders(const std::vector<std::pair<std::string, std::string>> &headers);
bool qpackDecodeHeaders(const std::string &data, std::vector<std::pair<std::string, std::string>> *headers);

// A single HTTP/3 request/response stream, carrying HTTP/3 frames over a
// QUIC bidirectional stream.
class Http3Stream
{
    NG_DISABLE_COPY(Http3Stream)
public:
    ~Http3Stream();

    std::uint64_t streamId() const;
    bool isValid() const;

    // Send application data as a DATA frame.
    std::int32_t sendData(const char *data, std::int32_t size);
    std::int32_t sendData(const std::string &data);
    // Send header fields as a HEADERS frame (QPACK-encoded).
    bool sendHeaders(const std::vector<std::pair<std::string, std::string>> &headers);
    // Receive one frame; returns the payload length, -1 on error/EOF.
    // The frame type is stored in *frameType and payload in *payload.
    std::int32_t recvFrame(std::uint8_t *frameType, std::string *payload);

    void close();
private:
    explicit Http3Stream(Http3StreamPrivate *d);
    friend class Http3Connection;
    friend class Http3ConnectionPrivate;
    friend class Http3StreamPrivate;
private:
    Http3StreamPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(Http3Stream)
};

// HTTP/3 connection (RFC 9114). Wraps a QUIC connection, manages the control and
// QPACK streams, and exposes request/response streams.
class Http3Connection
{
    NG_DISABLE_COPY(Http3Connection)
public:
    explicit Http3Connection(std::shared_ptr<QuicConnection> conn);
    ~Http3Connection();

    bool isValid() const;
    bool isServer() const;

    // Open a new request/response stream (client side) or accept a peer-initiated
    // stream (server side).
    std::shared_ptr<Http3Stream> openStream();
    std::shared_ptr<Http3Stream> acceptStream();

    // QPACK helpers.
    std::string encodeHeaders(const std::vector<std::pair<std::string, std::string>> &headers) const;
    bool decodeHeaders(const std::string &payload,
                       std::vector<std::pair<std::string, std::string>> *headers) const;

    void close();
private:
    Http3ConnectionPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(Http3Connection)
};

}  // namespace qtng

#endif  // QTNG_HTTP3_H
