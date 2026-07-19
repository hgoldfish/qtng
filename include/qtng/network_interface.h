#ifndef QTNG_NETWORK_INTERFACE_H
#define QTNG_NETWORK_INTERFACE_H

#include <algorithm>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "qtng/hostaddress.h"

namespace qtng {

class NetworkAddressEntryPrivate;
class NetworkAddressEntry
{
public:
    enum DnsEligibilityStatus : std::int8_t { DnsEligibilityUnknown = -1, DnsIneligible = 0, DnsEligible = 1 };

    NetworkAddressEntry();
    NetworkAddressEntry(const NetworkAddressEntry &other);
    NetworkAddressEntry(NetworkAddressEntry &&other) noexcept;
    NetworkAddressEntry &operator=(const NetworkAddressEntry &other);
    NetworkAddressEntry &operator=(NetworkAddressEntry &&other) noexcept;
    ~NetworkAddressEntry();

    void swap(NetworkAddressEntry &other) noexcept { std::swap(d, other.d); }

    bool operator==(const NetworkAddressEntry &other) const;
    inline bool operator!=(const NetworkAddressEntry &other) const { return !(*this == other); }

    DnsEligibilityStatus dnsEligibility() const;
    void setDnsEligibility(DnsEligibilityStatus status);

    HostAddress ip() const;
    void setIp(const HostAddress &newIp);

    HostAddress netmask() const;
    void setNetmask(const HostAddress &newNetmask);
    int prefixLength() const;
    void setPrefixLength(int length);

    HostAddress broadcast() const;
    void setBroadcast(const HostAddress &newBroadcast);

    bool isLifetimeKnown() const;
    void setAddressLifetime(std::int64_t preferredMs, std::int64_t validityMs);
    void clearAddressLifetime();
    bool isPermanent() const;

private:
    std::unique_ptr<NetworkAddressEntryPrivate> d;
};

class NetworkInterfacePrivate;
class NetworkInterface
{
public:
    enum InterfaceFlag {
        IsUp = 0x1,
        IsRunning = 0x2,
        CanBroadcast = 0x4,
        IsLoopBack = 0x8,
        IsPointToPoint = 0x10,
        CanMulticast = 0x20
    };
    enum InterfaceType {
        Loopback = 1,
        Virtual,
        Ethernet,
        Slip,
        CanBus,
        Ppp,
        Fddi,
        Wifi,
        Ieee80211 = Wifi,  // alias
        Phonet,
        Ieee802154,
        SixLoWPAN,  // 6LoWPAN, but we can't start with a digit
        Ieee80216,
        Ieee1394,

        Unknown = 0
    };
    using InterfaceFlags = int;
public:
    NetworkInterface();
    NetworkInterface(const NetworkInterface &other);
    NetworkInterface(NetworkInterface &&other) noexcept
    {
        d = std::move(other.d);
    }
    NetworkInterface &operator=(const NetworkInterface &other);
    NetworkInterface &operator=(NetworkInterface &&other) noexcept
    {
        swap(other);
        return *this;
    }
    ~NetworkInterface();
    void swap(NetworkInterface &other) noexcept { std::swap(d, other.d); }
public:
    bool isValid() const;
    int index() const;
    int maximumTransmissionUnit() const;
    std::string name() const;
    std::string humanReadableName() const;
    InterfaceFlags flags() const;
    InterfaceType type() const;
    std::string hardwareAddress() const;
    std::vector<NetworkAddressEntry> addressEntries() const;

    static int interfaceIndexFromName(const std::string &name);
    static NetworkInterface interfaceFromName(const std::string &name);
    static NetworkInterface interfaceFromIndex(int index);
    static std::string interfaceNameFromIndex(int index);
    static std::vector<NetworkInterface> allInterfaces();
    static std::vector<HostAddress> allAddresses();
private:
    friend class NetworkInterfacePrivate;
    std::shared_ptr<NetworkInterfacePrivate> d;
};


}  // namespace qtng

#ifndef NG_NO_DEBUG_STREAM
std::ostream &operator<<(std::ostream &debug, const qtng::NetworkAddressEntry &entry);
std::ostream &operator<<(std::ostream &debug, const qtng::NetworkInterface &networkInterface);
#endif

#endif  // QTNG_NETWORK_INTERFACE_H
