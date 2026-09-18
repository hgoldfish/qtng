#ifndef QTNG_KCP_H
#define QTNG_KCP_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "qtng/network_interface.h"
#include "qtng/socket.h"
#include "qtng/udp.h"
#include "qtng/utils/platform.h"

namespace qtng {

struct KcpStreamStats {
    std::uint32_t sndWnd;          // effective send window = min(BDP, mem, rmt) (segments)
    std::uint32_t sendBudgetSegs;  // Tuner BDP budget (segments)
    std::uint32_t memoryCapSegs;   // memory budget converted to segments
    std::uint32_t rtoResends;      // cumulative RTO retransmits (kcp->xmit)
    std::uint32_t fastResends;     // cumulative fast retransmits (kcp->xmit_fast)
    std::uint32_t fastresend;      // current fastresend threshold
    double lossRate;               // Tuner estimate
    double deliveryBps;            // Tuner estimate (bits/s)
};

class KcpStreamPrivate;
class KcpStream
{
public:
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

    // When true (default), Tuner loss above 5% shrinks the send budget, but
    // the loss-only floor is the cold-start budget (256 segments), not a
    // collapsed BDP. When false, loss does not shrink the budget; only
    // sustained queuing delay does. SLOW turns this off: multipath loss is
    // not congestion. Accept()-ed slaves snapshot the flag at construction.
    void setLossBasedBudget(bool enabled);
    bool lossBasedBudget() const;

    // Memory budget for the send path (bytes). Converted to segments via
    // (mss + 72). Effective snd_wnd is min(BDP budget, this limit, rmt_wnd).
    void setSendBufferLimit(std::uint64_t bytes);
    std::uint64_t sendBufferLimit() const;
    // MTU for ikcp segments. Default 1400. Accept()-ed slaves snapshot the
    // master's MTU at construction time; later setPacketSize() on the master
    // does not propagate to already-accepted slaves. Refused while waitsnd()>0.
    void setPacketSize(std::uint32_t packetSize);
    std::uint32_t packetSize() const;
    std::uint32_t payloadSizeHint() const;
    void setTearDownTime(float secs);
    float tearDownTime() const;
    KcpStreamStats stats() const;
    Event busy;
    Event notBusy;

    // Multipath / redundant-send heuristics: does this DatagramLink plaintext
    // contain a control frame that should be sprayed across paths (CLOSE /
    // KEEPALIVE / ikcp ACK / ACKN)? Shares cmd constants with the wire parser.
    static bool plaintextLooksCritical(const char *data, std::int32_t size);

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
    KcpStream(KcpStreamPrivate *d, const DatagramPath &remote);
    friend class SlaveKcpStreamPrivate;
    friend class MasterKcpStreamPrivate;
private:
    KcpStreamPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(KcpStream)
};

class KcpSocketPrivate;
class KcpSocket
{
public:
    explicit KcpSocket(HostAddress::NetworkLayerProtocol protocol = HostAddress::IPv4Protocol);
    explicit KcpSocket(std::intptr_t socketDescriptor);
    explicit KcpSocket(std::shared_ptr<Socket> rawSocket);
    virtual ~KcpSocket();
public:
    void setSendBufferLimit(std::uint64_t bytes);
    std::uint64_t sendBufferLimit() const;
    void setUdpPacketSize(std::uint32_t udpPacketSize);
    std::uint32_t udpPacketSize() const;
    std::uint32_t payloadSizeHint() const;
    void setTearDownTime(float secs);
    float tearDownTime() const;
    KcpStreamStats stats() const;
public:
    Socket::SocketError error() const;
    std::string errorString() const;
    bool isValid() const;
    HostAddress localAddress() const;
    std::uint16_t localPort() const;
    HostAddress peerAddress() const;
    std::string peerName() const;
    std::uint16_t peerPort() const;
    Socket::SocketType type() const;
    Socket::SocketState state() const;
    HostAddress::NetworkLayerProtocol protocol() const;
    std::string localAddressURI() const;
    std::string peerAddressURI() const;

    KcpSocket *accept();
    KcpSocket *accept(const HostAddress &addr, std::uint16_t port);
    KcpSocket *accept(const std::string &hostName, std::uint16_t port,
                      std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>());

    bool bind(const HostAddress &address, std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool bind(std::uint16_t port = 0, Socket::BindMode mode = Socket::DefaultForPlatform);
    bool connect(const HostAddress &addr, std::uint16_t port);
    bool connect(const std::string &hostName, std::uint16_t port,
                 std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>());
    void close();
    void abort();
    bool listen(int backlog);
    bool setOption(Socket::SocketOption option, int value);
    int option(Socket::SocketOption option) const;

    bool joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    bool leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface = NetworkInterface());
    NetworkInterface multicastInterface() const;
    bool setMulticastInterface(const NetworkInterface &iface);

    std::int32_t peek(char *data, std::int32_t size);
    std::int32_t peekRaw(char *data, std::int32_t size);
    std::int32_t recv(char *data, std::int32_t size);
    std::int32_t recvall(char *data, std::int32_t size);
    std::int32_t send(const char *data, std::int32_t size);
    std::int32_t sendall(const char *data, std::int32_t size);
    std::string recv(std::int32_t size);
    std::string recvall(std::int32_t size);
    std::int32_t send(const std::string &data);
    std::int32_t sendall(const std::string &data);

    virtual bool filter(char *data, std::int32_t *len, HostAddress *addr, std::uint16_t *port);
    void setFilter(std::function<bool(char *, std::int32_t *, HostAddress *, std::uint16_t *)> callback);
    std::int32_t udpSend(const char *data, std::int32_t size, const HostAddress &addr, std::uint16_t port);
    std::int32_t udpSend(const std::string &packet, const HostAddress &addr, std::uint16_t port)
    {
        return udpSend(packet.data(), packet.size(), addr, port);
    }

    static KcpSocket *createConnection(const HostAddress &host, std::uint16_t port, Socket::SocketError *error = nullptr,
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    static KcpSocket *createConnection(const std::string &hostName, std::uint16_t port, Socket::SocketError *error = nullptr,
                                       std::shared_ptr<SocketDnsCache> dnsCache = std::shared_ptr<SocketDnsCache>(),
                                       int allowProtocol = HostAddress::IPv4Protocol | HostAddress::IPv6Protocol);
    // if backlog == 0, do not bind and listen.
    static KcpSocket *createServer(const HostAddress &host, std::uint16_t port, int backlog = 50);
private:
    explicit KcpSocket(std::shared_ptr<KcpStream> stream);
    friend KcpSocket *wrapKcpStreamAsSocket(std::shared_ptr<KcpStream> stream);
private:
    KcpSocketPrivate * const d_ptr;
    NG_DECLARE_PRIVATE(KcpSocket)
    NG_DISABLE_COPY_MOVE(KcpSocket)
};

// Wrap an existing KcpStream (any DatagramLink) as a KcpSocket. UDP-only helpers
// are no-ops / fail when the underlying link is not UdpDatagramLink.
KcpSocket *wrapKcpStreamAsSocket(std::shared_ptr<KcpStream> stream);

std::shared_ptr<class SocketLike> asSocketLike(std::shared_ptr<KcpSocket> s);

inline std::shared_ptr<class SocketLike> asSocketLike(KcpSocket *s)
{
    return asSocketLike(std::shared_ptr<KcpSocket>(s));
}

std::shared_ptr<KcpSocket> convertSocketLikeToKcpSocket(std::shared_ptr<class SocketLike> socket);

}  // namespace qtng
#endif  // QTNG_KCP_H
