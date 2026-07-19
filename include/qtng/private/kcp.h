#ifndef QTNG_KCP_H
#define QTNG_KCP_H

#include <cstdint>
#include <memory>
#include <string>

#include "qtng/socket.h"
#include "qtng/utils/platform.h"

namespace qtng {

// Opaque peer identity for a datagram transport. Not tied to IP/port —
// ICMP / multipath / custom links may use any non-empty key string.
class DatagramPath
{
public:
    DatagramPath();
    explicit DatagramPath(const std::string &key);

    std::string key() const;
    bool isNull() const;

    bool operator==(const DatagramPath &other) const;
    bool operator!=(const DatagramPath &other) const { return !(*this == other); }
    bool operator<(const DatagramPath &other) const;
private:
    std::string m_key;
};

class DatagramLink
{
public:
    virtual ~DatagramLink();
    virtual std::int32_t recvfrom(char *data, std::int32_t size, DatagramPath *who) = 0;
    virtual std::int32_t sendto(const char *data, std::int32_t size, const DatagramPath &who) = 0;
    virtual void close() = 0;
    virtual void abort() = 0;
    virtual bool isValid() const = 0;
    virtual Socket::SocketError error() const;
    virtual std::string errorString() const;
};

class KcpStreamPrivate;
class KcpStream
{
public:
    enum Mode {
        LargeDelayInternet,
        Internet,
        FastInternet,
        Ethernet,
        Loopback,
    };
    // Builtin: [1-byte type][4-byte BE sessionId overlay on ikcp conv][payload]
    // External: [1-byte type][payload] — sessionId is managed outside the wire header.
    enum HeaderMode { Builtin, External };
public:
    explicit KcpStream(std::shared_ptr<DatagramLink> link);
    virtual ~KcpStream();
public:
    std::shared_ptr<DatagramLink> link() const;

    void setHeaderMode(HeaderMode mode);
    HeaderMode headerMode() const;
    std::uint32_t sessionId() const;
    void setSessionId(std::uint32_t id);

    void setMode(Mode mode);
    Mode mode() const;
    void setSendQueueSize(std::uint32_t sendQueueSize);
    std::uint32_t sendQueueSize() const;
    void setPacketSize(std::uint32_t packetSize);
    std::uint32_t packetSize() const;
    std::uint32_t payloadSizeHint() const;
    void setTearDownTime(float secs);
    float tearDownTime() const;
    Event busy;
    Event notBusy;
public:
    Socket::SocketError error() const;
    std::string errorString() const;
    bool isValid() const;
    DatagramPath peerPath() const;
    Socket::SocketState state() const;

    KcpStream *accept();
    KcpStream *accept(const DatagramPath &remote);

    bool connect(const DatagramPath &remote);
    // Transition Unconnected -> Bound after the underlying DatagramLink is ready.
    // Required before listen().
    bool markBound();
    void close();
    void abort();
    bool listen(int backlog);

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);
private:
    KcpStream(KcpStreamPrivate *d, const DatagramPath &remote, Mode mode);
    friend class SlaveKcpStreamPrivate;
    friend class MasterKcpStreamPrivate;
private:
    KcpStreamPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(KcpStream)
};

}  // namespace qtng

#endif  // QTNG_KCP_H
