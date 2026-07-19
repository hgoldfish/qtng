#ifndef QTNG_NETWORK_INTERFACE_P_H
#define QTNG_NETWORK_INTERFACE_P_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qtng/network_interface.h"
#include "qtng/private/hostaddress_p.h"

namespace qtng {

class NetworkAddressEntryPrivate
{
public:
    HostAddress address;
    HostAddress broadcast;
    Netmask netmask;
    std::int64_t preferredLifetimeMs = -1;
    std::int64_t validityLifetimeMs = -1;
    bool lifetimeKnown = false;
    NetworkAddressEntry::DnsEligibilityStatus dnsEligibility = NetworkAddressEntry::DnsEligibilityUnknown;
};

class NetworkInterfacePrivate 
{
public:
    NetworkInterfacePrivate()
        : index(0)
        , mtu(0)
        , flags()
    {
    }
public:
    int index;  // interface index, if know
    int mtu;
    NetworkInterface::InterfaceFlags flags;
    NetworkInterface::InterfaceType type = NetworkInterface::Unknown;
    std::string name;
    std::string friendlyName;
    std::string hardwareAddress;
    std::vector<NetworkAddressEntry> addressEntries;
public:
    static std::string makeHwAddress(int len, unsigned char *data);
    static void calculateDnsEligibility(NetworkAddressEntry *entry, bool isTemporary, bool isDeprecated)
    {
        // this implements an algorithm that yields the same results as Windows
        // produces, for the same input (as far as I can test)
        if (isTemporary || isDeprecated)
            entry->setDnsEligibility(NetworkAddressEntry::DnsIneligible);

        AddressClassification cl = HostAddressPrivate::classify(entry->ip());
        if (cl == LoopbackAddress || cl == LinkLocalAddress)
            entry->setDnsEligibility(NetworkAddressEntry::DnsIneligible);
        else
            entry->setDnsEligibility(NetworkAddressEntry::DnsEligible);
    }
private:
    // disallow copying -- avoid detaching
    NetworkInterfacePrivate &operator=(const NetworkInterfacePrivate &other);
    NetworkInterfacePrivate(const NetworkInterfacePrivate &other);
};

class NetworkInterfaceManager
{
public:
    NetworkInterfaceManager();
    ~NetworkInterfaceManager();

    std::shared_ptr<NetworkInterfacePrivate> interfaceFromName(const std::string &name);
    std::shared_ptr<NetworkInterfacePrivate> interfaceFromIndex(int index);
    std::vector<std::shared_ptr<NetworkInterfacePrivate>> allInterfaces();

    static uint interfaceIndexFromName(const std::string &name);
    static std::string interfaceNameFromIndex(uint index);

    // convenience:
    std::shared_ptr<NetworkInterfacePrivate> empty;
private:
    std::vector<NetworkInterfacePrivate *> scan();
};

}  // namespace qtng

#endif
