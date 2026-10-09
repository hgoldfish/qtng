#include "qtng/udp.h"
#include "qtng/private/udp_p.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

UdpDatagramPath::UdpDatagramPath()
    : m_port(0)
{
}

UdpDatagramPath::UdpDatagramPath(const HostAddress &addr, uint16_t port)
    : m_addr(addr)
    , m_port(port)
{
}

UdpDatagramPath::UdpDatagramPath(const DatagramPath &path)
    : m_port(0)
{
    const string &key = path.key();
    if (key.empty()) {
        return;
    }
    size_t pos = key.rfind(':');
    if (pos == string::npos || pos == 0) {
        return;
    }
    string host = key.substr(0, pos);
    string portStr = key.substr(pos + 1);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    unsigned long p = 0;
    try {
        size_t idx = 0;
        p = stoul(portStr, &idx);
        if (idx != portStr.size() || p > 65535) {
            return;
        }
    } catch (...) {
        return;
    }
    if (m_addr.setAddress(host)) {
        m_port = static_cast<uint16_t>(p);
    }
}

DatagramPath UdpDatagramPath::toPath() const
{
    if (isNull()) {
        return DatagramPath();
    }
    string key;
    if (m_addr.protocol() == HostAddress::IPv6Protocol) {
        key = "[" + m_addr.toString() + "]:" + utils::number(m_port);
    } else {
        key = m_addr.toString() + ":" + utils::number(m_port);
    }
    return DatagramPath(key);
}

bool UdpDatagramPath::isNull() const
{
    return m_addr.isNull() || m_port == 0;
}

UdpDatagramLink::UdpDatagramLink(HostAddress::NetworkLayerProtocol protocol)
    : rawSocket(make_shared<Socket>(protocol, Socket::UdpSocket))
{
}

UdpDatagramLink::UdpDatagramLink(intptr_t socketDescriptor)
    : rawSocket(make_shared<Socket>(socketDescriptor))
{
}

UdpDatagramLink::UdpDatagramLink(shared_ptr<Socket> s)
    : rawSocket(std::move(s))
{
}

UdpDatagramLink::~UdpDatagramLink() {}

shared_ptr<Socket> UdpDatagramLink::socket() const
{
    return rawSocket;
}

bool UdpDatagramLink::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    if (mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    return rawSocket->bind(address, port, mode);
}

bool UdpDatagramLink::bind(uint16_t port, Socket::BindMode mode)
{
    if (mode & Socket::ReuseAddressHint) {
        rawSocket->setOption(Socket::AddressReusable, true);
    }
    return rawSocket->bind(port, mode);
}

bool UdpDatagramLink::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return rawSocket->joinMulticastGroup(groupAddress, iface);
}

bool UdpDatagramLink::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return rawSocket->leaveMulticastGroup(groupAddress, iface);
}

NetworkInterface UdpDatagramLink::multicastInterface() const
{
    return rawSocket->multicastInterface();
}

bool UdpDatagramLink::setMulticastInterface(const NetworkInterface &iface)
{
    return rawSocket->setMulticastInterface(iface);
}

void UdpDatagramLink::setFilter(function<bool(char *, int32_t *, HostAddress *, uint16_t *)> callback)
{
    filterCallback = std::move(callback);
}

int32_t UdpDatagramLink::recvfrom(char *data, int32_t size, DatagramPath *who)
{
    HostAddress addr;
    uint16_t port = 0;
    int32_t len = rawSocket->recvfrom(data, size, &addr, &port);
    if (len < 0) {
        return len;
    }
    if (filterCallback) {
        int32_t filteredLen = len;
        if (filterCallback(data, &filteredLen, &addr, &port)) {
            return 0;
        }
        len = filteredLen;
    }
    if (who) {
        *who = UdpDatagramPath(addr, port).toPath();
    }
    return len;
}

int32_t UdpDatagramLink::sendto(const char *data, int32_t size, const DatagramPath &who)
{
    const UdpDatagramPath udp(who);
    if (udp.isNull()) {
        return -1;
    }
    return rawSocket->sendto(data, size, udp.address(), udp.port());
}

int32_t UdpDatagramLink::recvfrom(std::vector<Datagram> *datagrams)
{
    if (!rawSocket) {
        return -1;
    }
    const int32_t total = rawSocket->recvfrom(datagrams);
    if (total < 0 || !filterCallback) {
        return total;
    }
    // Apply the per-datagram filter: compact the kept entries in place and drop
    // the rest, subtracting the dropped bytes from the total.
    const size_t count = datagrams->size();
    size_t kept = 0;
    int32_t keptTotal = 0;
    for (size_t i = 0; i < count; ++i) {
        Datagram datagram = (*datagrams)[i];
        // The filter may rewrite the payload in place; the buffer belongs to the
        // caller, who filled it in before the call.
        if (filterCallback(datagram.data, &datagram.len, &datagram.addr, &datagram.port)) {
            continue;
        }
        (*datagrams)[kept] = datagram;
        keptTotal += datagram.len;
        ++kept;
    }
    datagrams->resize(kept);
    return keptTotal;
}

void UdpDatagramLink::close()
{
    rawSocket->close();
}

void UdpDatagramLink::abort()
{
    rawSocket->abort();
}

bool UdpDatagramLink::isValid() const
{
    return rawSocket && rawSocket->isValid();
}

Socket::SocketError UdpDatagramLink::error() const
{
    return rawSocket->error();
}

string UdpDatagramLink::errorString() const
{
    return rawSocket->errorString();
}

HostAddress UdpDatagramLink::localAddress() const
{
    return rawSocket->localAddress();
}

uint16_t UdpDatagramLink::localPort() const
{
    return rawSocket->localPort();
}

HostAddress::NetworkLayerProtocol UdpDatagramLink::protocol() const
{
    return rawSocket->protocol();
}

int32_t UdpDatagramLink::peek(char *data, int32_t size)
{
    return rawSocket->peek(data, size);
}

bool UdpDatagramLink::setOption(Socket::SocketOption option, int value)
{
    return rawSocket->setOption(option, value);
}

int UdpDatagramLink::option(Socket::SocketOption option) const
{
    return rawSocket->option(option);
}

DatagramPath::DatagramPath()
{
}

DatagramPath::DatagramPath(const string &key)
    : m_key(key)
{
}

string DatagramPath::key() const { return m_key; }

bool DatagramPath::isNull() const
{
    return m_key.empty();
}

bool DatagramPath::operator==(const DatagramPath &other) const { return m_key == other.m_key; }
bool DatagramPath::operator<(const DatagramPath &other) const { return m_key < other.m_key; }

DatagramLink::~DatagramLink() {}
Socket::SocketError DatagramLink::error() const { return Socket::NoError; }
string DatagramLink::errorString() const { return string(); }

int32_t DatagramLink::recvfrom(std::vector<Datagram> *datagrams)
{
    if (!datagrams || datagrams->empty()) {
        return -1;
    }
    const size_t maxCount = datagrams->size();
    for (size_t i = 0; i < maxCount; ++i) {
        if (!datagrams->at(i).data || datagrams->at(i).len <= 0) {
            return -1;
        }
    }
    size_t count = 0;
    int32_t total = 0;
    while (count < maxCount) {
        Datagram &datagram = datagrams->at(count);
        DatagramPath who;
        const int32_t n = recvfrom(datagram.data, datagram.len, &who);
        if (n < 0) {
            return count == 0 ? -1 : total;
        }
        if (n == 0) {
            // No more datagrams queued right now.
            break;
        }
        const UdpDatagramPath udp(who);
        datagram.len = n;
        datagram.addr = udp.address();
        datagram.port = udp.port();
        total += n;
        ++count;
    }
    datagrams->resize(count);
    return total;
}

}  // namespace qtng
