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
    // Wire framing always carries sessionId on KcpStream control commands
    // (CREATE_MULTIPATH / CLOSE / KEEPALIVE): [1-byte type][4-byte BE sessionId][pad...]
    //
    // DatagramLink feeds packets that start at cmd (no leading ikcp conv):
    //   - cmd 0x51-0x54: native ikcp body. Recv loops (doReceive / doAccept) keep
    //     4 bytes of headroom before the wire payload so handleDatagram can pass
    //     a zero-conv prefix to ikcp_input without copying.
    //   - cmd 0x01: legacy DATA; conv overlay is zeroed in place at bytes 1-4
    //   - cmd 0x02-0x04: KcpStream control
    //
    // protocolVersion controls how ikcp output is sent:
    //   1 (default, KcpSocket): wrap as DATA with sessionId overlaid on conv
    //   2 (SlowSocket): strip the 4-byte conv and send [cmd][payload...] directly
    enum ProtocolVersion : std::uint8_t {
        Version1 = 1,
        Version2 = 2,
    };
public:
    explicit KcpStream(std::shared_ptr<DatagramLink> link, std::uint32_t sessionId = 0);
    virtual ~KcpStream();
public:
    std::shared_ptr<DatagramLink> link() const;

    std::uint32_t sessionId() const;
    void setSessionId(std::uint32_t id);

    void setProtocolVersion(std::uint8_t version);
    std::uint8_t protocolVersion() const;

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
