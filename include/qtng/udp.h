#ifndef QTNG_UDP_H
#define QTNG_UDP_H

#include <cstdint>
#include <string>
#include <vector>

#include "qtng/socket.h"

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
    // Return size if the datagram was queued or intentionally dropped.
    // 0, a short write, or a negative value is a hard failure: KcpStream
    // closes the session. UDP-style drop-on-full must still return size.
    virtual std::int32_t sendto(const char *data, std::int32_t size, const DatagramPath &who) = 0;

    // Batch receive, mirroring Socket's: returns the number of bytes handled,
    // not the number of datagrams. The caller sizes and fills *datagrams
    // (buffer in data, capacity in len); see Datagram in socket.h.
    //
    // KcpStream drains several datagrams per call, so a DatagramLink feeding a
    // KcpStream must carry address-shaped paths: Datagram only holds addr/port,
    // so a link with an opaque DatagramPath key (not convertible to HostAddress
    // + port) would lose the peer identity. Such links must override this with a
    // correct implementation, or not be used with KcpStream.
    //
    // The default implementation loops over the single-datagram recvfrom() and
    // stops as soon as it reports no more data (0) or a failure (< 0), so it
    // assumes recvfrom() does not block forever once the queue is empty. Links
    // that can do better (UdpDatagramLink backed by Socket) override it to reach
    // recvmmsg.
    virtual std::int32_t recvfrom(std::vector<Datagram> *datagrams);

    virtual void close() = 0;
    virtual void abort() = 0;
    virtual bool isValid() const = 0;
    virtual Socket::SocketError error() const;
    virtual std::string errorString() const;
};

}  // namespace qtng
#endif  // QTNG_UDP_H
