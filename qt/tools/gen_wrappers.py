#!/usr/bin/env python3
"""Generate thin Qt wrapper .cpp files that delegate to qtng core."""

import os
import textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

MODULES = [
    ("hostaddress", "HostAddress", "::qtng::HostAddress", "d->core"),
    ("network_interface", "NetworkInterface", "::qtng::NetworkInterface", "d->core"),
    ("random", None, None, None),
    ("gzip", None, None, None),
]

HEADER = textwrap.dedent(
    """\
    #include "bridge/core_access.h"
    #include "{include}"

    using namespace std;
    using namespace QTNETWORKNG_NAMESPACE;
    using namespace qtng_bridge;

    namespace QTNETWORKNG_NAMESPACE {{
    """
)

FOOTER = "}\n"


def write_stub(name: str, include: str, body: str):
    path = os.path.join(SRC, f"{name}.cpp")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(HEADER.format(include=include))
        f.write(body)
        f.write(FOOTER)
    print("wrote", path)


def gen_hostaddress():
    body = textwrap.dedent(
        """\
        class HostAddressPrivate {
        public:
            ::qtng::HostAddress core;
        };

        HostAddress::HostAddress()
            : d(new HostAddressPrivate)
        {
        }

        HostAddress::HostAddress(const HostAddress &copy)
            : d(copy.d)
        {
        }

        HostAddress::HostAddress(SpecialAddress address)
            : d(new HostAddressPrivate)
        {
            d->core.setAddress(static_cast<::qtng::HostAddress::SpecialAddress>(address));
        }

        HostAddress::HostAddress(const QString &address)
            : d(new HostAddressPrivate)
        {
            d->core.setAddress(toStdString(address));
        }

        HostAddress::HostAddress(quint32 ip4Addr)
            : d(new HostAddressPrivate)
        {
            d->core.setAddress(ip4Addr);
        }

        HostAddress::HostAddress(quint8 *ip6Addr)
            : d(new HostAddressPrivate)
        {
            d->core.setAddress(ip6Addr);
        }

        HostAddress::HostAddress(const quint8 *ip6Addr)
            : d(new HostAddressPrivate)
        {
            d->core.setAddress(ip6Addr);
        }

        HostAddress::HostAddress(const IPv6Address &ip6Addr)
            : d(new HostAddressPrivate)
        {
            d->core.setAddress(ip6Addr.data());
        }

        HostAddress::HostAddress(const sockaddr *sockaddr)
            : d(new HostAddressPrivate)
        {
            d->core = ::qtng::HostAddress(sockaddr);
        }

        HostAddress::~HostAddress() = default;

        HostAddress &HostAddress::operator=(const HostAddress &other)
        {
            d = other.d;
            return *this;
        }

        HostAddress &HostAddress::operator=(SpecialAddress address)
        {
            d->core.setAddress(static_cast<::qtng::HostAddress::SpecialAddress>(address));
            return *this;
        }

        bool HostAddress::isEqual(const HostAddress &address, ConversionMode mode) const
        {
            return d->core.isEqual(address.d->core, static_cast<::qtng::HostAddress::ConversionModeFlag>(mode));
        }

        bool HostAddress::operator==(const HostAddress &address) const
        {
            return d->core == address.d->core;
        }

        bool HostAddress::operator==(SpecialAddress address) const
        {
            return d->core == static_cast<::qtng::HostAddress::SpecialAddress>(address);
        }

        void HostAddress::swap(HostAddress &other) noexcept
        {
            d.swap(other.d);
        }

        void HostAddress::clear()
        {
            d->core.clear();
        }

        void HostAddress::setAddress(const IPv4Address ipv4)
        {
            d->core.setAddress(ipv4);
        }

        void HostAddress::setAddress(const IPv6Address &ipv6)
        {
            d->core.setAddress(ipv6.data());
        }

        void HostAddress::setAddress(const quint8 *ipv6)
        {
            d->core.setAddress(ipv6);
        }

        bool HostAddress::setAddress(const QString &ipString)
        {
            return d->core.setAddress(toStdString(ipString));
        }

        void HostAddress::setAddress(SpecialAddress address)
        {
            d->core.setAddress(static_cast<::qtng::HostAddress::SpecialAddress>(address));
        }

        bool HostAddress::isNull() const
        {
            return d->core.isNull();
        }

        HostAddress::NetworkLayerProtocol HostAddress::protocol() const
        {
            return static_cast<NetworkLayerProtocol>(d->core.protocol());
        }

        HostAddress::IPv4Address HostAddress::toIPv4Address(bool *ok) const
        {
            return d->core.toIPv4Address(ok);
        }

        HostAddress::IPv6Address HostAddress::toIPv6Address() const
        {
            const ::qtng::IPv6Address v6 = d->core.toIPv6Address();
            IPv6Address result;
            for (int i = 0; i < 16; ++i) {
                result.data()[i] = v6.data()[i];
            }
            return result;
        }

        bool HostAddress::isInSubnet(const HostAddress &subnet, int netmask) const
        {
            return d->core.isInSubnet(subnet.d->core, netmask);
        }

        bool HostAddress::isInSubnet(const QPair<HostAddress, int> &subnet) const
        {
            return d->core.isInSubnet(make_pair(subnet.first.d->core, subnet.second));
        }

        QPair<HostAddress, int> HostAddress::parseSubnet(const QString &subnet)
        {
            const auto parsed = ::qtng::HostAddress::parseSubnet(toStdString(subnet));
            return qMakePair(HostAddress(toQString(parsed.first.toString())), parsed.second);
        }

        bool HostAddress::isIPv4() const { return d->core.isIPv4(); }
        bool HostAddress::isLoopback() const { return d->core.isLoopback(); }
        bool HostAddress::isGlobal() const { return d->core.isGlobal(); }
        bool HostAddress::isLinkLocal() const { return d->core.isLinkLocal(); }
        bool HostAddress::isSiteLocal() const { return d->core.isSiteLocal(); }
        bool HostAddress::isUniqueLocalUnicast() const { return d->core.isUniqueLocalUnicast(); }
        bool HostAddress::isMulticast() const { return d->core.isMulticast(); }
        bool HostAddress::isBroadcast() const { return d->core.isBroadcast(); }

        QString HostAddress::toString() const
        {
            return toQString(d->core.toString());
        }

        QString HostAddress::scopeId() const
        {
            return toQString(d->core.scopeId());
        }

        void HostAddress::setScopeId(const QString &id)
        {
            d->core.setScopeId(toStdString(id));
        }

        QList<HostAddress> HostAddress::getHostAddressByName(const QString &hostName)
        {
            const vector<::qtng::HostAddress> addrs = ::qtng::HostAddress::getHostAddressByName(toStdString(hostName));
            QList<HostAddress> result;
            for (const ::qtng::HostAddress &addr : addrs) {
                HostAddress ha;
                ha.d->core = addr;
                result.append(ha);
            }
            return result;
        }

        uint qHash(const HostAddress &key, uint seed) noexcept
        {
            return ::qtng::qHash(key.d->core, seed);
        }

        #ifdef Q_OS_WIN
        void initWinSock() { ::qtng::initWinSock(); }
        void freeWinSock() { ::qtng::freeWinSock(); }
        #endif
        """
    )
    write_stub("hostaddress", "hostaddress.h", body)


if __name__ == "__main__":
    gen_hostaddress()
