#ifndef QTNG_UDP_H
#define QTNG_UDP_H

#include <cstdint>
#include <string>

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
    virtual std::int32_t sendto(const char *data, std::int32_t size, const DatagramPath &who) = 0;
    virtual void close() = 0;
    virtual void abort() = 0;
    virtual bool isValid() const = 0;
    virtual Socket::SocketError error() const;
    virtual std::string errorString() const;
};

}  // namespace qtng
#endif  // QTNG_UDP_H
