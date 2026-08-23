#include <QtCore/qshareddata.h>
#include <QtCore/qdebug.h>

#include "bridge/core_access.h"
#include "hostaddress.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class HostAddressPrivate : public QSharedData
{
public:
    qtng_core::HostAddress core;

    static qtng_core::HostAddress coreOf(const HostAddress &addr) { return addr.d->core; }
    static HostAddress fromCore(const qtng_core::HostAddress &coreAddr)
    {
        HostAddress result;
        result.d->core = coreAddr;
        return result;
    }
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
    d->core.setAddress(static_cast<qtng_core::HostAddress::SpecialAddress>(address));
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
    d->core.setAddress(ip6Addr.c);
}

HostAddress::HostAddress(const sockaddr *sockaddr)
    : d(new HostAddressPrivate)
{
    d->core = qtng_core::HostAddress(sockaddr);
}

HostAddress::~HostAddress() = default;

HostAddress &HostAddress::operator=(const HostAddress &other)
{
    d = other.d;
    return *this;
}

HostAddress &HostAddress::operator=(SpecialAddress address)
{
    d->core.setAddress(static_cast<qtng_core::HostAddress::SpecialAddress>(address));
    return *this;
}

bool HostAddress::isEqual(const HostAddress &address, ConversionMode mode) const
{
    return d->core.isEqual(address.d->core, static_cast<qtng_core::HostAddress::ConversionModeFlag>(static_cast<int>(mode)));
}

bool HostAddress::operator==(const HostAddress &address) const
{
    return d->core == address.d->core;
}

bool HostAddress::operator==(SpecialAddress address) const
{
    return d->core == static_cast<qtng_core::HostAddress::SpecialAddress>(address);
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
    d->core.setAddress(ipv6.c);
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
    d->core.setAddress(static_cast<qtng_core::HostAddress::SpecialAddress>(address));
}

bool HostAddress::isNull() const
{
    return d->core.isNull();
}

HostAddress::NetworkLayerProtocol HostAddress::protocol() const
{
    return static_cast<NetworkLayerProtocol>(d->core.protocol());
}

IPv4Address HostAddress::toIPv4Address(bool *ok) const
{
    return d->core.toIPv4Address(ok);
}

IPv6Address HostAddress::toIPv6Address() const
{
    const qtng_core::IPv6Address v6 = d->core.toIPv6Address();
    IPv6Address result;
    for (int i = 0; i < 16; ++i) {
        result.c[i] = v6.c[i];
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
    const auto parsed = qtng_core::HostAddress::parseSubnet(toStdString(subnet));
    HostAddress ha;
    ha.d->core = parsed.first;
    return qMakePair(ha, parsed.second);
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
    const vector<qtng_core::HostAddress> addrs = qtng_core::HostAddress::getHostAddressByName(toStdString(hostName));
    QList<HostAddress> result;
    for (const qtng_core::HostAddress &addr : addrs) {
        HostAddress ha;
        ha.d->core = addr;
        result.append(ha);
    }
    return result;
}

uint qHash(const HostAddress &key, uint seed) noexcept
{
    return qtng_core::qHash(key.d->core, seed);
}

#ifdef Q_OS_WIN
void initWinSock() { qtng_core::initWinSock(); }
void freeWinSock() { qtng_core::freeWinSock(); }
#endif

}  // namespace QTNETWORKNG_NAMESPACE

namespace qtng_bridge {

qtng_core::HostAddress toCoreAddress(const HostAddress &addr)
{
    return HostAddressPrivate::coreOf(addr);
}

HostAddress toQtAddress(const qtng_core::HostAddress &addr)
{
    return HostAddressPrivate::fromCore(addr);
}

}  // namespace qtng_bridge

#ifndef QT_NO_DEBUG_STREAM
QT_BEGIN_NAMESPACE
QDebug operator<<(QDebug d, const QTNETWORKNG_NAMESPACE::HostAddress &t)
{
    QDebug nsp = d.nospace();
    nsp << "HostAddress(" << t.toString() << ')';
    return d.space();
}
QT_END_NAMESPACE
#endif
