#ifndef QTNG_HOSTADDRESS_H
#define QTNG_HOSTADDRESS_H

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "qtng/utils/platform.h"

struct sockaddr;

namespace qtng {

struct IPv6Address
{
    inline std::uint8_t &operator[](int index) { return c[index]; }
    inline std::uint8_t operator[](int index) const { return c[index]; }
    std::uint8_t c[16];
};

typedef std::uint32_t IPv4Address;

#ifdef NG_OS_WIN
void initWinSock();
void freeWinSock();
#endif

class HostAddressPrivate;
class HostAddress
{
public:
    enum SpecialAddress { Null, Broadcast, LocalHost, LocalHostIPv6, Any, AnyIPv6, AnyIPv4 };
    enum NetworkLayerProtocol {
        IPv4Protocol = 1,
        IPv6Protocol = 2,
        AnyIPProtocol = 3,
        UnknownNetworkLayerProtocol = -1
    };
    enum ConversionModeFlag {
        ConvertV4MappedToIPv4 = 1,
        ConvertV4CompatToIPv4 = 2,
        ConvertUnspecifiedAddress = 4,
        ConvertLocalHost = 8,
        TolerantConversion = 0xff,
        StrictConversion = 0
    };
public:
    HostAddress();
    HostAddress(const HostAddress &copy);
    HostAddress(SpecialAddress address);
    HostAddress(const std::string &address);
    HostAddress(std::uint32_t ip4Addr);
    HostAddress(std::uint8_t *ip6Addr);
    HostAddress(const std::uint8_t *ip6Addr);
    HostAddress(const IPv6Address &ip6Addr);
    HostAddress(const sockaddr *sockaddr);
    ~HostAddress();

    HostAddress &operator=(HostAddress &&other) noexcept
    {
        swap(other);
        return *this;
    }
    HostAddress &operator=(const HostAddress &other);
    HostAddress &operator=(SpecialAddress address);

    bool isEqual(const HostAddress &address, ConversionModeFlag mode = TolerantConversion) const;
    bool operator==(const HostAddress &address) const;
    bool operator==(SpecialAddress address) const;
    inline bool operator!=(const HostAddress &address) const { return !operator==(address); }
    inline bool operator!=(SpecialAddress address) const { return !operator==(address); }

    void swap(HostAddress &other) noexcept;
    void clear();
public:
    void setAddress(const IPv4Address ipv4);
    void setAddress(const IPv6Address &ipv6);
    void setAddress(const std::uint8_t *ipv6);
    bool setAddress(const std::string &ipString);
    void setAddress(SpecialAddress address);

    bool isNull() const;
    NetworkLayerProtocol protocol() const;
    IPv4Address toIPv4Address() const { return toIPv4Address(nullptr); }
    IPv4Address toIPv4Address(bool *ok) const;
    IPv6Address toIPv6Address() const;

    bool isInSubnet(const HostAddress &subnet, int netmask) const;
    bool isInSubnet(const std::pair<HostAddress, int> &subnet) const;
    static std::pair<HostAddress, int> parseSubnet(const std::string &subnet);

    bool isIPv4() const;
    bool isLoopback() const;
    bool isGlobal() const;
    bool isLinkLocal() const;
    bool isSiteLocal() const;
    bool isUniqueLocalUnicast() const;
    bool isMulticast() const;
    bool isBroadcast() const;
    std::string toString() const;
    std::string scopeId() const;
    void setScopeId(const std::string &id);

    static std::vector<HostAddress> getHostAddressByName(const std::string &hostName);

    friend uint qHash(const HostAddress &key, uint seed) noexcept;
private:
    friend class HostAddressPrivate;
    std::shared_ptr<HostAddressPrivate> d;
};

}  // namespace qtng

#ifndef NG_NO_DEBUG_STREAM
#include "qtng/utils/platform.h"
std::ostream &operator<<(std::ostream &out, const qtng::HostAddress &t);
#endif

#endif  // QTNG_HOSTADDRESS_H
