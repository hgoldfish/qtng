#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "qtng/network_interface.h"
#include "qtng/private/hostaddress_p.h"
#include "qtng/private/network_interface_p.h"

using namespace std;

// In case these aren't defined
#define IF_TYPE_IEEE80216_WMAN  237
#define IF_TYPE_IEEE802154      259

namespace qtng {

typedef DWORD (WINAPI *PtrGetAdaptersInfo)(PIP_ADAPTER_INFO, PULONG);
static PtrGetAdaptersInfo ptrGetAdaptersInfo = 0;
typedef ULONG (WINAPI *PtrGetAdaptersAddresses)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
static PtrGetAdaptersAddresses ptrGetAdaptersAddresses = 0;
typedef DWORD (WINAPI *PtrGetNetworkParams)(PFIXED_INFO, PULONG);
static PtrGetNetworkParams ptrGetNetworkParams = 0;

typedef NETIO_STATUS (WINAPI *PtrConvertInterfaceLuidToName)(const NET_LUID *, PWSTR, SIZE_T);
static PtrConvertInterfaceLuidToName ptrConvertInterfaceLuidToName = 0;
typedef NETIO_STATUS (WINAPI *PtrConvertInterfaceLuidToIndex)(const NET_LUID *, PNET_IFINDEX);
static PtrConvertInterfaceLuidToIndex ptrConvertInterfaceLuidToIndex = 0;
typedef NETIO_STATUS (WINAPI *PtrConvertInterfaceNameToLuid)(const wchar_t*, NET_LUID *);
static PtrConvertInterfaceNameToLuid ptrConvertInterfaceNameToLuid = 0;
typedef NETIO_STATUS (WINAPI *PtrConvertInterfaceIndexToLuid)(NET_IFINDEX, PNET_LUID);
static PtrConvertInterfaceIndexToLuid ptrConvertInterfaceIndexToLuid = 0;

static void resolveLibs()
{
    // try to find the functions we need from Iphlpapi.dll
    static bool done = false;

    if (!done) {
        done = true;

        void /*lib*/ lib("iphlpapi");
        if (!lib.load()) {
            return;
        }
        ptrGetAdaptersInfo = (PtrGetAdaptersInfo) lib.resolve("GetAdaptersInfo");
        ptrGetAdaptersAddresses = (PtrGetAdaptersAddresses) lib.resolve("GetAdaptersAddresses");
        ptrGetNetworkParams = (PtrGetNetworkParams) lib.resolve("GetNetworkParams");
        ptrConvertInterfaceLuidToName = (PtrConvertInterfaceLuidToName) lib.resolve("ConvertInterfaceLuidToNameW");
        ptrConvertInterfaceLuidToIndex = (PtrConvertInterfaceLuidToIndex) lib.resolve("ConvertInterfaceLuidToIndex");
        ptrConvertInterfaceNameToLuid = (PtrConvertInterfaceNameToLuid) lib.resolve("ConvertInterfaceNameToLuidW");
        ptrConvertInterfaceIndexToLuid = (PtrConvertInterfaceIndexToLuid) lib.resolve("ConvertInterfaceIndexToLuid");
    }
}


static unordered_map<HostAddress, HostAddress> ipv4Netmasks()
{
    //Retrieve all the IPV4 addresses & netmasks
    IP_ADAPTER_INFO staticBuf[2]; // 2 is arbitrary
    PIP_ADAPTER_INFO pAdapter = staticBuf;
    ULONG bufSize = sizeof staticBuf;
    unordered_map<HostAddress, HostAddress> ipv4netmasks;

    DWORD retval = ptrGetAdaptersInfo(pAdapter, &bufSize);
    if (retval == ERROR_BUFFER_OVERFLOW) {
        // need more memory
        pAdapter = (IP_ADAPTER_INFO *)malloc(bufSize);
        if (!pAdapter)
            return ipv4netmasks;
        // try again
        if (ptrGetAdaptersInfo(pAdapter, &bufSize) != ERROR_SUCCESS) {
            free(pAdapter);
            return ipv4netmasks;
        }
    } else if (retval != ERROR_SUCCESS) {
        // error
        return ipv4netmasks;
    }

    // iterate over the list and add the entries to our listing
    for (PIP_ADAPTER_INFO ptr = pAdapter; ptr; ptr = ptr->Next) {
        for (PIP_ADDR_STRING addr = &ptr->IpAddressList; addr; addr = addr->Next) {
            HostAddress address(addr->IpAddress.String);
            HostAddress mask(addr->IpMask.String);
            ipv4netmasks[address] = mask;
        }
    }
    if (pAdapter != staticBuf)
        free(pAdapter);

    return ipv4netmasks;
}


static vector<NetworkInterfacePrivate *> interfaceListingWinXP()
{
    vector<NetworkInterfacePrivate *> interfaces;
    IP_ADAPTER_ADDRESSES staticBuf[2]; // 2 is arbitrary
    PIP_ADAPTER_ADDRESSES pAdapter = staticBuf;
    ULONG bufSize = sizeof staticBuf;

    const unordered_map<HostAddress, HostAddress> &ipv4netmasks = ipv4Netmasks();
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX |
                  GAA_FLAG_SKIP_DNS_SERVER |
                  GAA_FLAG_SKIP_MULTICAST;
    ULONG retval = ptrGetAdaptersAddresses(AF_UNSPEC, flags, NULL, pAdapter, &bufSize);
    if (retval == ERROR_BUFFER_OVERFLOW) {
        // need more memory
        pAdapter = (IP_ADAPTER_ADDRESSES *)malloc(bufSize);
        if (!pAdapter)
            return interfaces;
        // try again
        if (ptrGetAdaptersAddresses(AF_UNSPEC, flags, NULL, pAdapter, &bufSize) != ERROR_SUCCESS) {
            free(pAdapter);
            return interfaces;
        }
    } else if (retval != ERROR_SUCCESS) {
        // error
        return interfaces;
    }

    // iterate over the list and add the entries to our listing
    for (PIP_ADAPTER_ADDRESSES ptr = pAdapter; ptr; ptr = ptr->Next) {
        NetworkInterfacePrivate *iface = new NetworkInterfacePrivate;
        interfaces.push_back(iface);

        iface->index = 0;
        if (ptr->Length >= offsetof(IP_ADAPTER_ADDRESSES, Ipv6IfIndex) && ptr->Ipv6IfIndex != 0)
            iface->index = ptr->Ipv6IfIndex;
        else if (ptr->IfIndex != 0)
            iface->index = ptr->IfIndex;

        iface->flags = NetworkInterface::CanBroadcast;
        if (ptr->OperStatus == IfOperStatusUp)
            iface->flags |= NetworkInterface::IsUp | NetworkInterface::IsRunning;
        if ((ptr->int & IP_ADAPTER_NO_MULTICAST) == 0)
            iface->flags |= NetworkInterface::CanMulticast;
        if (ptr->IfType == IF_TYPE_PPP)
            iface->flags |= NetworkInterface::IsPointToPoint;

        iface->name = string::fromLocal8Bit(ptr->AdapterName);
        iface->friendlyName = string::fromWCharArray(ptr->FriendlyName);
        if (ptr->PhysicalAddressLength)
            iface->hardwareAddress = iface->makeHwAddress(ptr->PhysicalAddressLength,
                                                          ptr->PhysicalAddress);
        else
            // loopback if it has no address
            iface->flags |= NetworkInterface::IsLoopBack;

        // The GetAdaptersAddresses call has an interesting semantic:
        // It can return a number N of addresses and a number M of prefixes.
        // But if you have IPv6 addresses, generally N > M.
        // I cannot find a way to relate the Address to the Prefix, aside from stopping
        // the iteration at the last Prefix entry and assume that it applies to all addresses
        // from that point on.
        PIP_ADAPTER_PREFIX pprefix = 0;
        if (ptr->Length >= offsetof(IP_ADAPTER_ADDRESSES, FirstPrefix))
            pprefix = ptr->FirstPrefix;
        for (PIP_ADAPTER_UNICAST_ADDRESS addr = ptr->FirstUnicastAddress; addr; addr = addr->Next) {
            NetworkAddressEntry entry;
            entry.setIp(HostAddress(addr->Address.lpSockaddr));
            if (pprefix) {
                if (entry.ip().protocol() == HostAddress::IPv4Protocol) {
                    entry.setNetmask(ipv4netmasks[entry.ip()]);

                    // broadcast address is set on postProcess()
                } else { //IPV6
                    entry.setPrefixLength(pprefix->PrefixLength);
                }
                pprefix = pprefix->Next ? pprefix->Next : pprefix;
            }
            iface->addressEntries << entry;
        }
    }

    if (pAdapter != staticBuf)
        free(pAdapter);

    return interfaces;
}


static vector<NetworkInterfacePrivate *> interfaceListingWin2k()
{
    vector<NetworkInterfacePrivate *> interfaces;
    IP_ADAPTER_INFO staticBuf[2]; // 2 is arbitrary
    PIP_ADAPTER_INFO pAdapter = staticBuf;
    ULONG bufSize = sizeof staticBuf;

    DWORD retval = ptrGetAdaptersInfo(pAdapter, &bufSize);
    if (retval == ERROR_BUFFER_OVERFLOW) {
        // need more memory
        pAdapter = (IP_ADAPTER_INFO *)malloc(bufSize);
        if (!pAdapter)
            return interfaces;
        // try again
        if (ptrGetAdaptersInfo(pAdapter, &bufSize) != ERROR_SUCCESS) {
            free(pAdapter);
            return interfaces;
        }
    } else if (retval != ERROR_SUCCESS) {
        // error
        return interfaces;
    }

    // iterate over the list and add the entries to our listing
    for (PIP_ADAPTER_INFO ptr = pAdapter; ptr; ptr = ptr->Next) {
        NetworkInterfacePrivate *iface = new NetworkInterfacePrivate;
        interfaces.push_back(iface);

        iface->index = ptr->Index;
        iface->flags = NetworkInterface::IsUp | NetworkInterface::IsRunning;
        if (ptr->Type == MIB_IF_TYPE_PPP)
            iface->flags |= NetworkInterface::IsPointToPoint;
        else
            iface->flags |= NetworkInterface::CanBroadcast;
        iface->name = string::fromLocal8Bit(ptr->AdapterName);
        iface->hardwareAddress = NetworkInterfacePrivate::makeHwAddress(ptr->AddressLength,
                                                                         ptr->Address);

        for (PIP_ADDR_STRING addr = &ptr->IpAddressList; addr; addr = addr->Next) {
            NetworkAddressEntry entry;
            entry.setIp(HostAddress(addr->IpAddress.String));
            entry.setNetmask(HostAddress(addr->IpMask.String));
            // broadcast address is set on postProcess()

            iface->addressEntries << entry;
        }
    }

    if (pAdapter != staticBuf)
        free(pAdapter);

    return interfaces;
}

static vector<NetworkInterfacePrivate *> interfaceListingVista()
{
    vector<NetworkInterfacePrivate *> interfaces;
    IP_ADAPTER_ADDRESSES staticBuf[2]; // 2 is arbitrary
    PIP_ADAPTER_ADDRESSES pAdapter = staticBuf;
    ULONG bufSize = sizeof staticBuf;

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX |
                  GAA_FLAG_SKIP_DNS_SERVER |
                  GAA_FLAG_SKIP_MULTICAST;
    ULONG retval = ptrGetAdaptersAddresses(AF_UNSPEC, flags, NULL, pAdapter, &bufSize);
    if (retval == ERROR_BUFFER_OVERFLOW) {
        // need more memory
        pAdapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(malloc(bufSize));
        if (!pAdapter)
            return interfaces;
        // try again
        if (ptrGetAdaptersAddresses(AF_UNSPEC, flags, NULL, pAdapter, &bufSize) != ERROR_SUCCESS) {
            free(pAdapter);
            return interfaces;
        }
    } else if (retval != ERROR_SUCCESS) {
        // error
        return interfaces;
    }

    // iterate over the list and add the entries to our listing
    for (PIP_ADAPTER_ADDRESSES ptr = pAdapter; ptr; ptr = ptr->Next) {
        // the structure grows over time, so let's make sure the fields
        // introduced in Windows Vista are present (Luid is the furthest
        // field we access from IP_ADAPTER_ADDRESSES_LH)
        assert(ptr->Length >= offsetof(IP_ADAPTER_ADDRESSES, Luid));
        assert(ptr->Length >= offsetof(IP_ADAPTER_ADDRESSES, Ipv6IfIndex));

        NetworkInterfacePrivate *iface = new NetworkInterfacePrivate();
        interfaces.push_back(iface);

        iface->index = 0;
        if (ptr->Ipv6IfIndex != 0)
            iface->index = ptr->Ipv6IfIndex;
        else if (ptr->IfIndex != 0)
            iface->index = ptr->IfIndex;

        iface->mtu = min<int64_t>(ptr->Mtu, INT_MAX);
        iface->flags = NetworkInterface::CanBroadcast;
        if (ptr->OperStatus == IfOperStatusUp)
            iface->flags |= NetworkInterface::IsUp | NetworkInterface::IsRunning;
        if ((ptr->int & IP_ADAPTER_NO_MULTICAST) == 0)
            iface->flags |= NetworkInterface::CanMulticast;
        if (ptr->IfType == IF_TYPE_PPP)
            iface->flags |= NetworkInterface::IsPointToPoint;

        switch (ptr->IfType) {
        case IF_TYPE_ETHERNET_CSMACD:
            iface->type = NetworkInterface::Ethernet;
            break;

        case IF_TYPE_FDDI:
            iface->type = NetworkInterface::Fddi;
            break;

        case IF_TYPE_PPP:
            iface->type = NetworkInterface::Ppp;
            break;

        case IF_TYPE_SLIP:
            iface->type = NetworkInterface::Slip;
            break;

        case IF_TYPE_SOFTWARE_LOOPBACK:
            iface->type = NetworkInterface::Loopback;
            iface->flags |= NetworkInterface::IsLoopBack;
            break;

        case IF_TYPE_IEEE80211:
            iface->type = NetworkInterface::Ieee80211;
            break;

        case IF_TYPE_IEEE1394:
            iface->type = NetworkInterface::Ieee1394;
            break;

        case IF_TYPE_IEEE80216_WMAN:
            iface->type = NetworkInterface::Ieee80216;
            break;

        case IF_TYPE_IEEE802154:
            iface->type = NetworkInterface::Ieee802154;
            break;
        }

        // use ConvertInterfaceLuidToNameW because that returns a friendlier name, though not
        // as "friendly" as FriendlyName below
        WCHAR buf[IF_MAX_STRING_SIZE + 1];
        if (ptrConvertInterfaceLuidToName(&ptr->Luid, buf, sizeof(buf)/sizeof(buf[0])) == NO_ERROR)
            iface->name = string::fromWCharArray(buf);
        if (iface->name.empty())
            iface->name = string::fromLocal8Bit(ptr->AdapterName);

        iface->friendlyName = string::fromWCharArray(ptr->FriendlyName);
        if (ptr->PhysicalAddressLength)
            iface->hardwareAddress = iface->makeHwAddress(ptr->PhysicalAddressLength,
                                                          ptr->PhysicalAddress);

        // parse the IP (unicast) addresses
        for (PIP_ADAPTER_UNICAST_ADDRESS addr = ptr->FirstUnicastAddress; addr; addr = addr->Next) {
            assert(addr->Length >= offsetof(IP_ADAPTER_UNICAST_ADDRESS, OnLinkPrefixLength));

            // skip addresses in invalid state
            if (addr->DadState == IpDadStateInvalid)
                continue;

            NetworkAddressEntry entry;
            entry.setIp(HostAddress(addr->Address.lpSockaddr));
            entry.setPrefixLength(addr->OnLinkPrefixLength);
            iface->addressEntries << entry;
        }
    }

    if (pAdapter != staticBuf)
        free(pAdapter);

    return interfaces;
}


static vector<NetworkInterfacePrivate *> interfaceListing()
{
    resolveLibs();
    if (ptrConvertInterfaceLuidToName && ptrGetAdaptersAddresses) {
        return interfaceListingVista();
    } else if (ptrGetAdaptersAddresses) {
        return interfaceListingWinXP();
    } else if (ptrGetAdaptersInfo) {
        return interfaceListingWin2k();
    }
    // failed
    return vector<NetworkInterfacePrivate *>();
}


uint NetworkInterfaceManager::interfaceIndexFromName(const string &name)
{
    resolveLibs();
    if (ptrConvertInterfaceNameToLuid && ptrConvertInterfaceLuidToIndex) {
        NET_IFINDEX id;
        NET_LUID luid;
        if (ptrConvertInterfaceNameToLuid(reinterpret_cast<const wchar_t *>(name.data()), &luid) == NO_ERROR
                && ptrConvertInterfaceLuidToIndex(&luid, &id) == NO_ERROR)
            return uint(id);
    }
    return 0;
}


string NetworkInterfaceManager::interfaceNameFromIndex(uint index)
{
    resolveLibs();
    if (ptrConvertInterfaceLuidToName && ptrConvertInterfaceIndexToLuid) {
        NET_LUID luid;
        if (ptrConvertInterfaceIndexToLuid(index, &luid) == NO_ERROR) {
            WCHAR buf[IF_MAX_STRING_SIZE + 1];
            if (ptrConvertInterfaceLuidToName(&luid, buf, sizeof(buf)/sizeof(buf[0])) == NO_ERROR)
                return string::fromWCharArray(buf);
        }
    }
    return string::number(index);
}


vector<NetworkInterfacePrivate *> NetworkInterfaceManager::scan()
{
    return interfaceListing();
}


}  // namespace qtng
