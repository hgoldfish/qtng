#include "qtng/utils/platform.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#ifdef NG_OS_ANDROID
// android lacks if_nameindex
#  define QTNG_NO_IPV6IFNAME
#  define QTNG_NO_GETIFADDRS
#endif
#ifndef QT_NO_GETIFADDRS
#  include <ifaddrs.h>
#endif

#include "qtng/network_interface.h"
#include "qtng/private/network_interface_p.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/string_utils.h"
#include "network_interface_unix_p.h"

using namespace std;

NG_LOGGER("qtng.network_interface")

namespace qtng {

static HostAddress addressFromSockaddr(sockaddr *sa, int ifindex = 0, const string &ifname = string())
{
    HostAddress address;
    if (!sa)
        return address;

    if (sa->sa_family == AF_INET)
        address.setAddress(htonl(((sockaddr_in *) sa)->sin_addr.s_addr));
    else if (sa->sa_family == AF_INET6) {
        address.setAddress(((sockaddr_in6 *) sa)->sin6_addr.s6_addr);
        int scope = ((sockaddr_in6 *) sa)->sin6_scope_id;
        if (scope && scope == ifindex) {
            // this is the most likely scenario:
            // a scope ID in a socket is that of the interface this address came from
            address.setScopeId(ifname);
        } else if (scope) {
            address.setScopeId(NetworkInterfaceManager::interfaceNameFromIndex(scope));
        }
    }
    return address;
}

uint NetworkInterfaceManager::interfaceIndexFromName(const string &name)
{
#ifndef QTNG_NO_IPV6IFNAME
    return ::if_nametoindex(name.c_str());
#elif defined(SIOCGIFINDEX)
    struct ifreq req;
    int socket = qt_safe_socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0)
        return 0;

    string name8bit = name;
    memset(&req, 0, sizeof(ifreq));
    memcpy(req.ifr_name, name8bit, min<int>(name8bit.size() + 1, sizeof(req.ifr_name) - 1));

    uint id = 0;
    if (qt_safe_ioctl(socket, SIOCGIFINDEX, &req) >= 0)
        id = req.ifr_ifindex;
    qt_safe_close(socket);
    return id;
#else
    return 0;
#endif
}

string NetworkInterfaceManager::interfaceNameFromIndex(uint index)
{
#ifndef QTNG_NO_IPV6IFNAME
    char buf[IF_NAMESIZE];
    if (::if_indextoname(index, buf))
        return buf;
#elif defined(SIOCGIFNAME)
    struct ifreq req;
    int socket = qt_safe_socket(AF_INET, SOCK_STREAM, 0);
    if (socket >= 0) {
        memset(&req, 0, sizeof(ifreq));
        req.ifr_ifindex = index;

        if (qt_safe_ioctl(socket, SIOCGIFNAME, &req) >= 0) {
            qt_safe_close(socket);
            return req.ifr_name;
        }
        qt_safe_close(socket);
    }
#endif
    return to_string(index);
}

static int getMtu(int socket, struct ifreq *req)
{
#ifdef SIOCGIFMTU
    if (qt_safe_ioctl(socket, SIOCGIFMTU, req) == 0)
        return req->ifr_mtu;
#endif
    return 0;
}

#ifdef QTNG_NO_GETIFADDRS
// getifaddrs not available

static unordered_set<string> interfaceNames(int socket)
{
    unordered_set<string> result;
#  ifdef QTNG_NO_IPV6IFNAME
    string storageBuffer;
    struct ifconf interfaceList;
    static const int STORAGEBUFFER_GROWTH = 256;

    forever {
        // grow the storage buffer
        storageBuffer.resize(storageBuffer.size() + STORAGEBUFFER_GROWTH);
        interfaceList.ifc_buf = storageBuffer.data();
        interfaceList.ifc_len = storageBuffer.size();

        // get the interface list
        if (qt_safe_ioctl(socket, SIOCGIFCONF, &interfaceList) >= 0) {
            if (int(interfaceList.ifc_len + sizeof(ifreq) + 64) < storageBuffer.size()) {
                // if the buffer was big enough, break
                storageBuffer.resize(interfaceList.ifc_len);
                break;
            }
        } else {
            // internal error
            return result;
        }
        if (storageBuffer.size() > 100000) {
            // out of space
            return result;
        }
    }

    int interfaceCount = interfaceList.ifc_len / sizeof(ifreq);
    for (int i = 0; i < interfaceCount; ++i) {
        string name = string(interfaceList.ifc_req[i].ifr_name);
        if (!name.empty())
            result.push_back(name);
    }

    return result;
#  else
    (void)(socket);

    // use if_nameindex
    struct if_nameindex *interfaceList = ::if_nameindex();
    for (struct if_nameindex *ptr = interfaceList; ptr && ptr->if_name; ++ptr)
        result.push_back(ptr->if_name);

    if_freenameindex(interfaceList);
    return result;
#  endif
}

static NetworkInterfacePrivate *findInterface(int socket, vector<NetworkInterfacePrivate *> &interfaces,
                                              struct ifreq &req)
{
    NetworkInterfacePrivate *iface = nullptr;
    int ifindex = 0;

#  if !defined(QTNG_NO_IPV6IFNAME) || defined(SIOCGIFINDEX)
    // Get the interface index
#    ifdef SIOCGIFINDEX
    if (qt_safe_ioctl(socket, SIOCGIFINDEX, &req) >= 0) {
        ifindex = req.ifr_ifindex;
    }
#    else
    ifindex = if_nametoindex(req.ifr_name);
#    endif

    // find the interface data
    vector<NetworkInterfacePrivate *>::iterator if_it = interfaces.begin();
    for (; if_it != interfaces.end(); ++if_it)
        if ((*if_it)->index == ifindex) {
            // existing interface
            iface = *if_it;
            break;
        }
#  else
    // Search by name
    vector<NetworkInterfacePrivate *>::iterator if_it = interfaces.begin();
    for (; if_it != interfaces.end(); ++if_it)
        if ((*if_it)->name == req.ifr_name) {
            // existing interface
            iface = *if_it;
            break;
        }
#  endif

    if (!iface) {
        // new interface, create data:
        iface = new NetworkInterfacePrivate;
        iface->index = ifindex;
        interfaces.push_back(iface);
    }

    return iface;
}

static vector<NetworkInterfacePrivate *> interfaceListing()
{
    vector<NetworkInterfacePrivate *> interfaces;

    int socket;
    if ((socket = qt_safe_socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) == -1)
        return interfaces;  // error

    unordered_set<string> names = interfaceNames(socket);
    for (unordered_set<string>::const_iterator it = names.begin(); it != names.end(); ++it) {
        ifreq req;
        memset(&req, 0, sizeof(ifreq));
        memcpy(req.ifr_name, *it, min<int>(it->length() + 1, sizeof(req.ifr_name) - 1));

        NetworkInterfacePrivate *iface = findInterface(socket, interfaces, req);

#  ifdef SIOCGIFNAME
        // Get the canonical name
        string oldName = req.ifr_name;
        if (qt_safe_ioctl(socket, SIOCGIFNAME, &req) >= 0) {
            iface->name = req.ifr_name;

            // reset the name:
            memcpy(req.ifr_name, oldName, min<int>(oldName.size() + 1, sizeof(req.ifr_name) - 1));
        } else
#  endif
        {
            // use this name anyways
            iface->name = req.ifr_name;
        }

        // Get interface flags
        if (qt_safe_ioctl(socket, SIOCGIFFLAGS, &req) >= 0) {
            iface->flags = convertint(req.ifr_flags);
        }
        iface->mtu = getMtu(socket, &req);

#  ifdef SIOCGIFHWADDR
        // Get the HW address
        if (qt_safe_ioctl(socket, SIOCGIFHWADDR, &req) >= 0) {
            uint8_t *addr = (uint8_t *) req.ifr_addr.sa_data;
            iface->hardwareAddress = iface->makeHwAddress(6, addr);
        }
#  endif

        // Get the address of the interface
        NetworkAddressEntry entry;
        if (qt_safe_ioctl(socket, SIOCGIFADDR, &req) >= 0) {
            sockaddr *sa = &req.ifr_addr;
            entry.setIp(addressFromSockaddr(sa));

            // Get the interface broadcast address
            if (iface->flags & NetworkInterface::CanBroadcast) {
                if (qt_safe_ioctl(socket, SIOCGIFBRDADDR, &req) >= 0) {
                    sockaddr *sa = &req.ifr_addr;
                    if (sa->sa_family == AF_INET)
                        entry.setBroadcast(addressFromSockaddr(sa));
                }
            }

            // Get the interface netmask
            if (qt_safe_ioctl(socket, SIOCGIFNETMASK, &req) >= 0) {
                sockaddr *sa = &req.ifr_addr;
                entry.setNetmask(addressFromSockaddr(sa));
            }

            iface->addressEntries.push_back(entry);
        }
    }

    ::close(socket);
    return interfaces;
}

#else
// platform-specific defs:
#  ifdef NG_OS_LINUX
#    include <features.h>
#  endif

#  if defined(NG_OS_LINUX) && __GLIBC__ - 0 >= 2 && __GLIBC_MINOR__ - 0 >= 1
#    include <netpacket/packet.h>

static vector<NetworkInterfacePrivate *> createInterfaces(ifaddrs *rawList)
{
    vector<NetworkInterfacePrivate *> interfaces;
    unordered_set<string> seenInterfaces;
    unordered_set<int> seenIndexes;

    // On Linux, glibc, uClibc and MUSL obtain the address listing via two
    // netlink calls: first an RTM_GETLINK to obtain the interface listing,
    // then one RTM_GETADDR to get all the addresses (uClibc implementation is
    // copied from glibc; Bionic currently doesn't support getifaddrs). They
    // synthesize AF_PACKET addresses from the RTM_GETLINK responses, which
    // means by construction they currently show up first in the interface
    // listing.
    for (ifaddrs *ptr = rawList; ptr; ptr = ptr->ifa_next) {
        if (ptr->ifa_addr && ptr->ifa_addr->sa_family == AF_PACKET) {
            sockaddr_ll *sll = (sockaddr_ll *) ptr->ifa_addr;
            NetworkInterfacePrivate *iface = new NetworkInterfacePrivate;
            interfaces.push_back(iface);
            iface->index = sll->sll_ifindex;
            iface->name = ptr->ifa_name;
            iface->flags = convertint(ptr->ifa_flags);
            iface->hardwareAddress = iface->makeHwAddress(sll->sll_halen, (uint8_t *) sll->sll_addr);

            assert(seenIndexes.find(iface->index) == seenIndexes.end());
            seenIndexes.insert(iface->index);
            seenInterfaces.insert(iface->name);
        }
    }

    // see if we missed anything:
    // - virtual interfaces with no HW address have no AF_PACKET
    // - interface labels have no AF_PACKET, but shouldn't be shown as a new interface
    for (ifaddrs *ptr = rawList; ptr; ptr = ptr->ifa_next) {
        if (!ptr->ifa_addr || ptr->ifa_addr->sa_family != AF_PACKET) {
            string name = ptr->ifa_name;
            if (seenInterfaces.find(name) != seenInterfaces.end())
                continue;

            int ifindex = if_nametoindex(ptr->ifa_name);
            if (seenIndexes.find(ifindex) != seenIndexes.end())
                continue;

            seenInterfaces.insert(name);
            seenIndexes.insert(ifindex);

            NetworkInterfacePrivate *iface = new NetworkInterfacePrivate;
            interfaces.push_back(iface);
            iface->name = name;
            iface->flags = convertint(ptr->ifa_flags);
            iface->index = ifindex;
        }
    }

    return interfaces;
}

static void getAddressExtraInfo(NetworkAddressEntry *entry, struct sockaddr *sa, const char *ifname)
{
    (void)(entry);
    (void)(sa);
    (void)(ifname);
}

#  elif defined(NG_OS_FREEBSD)
#    include <net/if_dl.h>
#    if defined(QT_PLATFORM_UIKIT)
#      include "NetworkInterface_uikit_p.h"
#      if !defined(QT_WATCHOS_OUTDATED_SDK_WORKAROUND)
// TODO: remove it as soon as SDK is updated on CI!!!
#        include <net/if_types.h>
#      endif
#    else
#      include <net/if_media.h>
#      include <net/if_types.h>
#      include <netinet/in_var.h>
#      ifdef NG_OS_OPENBSD
#        include <netinet6/in6_var.h>
#      endif
#    endif  // QT_PLATFORM_UIKIT

static int openSocket(int &socket)
{
    if (socket == -1)
        socket = qt_safe_socket(AF_INET, SOCK_DGRAM, 0);
    return socket;
}

static NetworkInterface::InterfaceType probeIfType(int socket, int iftype, struct ifmediareq *req)
{
    // Determine the interface type.

    // On Darwin, these are #defines, but on FreeBSD they're just an
    // enum, so we can't #ifdef them. Use the authoritative list from
    // https://www.iana.org/assignments/smi-numbers/smi-numbers.xhtml#smi-numbers-5
    switch (iftype) {
    case IFT_PPP:
        return NetworkInterface::Ppp;

    case IFT_LOOP:
        return NetworkInterface::Loopback;

    case IFT_SLIP:
        return NetworkInterface::Slip;

    case 0x47:  // IFT_IEEE80211
        return NetworkInterface::Ieee80211;

    case IFT_IEEE1394:
        return NetworkInterface::Ieee1394;
#    ifndef IFT_GIF
#      define IFT_GIF 0xf0
#    endif
    case IFT_GIF:
#    ifndef IFT_STF
#      define IFT_STF 0xd7
#    endif
    case IFT_STF:
        return NetworkInterface::Virtual;
    }

    // For the remainder (including Ethernet), let's try SIOGIFMEDIA
    req->ifm_count = 0;
    if (qt_safe_ioctl(socket, SIOCGIFMEDIA, req) == 0) {
        // see https://man.openbsd.org/ifmedia.4

        switch (IFM_TYPE(req->ifm_current)) {
        case IFM_ETHER:
            return NetworkInterface::Ethernet;

#    ifdef IFM_FDDI
        case IFM_FDDI:
            return NetworkInterface::Fddi;
#    endif

        case IFM_IEEE80211:
            return NetworkInterface::Ieee80211;
        }
    }

    return NetworkInterface::Unknown;
}

static vector<NetworkInterfacePrivate *> createInterfaces(ifaddrs *rawList)
{
    vector<NetworkInterfacePrivate *> interfaces;
    union {
        struct ifmediareq mediareq;
        struct ifreq req;
    };
    int socket = -1;

    // ensure both structs start with the name field, of size IFNAMESIZ
    static_assert(sizeof(mediareq.ifm_name) == sizeof(req.ifr_name));
    assert(&mediareq.ifm_name == &req.ifr_name);

    // on NetBSD we use AF_LINK and sockaddr_dl
    // scan the list for that family
    for (ifaddrs *ptr = rawList; ptr; ptr = ptr->ifa_next)
        if (ptr->ifa_addr && ptr->ifa_addr->sa_family == AF_LINK) {
            NetworkInterfacePrivate *iface = new NetworkInterfacePrivate;
            interfaces.push_back(iface);

            sockaddr_dl *sdl = (sockaddr_dl *) ptr->ifa_addr;
            iface->index = sdl->sdl_index;
            iface->name = ptr->ifa_name;
            iface->flags = convertint(ptr->ifa_flags);
            iface->hardwareAddress = iface->makeHwAddress(sdl->sdl_alen, (uint8_t *) LLADDR(sdl));

            qstrncpy(mediareq.ifm_name, ptr->ifa_name, sizeof(mediareq.ifm_name));
            iface->type = probeIfType(openSocket(socket), sdl->sdl_type, &mediareq);
            iface->mtu = getMtu(socket, &req);
        }

    if (socket != -1)
        qt_safe_close(socket);
    return interfaces;
}

static void getAddressExtraInfo(NetworkAddressEntry *entry, struct sockaddr *sa, const char *ifname)
{
    // get IPv6 address lifetimes
    if (sa->sa_family != AF_INET6)
        return;

    struct in6_ifreq ifr;

    int s6 = qt_safe_socket(AF_INET6, SOCK_DGRAM, 0);
    if ((s6 < 0)) {
        ngWarning() << "NetworkInterface: could not create IPv6 socket";
        return;
    }

    qstrncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

    // get flags
    ifr.ifr_addr = *reinterpret_cast<struct sockaddr_in6 *>(sa);
    if (qt_safe_ioctl(s6, SIOCGIFAFLAG_IN6, &ifr) < 0) {
        qt_safe_close(s6);
        return;
    }
    int flags = ifr.ifr_ifru.ifru_flags6;
    NetworkInterfacePrivate::calculateDnsEligibility(entry, flags & IN6_IFF_TEMPORARY, flags & IN6_IFF_DEPRECATED);

    ifr.ifr_addr = *reinterpret_cast<struct sockaddr_in6 *>(sa);
    if (qt_safe_ioctl(s6, SIOCGIFALIFETIME_IN6, &ifr) < 0) {
        qt_safe_close(s6);
        return;
    }
    qt_safe_close(s6);

    const auto toDeadline = [](time_t when) -> int64_t {
        return when ? static_cast<int64_t>(when) * 1000 : -1;
    };
    entry->setAddressLifetime(toDeadline(ifr.ifr_ifru.ifru_lifetime.ia6t_preferred),
                              toDeadline(ifr.ifr_ifr_ifru.ifru_lifetime.ia6t_expire));
}

#  else  // Generic version

static vector<NetworkInterfacePrivate *> createInterfaces(ifaddrs *rawList)
{
    (void)(getMtu);
    vector<NetworkInterfacePrivate *> interfaces;

    // make sure there's one entry for each interface
    for (ifaddrs *ptr = rawList; ptr; ptr = ptr->ifa_next) {
        // Get the interface index
        int ifindex = if_nametoindex(ptr->ifa_name);

        vector<NetworkInterfacePrivate *>::iterator if_it = interfaces.begin();
        for (; if_it != interfaces.end(); ++if_it)
            if ((*if_it)->index == ifindex)
                // this one has been added already
                break;

        if (if_it == interfaces.end()) {
            // none found, create
            NetworkInterfacePrivate *iface = new NetworkInterfacePrivate;
            interfaces.push_back(iface);

            iface->index = ifindex;
            iface->name = ptr->ifa_name;
            iface->flags = convertint(ptr->ifa_flags);
        }
    }

    return interfaces;
}

static void getAddressExtraInfo(NetworkAddressEntry *entry, struct sockaddr *sa, const char *ifname)
{
    (void)(entry);
    (void)(sa);
    (void)(ifname);
}
#  endif

static vector<NetworkInterfacePrivate *> interfaceListing()
{
    vector<NetworkInterfacePrivate *> interfaces;

    ifaddrs *interfaceListing;
    if (getifaddrs(&interfaceListing) == -1) {
        // error
        return interfaces;
    }

    interfaces = createInterfaces(interfaceListing);
    for (ifaddrs *ptr = interfaceListing; ptr; ptr = ptr->ifa_next) {
        // Find the interface
        const string name(ptr->ifa_name);
        NetworkInterfacePrivate *iface = 0;
        vector<NetworkInterfacePrivate *>::iterator if_it = interfaces.begin();
        for (; if_it != interfaces.end(); ++if_it)
            if ((*if_it)->name == name) {
                // found this interface already
                iface = *if_it;
                break;
            }

        if (!iface) {
            // it may be an interface label, search by interface index
            int ifindex = if_nametoindex(ptr->ifa_name);
            for (if_it = interfaces.begin(); if_it != interfaces.end(); ++if_it)
                if ((*if_it)->index == ifindex) {
                    // found this interface already
                    iface = *if_it;
                    break;
                }
        }

        if (!iface) {
            // skip all non-IP interfaces
            continue;
        }

        NetworkAddressEntry entry;
        entry.setIp(addressFromSockaddr(ptr->ifa_addr, iface->index, iface->name));
        if (entry.ip().isNull())
            // could not parse the address
            continue;

        entry.setNetmask(addressFromSockaddr(ptr->ifa_netmask, iface->index, iface->name));
        if (iface->flags & NetworkInterface::CanBroadcast)
            entry.setBroadcast(addressFromSockaddr(ptr->ifa_broadaddr, iface->index, iface->name));
        getAddressExtraInfo(&entry, ptr->ifa_addr, name.c_str());

        iface->addressEntries.push_back(entry);
    }

    freeifaddrs(interfaceListing);
    return interfaces;
}

#endif  // getifaddrs

vector<NetworkInterfacePrivate *> NetworkInterfaceManager::scan()
{
    return interfaceListing();
}

}  // namespace qtng
