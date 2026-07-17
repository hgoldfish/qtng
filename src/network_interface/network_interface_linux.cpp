using namespace std;

#include <cassert>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include "qtng/network_interface.h"
#include "qtng/utils/platform.h"
#include "qtng/utils/logging.h"
#include "qtng/private/eventloop_p.h"

// accordding to rtnetlink(7)
#include <asm/types.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/wireless.h>
#include <sys/socket.h>
#include "qtng/private/network_interface_p.h"
#include "network_interface_unix_p.h"

NG_LOGGER("qtng.network_interface")

/* in case these aren't defined in linux/if_arp.h (added since 2.6.28)  */
#define ARPHRD_PHONET       820     /* v2.6.29: PhoNet media type */
#define ARPHRD_PHONET_PIPE  821     /* v2.6.29: PhoNet pipe header */
#define ARPHRD_IEEE802154   804     /* v2.6.31 */
#define ARPHRD_6LOWPAN      825     /* v3.14: IPv6 over LoWPAN */

namespace qtng {

enum {
    BufferSize = 8192
};

static NetworkInterface::InterfaceType probeIfType(int socket, struct ifreq *req, short arptype)
{
    switch (ushort(arptype)) {
    case ARPHRD_LOOPBACK:
        return NetworkInterface::Loopback;

    case ARPHRD_ETHER:
        // check if it's a WiFi interface
        if (qt_safe_ioctl(socket, SIOCGIWMODE, req) >= 0)
            return NetworkInterface::Wifi;
        return NetworkInterface::Ethernet;

    case ARPHRD_SLIP:
    case ARPHRD_CSLIP:
    case ARPHRD_SLIP6:
    case ARPHRD_CSLIP6:
        return NetworkInterface::Slip;

    case ARPHRD_CAN:
        return NetworkInterface::CanBus;

    case ARPHRD_PPP:
        return NetworkInterface::Ppp;

    case ARPHRD_FDDI:
        return NetworkInterface::Fddi;

    case ARPHRD_IEEE80211:
    case ARPHRD_IEEE80211_PRISM:
    case ARPHRD_IEEE80211_RADIOTAP:
        return NetworkInterface::Ieee80211;

    case ARPHRD_IEEE802154:
        return NetworkInterface::Ieee802154;

    case ARPHRD_PHONET:
    case ARPHRD_PHONET_PIPE:
        return NetworkInterface::Phonet;

    case ARPHRD_6LOWPAN:
        return NetworkInterface::SixLoWPAN;

    case ARPHRD_TUNNEL:
    case ARPHRD_TUNNEL6:
    case ARPHRD_NONE:
    case ARPHRD_VOID:
        return NetworkInterface::Virtual;
    }
    return NetworkInterface::Unknown;
}


namespace {

template<typename Ret, typename Arg1, typename Arg2>
struct BinaryLambdaTraits {
    using FirstArgument = Arg1;
};

template<typename Lambda>
struct LambdaFirstArgument;

template<typename C, typename Ret, typename Arg1, typename Arg2>
struct LambdaFirstArgument<Ret (C::*)(Arg1, Arg2) const>
    : BinaryLambdaTraits<Ret, Arg1, Arg2>
{
};

template<typename Lambda>
struct LambdaFirstArgument : LambdaFirstArgument<decltype(&Lambda::operator())>
{
};

struct NetlinkSocket
{
    int sock;
    NetlinkSocket(int bufferSize)
    {
        sock = qt_safe_socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE, O_NONBLOCK);
        if (sock == -1) {
            ngWarning() << "Could not create AF_NETLINK socket";
        }

        // set buffer length
        socklen_t len = sizeof(bufferSize);
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufferSize, len);
    }

    ~NetlinkSocket()
    {
        if (sock != -1) {
            qt_safe_close(sock);
        }
    }

    operator int() const { return sock; }
};

template <typename Lambda> struct ProcessNetlinkRequest
{
    using FirstArgument = typename LambdaFirstArgument<Lambda>::FirstArgument;

    static int expectedTypeForRequest(int rtype)
    {
        static_assert(RTM_NEWADDR == RTM_GETADDR - 2);
        static_assert(RTM_NEWLINK == RTM_GETLINK - 2);
        assert(rtype == RTM_GETADDR || rtype == RTM_GETLINK);
        return rtype - 2;
    }

    void operator()(int sock, nlmsghdr *hdr, char *buf, size_t bufsize, Lambda &&func)
    {
        // send the request (non-blocking: yield on EAGAIN until the whole datagram is sent)
        for (;;) {
            ssize_t sent;
            do {
                sent = ::send(sock, hdr, hdr->nlmsg_len, 0);
            } while (sent < 0 && errno == EINTR);
            if (sent == ssize_t(hdr->nlmsg_len))
                break;
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ScopedIoWatcher watcher(EventLoopCoroutine::Write, sock);
                if (!watcher.start())
                    return;
                continue;
            }
            return;
        }

        // receive and parse the request
        int expectedType = expectedTypeForRequest(hdr->nlmsg_type);
        const bool isDump = hdr->nlmsg_flags & NLM_F_DUMP;
        for (;;) {
            ssize_t len;
            do {
                len = ::recv(sock, buf, bufsize, 0);
            } while (len < 0 && errno == EINTR);
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ScopedIoWatcher watcher(EventLoopCoroutine::Read, sock);
                if (!watcher.start())
                    return;
                continue;
            }
            hdr = reinterpret_cast<struct nlmsghdr *>(buf);
            if (!NLMSG_OK(hdr, uint32_t(len)))
                return;

            FirstArgument arg = reinterpret_cast<FirstArgument>(NLMSG_DATA(hdr));
            size_t payloadLen = NLMSG_PAYLOAD(hdr, 0);

            // is this a multipart message?
            assert(isDump == !!(hdr->nlmsg_flags & NLM_F_MULTI));
            if (!isDump) {
                // no, single message
                if (hdr->nlmsg_type == expectedType && payloadLen >= sizeof(FirstArgument))
                    return void(func(arg, payloadLen));
            } else {
                // multipart, parse until done
                do {
                    if (hdr->nlmsg_type == NLMSG_DONE)
                        return;
                    if (hdr->nlmsg_type != expectedType || payloadLen < sizeof(FirstArgument))
                        break;
                    func(arg, payloadLen);

                    // NLMSG_NEXT also updates the len variable
                    hdr = NLMSG_NEXT(hdr, len);
                    arg = reinterpret_cast<FirstArgument>(NLMSG_DATA(hdr));
                    payloadLen = NLMSG_PAYLOAD(hdr, 0);
                } while (NLMSG_OK(hdr, uint32_t(len)));

                if (len == 0)
                    continue;       // get new datagram
            }

#ifndef QT_NO_DEBUG
            if (NLMSG_OK(hdr, uint32_t(len)))
                ngWarning() << "NetworkInterface/AF_NETLINK: received unknown packet type ("
                             << hdr->nlmsg_type << ") or too short (" << hdr->nlmsg_len << ")";
            else
                ngWarning() << "NetworkInterface/AF_NETLINK: received invalid packet with size "
                             << int(len);
#endif
            return;
        }
    }
};

template <typename Lambda>
void processNetlinkRequest(int sock, struct nlmsghdr *hdr, char *buf, size_t bufsize, Lambda &&l)
{
    ProcessNetlinkRequest<Lambda>()(sock, hdr, buf, bufsize, forward<Lambda>(l));
}
}  // namespace


uint NetworkInterfaceManager::interfaceIndexFromName(const string &name)
{
    uint index = 0;
    if (name.size() >= IFNAMSIZ)
        return index;

    int socket = qt_safe_socket(AF_INET, SOCK_DGRAM, 0);
    if (socket >= 0) {
        struct ifreq req;
        req.ifr_ifindex = 0;
        strcpy(req.ifr_name, name.data());

        if (qt_safe_ioctl(socket, SIOCGIFINDEX, &req) >= 0)
            index = req.ifr_ifindex;
        qt_safe_close(socket);
    }
    return index;
}


string NetworkInterfaceManager::interfaceNameFromIndex(uint index)
{
    int socket = qt_safe_socket(AF_INET, SOCK_DGRAM, 0);
    if (socket >= 0) {
        struct ifreq req;
        req.ifr_ifindex = index;

        if (qt_safe_ioctl(socket, SIOCGIFNAME, &req) >= 0) {
            qt_safe_close(socket);
            return req.ifr_name;
        }
        qt_safe_close(socket);
    }
    return string();
}


static vector<NetworkInterfacePrivate *> getInterfaces(int sock, char *buf)
{
    vector<NetworkInterfacePrivate *> result;
    struct ifreq req;

    // request all links
    struct {
        struct nlmsghdr req;
        struct ifinfomsg ifi;
    } ifi_req;
    memset(&ifi_req, 0, sizeof(ifi_req));

    ifi_req.req.nlmsg_len = sizeof(ifi_req);
    ifi_req.req.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    ifi_req.req.nlmsg_type = RTM_GETLINK;

    // parse the interfaces
    processNetlinkRequest(sock, &ifi_req.req, buf, BufferSize, [&](ifinfomsg *ifi, size_t len) {
        NetworkInterfacePrivate *iface = new NetworkInterfacePrivate();
        iface->index = ifi->ifi_index;
        iface->flags = convertint(ifi->ifi_flags);

        // read attributes
        struct rtattr *rta = reinterpret_cast<struct rtattr *>(ifi + 1);
        len -= sizeof(*ifi);
        for ( ; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
            int payloadLen = RTA_PAYLOAD(rta);
            char *payloadPtr = reinterpret_cast<char *>(RTA_DATA(rta));

            switch (rta->rta_type) {
            case IFLA_ADDRESS:      // link-level address
                iface->hardwareAddress =
                        iface->makeHwAddress(payloadLen, reinterpret_cast<uint8_t *>(payloadPtr));
                break;

            case IFLA_IFNAME:       // interface name
                assert(payloadLen <= int(sizeof(req.ifr_name)));
                memcpy(req.ifr_name, payloadPtr, payloadLen);   // including terminating NUL
                iface->name.assign(payloadPtr, payloadLen - 1);
                break;

            case IFLA_MTU:
                assert(payloadLen == sizeof(int));
                iface->mtu = *reinterpret_cast<int *>(payloadPtr);
                break;

            case IFLA_OPERSTATE:    // operational state
                if (*payloadPtr != IF_OPER_UNKNOWN) {
                    // override the flag
                    iface->flags &= ~NetworkInterface::IsUp;
                    if (*payloadPtr == IF_OPER_UP)
                        iface->flags |= NetworkInterface::IsUp;
                }
                break;
            }
        }

        if ((iface->name.empty())) {
            ngWarning() << "NetworkInterface: found interface " << iface->index << " with no name";
            delete iface;
        } else {
            iface->type = probeIfType(sock, &req, ifi->ifi_type);
            result.push_back(iface);
        }
    });
    return result;
}


static void getAddresses(int sock, char *buf, vector<NetworkInterfacePrivate *> &result)
{
    // request all addresses
    struct {
        struct nlmsghdr req;
        struct ifaddrmsg ifa;
    } ifa_req;
    memset(&ifa_req, 0, sizeof(ifa_req));

    ifa_req.req.nlmsg_len = sizeof(ifa_req);
    ifa_req.req.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    ifa_req.req.nlmsg_type = RTM_GETADDR;
    ifa_req.req.nlmsg_seq = 1;

    // parse the addresses
    processNetlinkRequest(sock, &ifa_req.req, buf, BufferSize, [&](ifaddrmsg *ifa, size_t len) {
        if ((ifa->ifa_family != AF_INET && ifa->ifa_family != AF_INET6)) {
            // unknown address types
            return;
        }

        // find the interface this is relevant to
        NetworkInterfacePrivate *iface = nullptr;
        for (NetworkInterfacePrivate *candidate : result) {
            if (candidate->index != int(ifa->ifa_index))
                continue;
            iface = candidate;
            break;
        }

        if ((!iface)) {
            ngWarning() << "NetworkInterface/AF_NETLINK: found unknown interface with index "
                         << ifa->ifa_index;
            return;
        }

        NetworkAddressEntry entry;
        uint32_t flags = ifa->ifa_flags;  // may be overwritten by IFA_FLAGS

        function<HostAddress(uint8_t *, int)> makeAddress = [=](uint8_t *ptr, int len) {
            HostAddress addr;
            if (ifa->ifa_family == AF_INET) {
                assert(len == 4);
                addr.setAddress(ngFromBigEndian<uint32_t>(ptr));
            } else {
                assert(len == 16);
                addr.setAddress(ptr);

                // do we need a scope ID?
                if (addr.isLinkLocal())
                    addr.setScopeId(iface->name);
            }
            return addr;
        };

        // read attributes
        struct rtattr *rta = reinterpret_cast<struct rtattr *>(ifa + 1);
        len -= sizeof(*ifa);
        for ( ; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
            int payloadLen = RTA_PAYLOAD(rta);
            uint8_t *payloadPtr = reinterpret_cast<uint8_t *>(RTA_DATA(rta));

            switch (rta->rta_type) {
            case IFA_ADDRESS:
                // Local address (all interfaces except for point-to-point)
                if (entry.ip().isNull())
                    entry.setIp(makeAddress(payloadPtr, payloadLen));
                break;

            case IFA_LOCAL:
                // Override the local address (point-to-point interfaces)
                entry.setIp(makeAddress(payloadPtr, payloadLen));
                break;

            case IFA_BROADCAST:
                assert(ifa->ifa_family == AF_INET);
                entry.setBroadcast(makeAddress(payloadPtr, payloadLen));
                break;

            case IFA_CACHEINFO:
                if (size_t(payloadLen) >= sizeof(ifa_cacheinfo)) {
                    ifa_cacheinfo *cacheinfo = reinterpret_cast<ifa_cacheinfo *>(payloadPtr);
                    const auto toDeadline = [](uint32_t lifetime) -> int64_t {
                        if (lifetime == uint32_t(-1))
                            return -1;
                        return static_cast<int64_t>(lifetime) * 1000;
                    };
                    entry.setAddressLifetime(toDeadline(cacheinfo->ifa_prefered), toDeadline(cacheinfo->ifa_valid));
                }
                break;

            case IFA_FLAGS:
                assert(payloadLen == 4);
                flags = *reinterpret_cast<const uint32_t *>(payloadPtr);
                break;
            }
        }

        // now handle flags
        NetworkInterfacePrivate::calculateDnsEligibility(&entry,
                                                          flags & IFA_F_TEMPORARY,
                                                          flags & IFA_F_DEPRECATED);


        if (!entry.ip().isNull()) {
            entry.setPrefixLength(ifa->ifa_prefixlen);
            iface->addressEntries.push_back(entry);
        }
    });
}


vector<NetworkInterfacePrivate *> NetworkInterfaceManager::scan()
{
    // open netlink socket
    vector<NetworkInterfacePrivate *> result;
    NetlinkSocket sock(BufferSize);
    if ((sock == -1))
        return result;

    vector<char> buffer(BufferSize);
    char *buf = buffer.data();

    result = getInterfaces(sock, buf);
    getAddresses(sock, buf, result);

    return result;
}


}  // namespace qtng
