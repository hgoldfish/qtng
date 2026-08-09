#include <QtCore/qdebug.h>

#include "bridge/core_access.h"
#include "network_interface.h"
#include "hostaddress.h"

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class NetworkAddressEntryPrivate
{
public:
    qtng_core::NetworkAddressEntry core;
#if QT_VERSION > QT_VERSION_CHECK(5, 8, 0)
    QDeadlineTimer preferredLifetime = QDeadlineTimer(QDeadlineTimer::Forever);
    QDeadlineTimer validityLifetime = QDeadlineTimer(QDeadlineTimer::Forever);
#endif
};

class NetworkInterfacePrivate : public QSharedData
{
public:
    qtng_core::NetworkInterface core;
};

namespace {

qtng_core::HostAddress toCoreAddress(const HostAddress &addr)
{
    if (addr.isNull()) {
        return qtng_core::HostAddress();
    }
    if (addr.protocol() == HostAddress::IPv4Protocol) {
        return qtng_core::HostAddress(addr.toIPv4Address());
    }
    const IPv6Address v6 = addr.toIPv6Address();
    qtng_core::HostAddress result(v6.c);
    result.setScopeId(toStdString(addr.scopeId()));
    return result;
}

HostAddress toQtAddress(const qtng_core::HostAddress &addr)
{
    if (addr.isNull()) {
        return HostAddress();
    }
    if (addr.protocol() == qtng_core::HostAddress::IPv4Protocol) {
        return HostAddress(addr.toIPv4Address());
    }
    const qtng_core::IPv6Address v6 = addr.toIPv6Address();
    IPv6Address qv6;
    for (int i = 0; i < 16; ++i) {
        qv6.c[i] = v6.c[i];
    }
    HostAddress result(qv6);
    result.setScopeId(toQString(addr.scopeId()));
    return result;
}

NetworkAddressEntry toQtNetworkAddressEntry(const qtng_core::NetworkAddressEntry &entry)
{
    NetworkAddressEntry result;
    result.setIp(toQtAddress(entry.ip()));
    result.setNetmask(toQtAddress(entry.netmask()));
    result.setPrefixLength(entry.prefixLength());
    result.setBroadcast(toQtAddress(entry.broadcast()));
    result.setDnsEligibility(static_cast<NetworkAddressEntry::DnsEligibilityStatus>(entry.dnsEligibility()));
#if QT_VERSION > QT_VERSION_CHECK(5, 8, 0)
    if (entry.isLifetimeKnown()) {
        result.setAddressLifetime(QDeadlineTimer(QDeadlineTimer::Forever),
                                  entry.isPermanent() ? QDeadlineTimer(QDeadlineTimer::Forever) : QDeadlineTimer(0));
    }
#endif
    return result;
}

NetworkInterface toQtNetworkInterface(const qtng_core::NetworkInterface &iface)
{
    if (!iface.isValid()) {
        return NetworkInterface();
    }
    return NetworkInterface::interfaceFromName(toQString(iface.name()));
}

}  // namespace

NetworkAddressEntry::NetworkAddressEntry()
    : d(new NetworkAddressEntryPrivate)
{
}

NetworkAddressEntry::NetworkAddressEntry(const NetworkAddressEntry &other)
    : d(new NetworkAddressEntryPrivate(*other.d))
{
}

NetworkAddressEntry &NetworkAddressEntry::operator=(const NetworkAddressEntry &other)
{
    *d = *other.d;
    return *this;
}

NetworkAddressEntry::~NetworkAddressEntry() = default;

bool NetworkAddressEntry::operator==(const NetworkAddressEntry &other) const
{
    return d->core == other.d->core;
}

NetworkAddressEntry::DnsEligibilityStatus NetworkAddressEntry::dnsEligibility() const
{
    return static_cast<DnsEligibilityStatus>(d->core.dnsEligibility());
}

void NetworkAddressEntry::setDnsEligibility(DnsEligibilityStatus status)
{
    d->core.setDnsEligibility(static_cast<qtng_core::NetworkAddressEntry::DnsEligibilityStatus>(status));
}

HostAddress NetworkAddressEntry::ip() const
{
    return toQtAddress(d->core.ip());
}

void NetworkAddressEntry::setIp(const HostAddress &newIp)
{
    d->core.setIp(toCoreAddress(newIp));
}

HostAddress NetworkAddressEntry::netmask() const
{
    return toQtAddress(d->core.netmask());
}

void NetworkAddressEntry::setNetmask(const HostAddress &newNetmask)
{
    d->core.setNetmask(toCoreAddress(newNetmask));
}

int NetworkAddressEntry::prefixLength() const
{
    return d->core.prefixLength();
}

void NetworkAddressEntry::setPrefixLength(int length)
{
    d->core.setPrefixLength(length);
}

HostAddress NetworkAddressEntry::broadcast() const
{
    return toQtAddress(d->core.broadcast());
}

void NetworkAddressEntry::setBroadcast(const HostAddress &newBroadcast)
{
    d->core.setBroadcast(toCoreAddress(newBroadcast));
}

#if QT_VERSION > QT_VERSION_CHECK(5, 8, 0)
bool NetworkAddressEntry::isLifetimeKnown() const
{
    return d->core.isLifetimeKnown();
}

QDeadlineTimer NetworkAddressEntry::preferredLifetime() const
{
    return d->preferredLifetime;
}

QDeadlineTimer NetworkAddressEntry::validityLifetime() const
{
    return d->validityLifetime;
}

void NetworkAddressEntry::setAddressLifetime(QDeadlineTimer preferred, QDeadlineTimer validity)
{
    d->preferredLifetime = preferred;
    d->validityLifetime = validity;
    const qint64 prefMs = preferred.isForever() ? -1 : preferred.remainingTime();
    const qint64 validMs = validity.isForever() ? -1 : validity.remainingTime();
    d->core.setAddressLifetime(prefMs, validMs);
}

void NetworkAddressEntry::clearAddressLifetime()
{
    d->core.clearAddressLifetime();
}

bool NetworkAddressEntry::isPermanent() const
{
    return d->core.isPermanent();
}
#endif

NetworkInterface::NetworkInterface()
    : d(new NetworkInterfacePrivate)
{
}

NetworkInterface::NetworkInterface(const NetworkInterface &other)
    : d(other.d)
{
}

NetworkInterface &NetworkInterface::operator=(const NetworkInterface &other)
{
    d = other.d;
    return *this;
}

NetworkInterface::~NetworkInterface() = default;

bool NetworkInterface::isValid() const
{
    return d && d->core.isValid();
}

int NetworkInterface::index() const
{
    return d ? d->core.index() : 0;
}

int NetworkInterface::maximumTransmissionUnit() const
{
    return d ? d->core.maximumTransmissionUnit() : 0;
}

QString NetworkInterface::name() const
{
    return d ? toQString(d->core.name()) : QString();
}

QString NetworkInterface::humanReadableName() const
{
    return d ? toQString(d->core.humanReadableName()) : QString();
}

NetworkInterface::InterfaceFlags NetworkInterface::flags() const
{
    return d ? static_cast<InterfaceFlags>(d->core.flags()) : InterfaceFlags();
}

NetworkInterface::InterfaceType NetworkInterface::type() const
{
    return d ? static_cast<InterfaceType>(d->core.type()) : Unknown;
}

QString NetworkInterface::hardwareAddress() const
{
    return d ? toQString(d->core.hardwareAddress()) : QString();
}

QList<NetworkAddressEntry> NetworkInterface::addressEntries() const
{
    QList<NetworkAddressEntry> result;
    if (!d) {
        return result;
    }
    for (const qtng_core::NetworkAddressEntry &entry : d->core.addressEntries()) {
        result.append(toQtNetworkAddressEntry(entry));
    }
    return result;
}

int NetworkInterface::interfaceIndexFromName(const QString &name)
{
    return qtng_core::NetworkInterface::interfaceIndexFromName(toStdString(name));
}

NetworkInterface NetworkInterface::interfaceFromName(const QString &name)
{
    return toQtNetworkInterface(qtng_core::NetworkInterface::interfaceFromName(toStdString(name)));
}

NetworkInterface NetworkInterface::interfaceFromIndex(int index)
{
    return toQtNetworkInterface(qtng_core::NetworkInterface::interfaceFromIndex(index));
}

QString NetworkInterface::interfaceNameFromIndex(int index)
{
    return toQString(qtng_core::NetworkInterface::interfaceNameFromIndex(index));
}

QList<NetworkInterface> NetworkInterface::allInterfaces()
{
    const vector<qtng_core::NetworkInterface> ifaces = qtng_core::NetworkInterface::allInterfaces();
    QList<NetworkInterface> result;
    for (const qtng_core::NetworkInterface &iface : ifaces) {
        result.append(toQtNetworkInterface(iface));
    }
    return result;
}

QList<HostAddress> NetworkInterface::allAddresses()
{
    const vector<qtng_core::HostAddress> addrs = qtng_core::NetworkInterface::allAddresses();
    QList<HostAddress> result;
    for (const qtng_core::HostAddress &addr : addrs) {
        result.append(toQtAddress(addr));
    }
    return result;
}

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug debug, const NetworkAddressEntry &entry)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "NetworkAddressEntry(address=" << entry.ip();
    if (!entry.netmask().isNull()) {
        debug << ", netmask=" << entry.netmask();
    }
    if (!entry.broadcast().isNull()) {
        debug << ", broadcast=" << entry.broadcast();
    }
    debug << ')';
    return debug;
}

QDebug operator<<(QDebug debug, const NetworkInterface &networkInterface)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "NetworkInterface(name=" << networkInterface.name()
                    << ", hardwareAddress=" << networkInterface.hardwareAddress()
                    << ", flags=" << static_cast<int>(networkInterface.flags()) << ')';
    return debug;
}
#endif

}  // namespace QTNETWORKNG_NAMESPACE
