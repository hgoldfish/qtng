#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "qtng/network_interface.h"
#include "qtng/private/network_interface_p.h"
#include "qtng/utils/platform.h"
#include "qtng/utils/string_utils.h"

using namespace std;

namespace qtng {

static vector<NetworkInterfacePrivate *> postProcess(vector<NetworkInterfacePrivate *> list)
{
    for (NetworkInterfacePrivate *interface : list) {
        for (NetworkAddressEntry &address : interface->addressEntries) {
            if (address.ip().protocol() != HostAddress::IPv4Protocol) {
                continue;
            }
            if (!address.netmask().isNull() && address.broadcast().isNull()) {
                HostAddress bcast = address.ip();
                bcast = HostAddress(bcast.toIPv4Address() | ~address.netmask().toIPv4Address());
                address.setBroadcast(bcast);
            }
        }
    }

    return list;
}

NG_GLOBAL_STATIC(NetworkInterfaceManager, manager)
NetworkInterfaceManager::NetworkInterfaceManager() { }
NetworkInterfaceManager::~NetworkInterfaceManager() { }

shared_ptr<NetworkInterfacePrivate> NetworkInterfaceManager::interfaceFromName(const string &name)
{
    const vector<shared_ptr<NetworkInterfacePrivate>> &interfaceList = allInterfaces();

    bool ok = false;
    int index = utils::parseInt(name, &ok);

    for (const shared_ptr<NetworkInterfacePrivate> &interface : interfaceList) {
        if (ok && interface->index == index) {
            return interface;
        } else if (interface->name == name) {
            return interface;
        }
    }

    return empty;
}

shared_ptr<NetworkInterfacePrivate> NetworkInterfaceManager::interfaceFromIndex(int index)
{
    const vector<shared_ptr<NetworkInterfacePrivate>> &interfaceList = allInterfaces();
    for (const shared_ptr<NetworkInterfacePrivate> &interface : interfaceList) {
        if (interface->index == index) {
            return interface;
        }
    }

    return empty;
}

vector<shared_ptr<NetworkInterfacePrivate>> NetworkInterfaceManager::allInterfaces()
{
    const vector<NetworkInterfacePrivate *> list = postProcess(scan());
    vector<shared_ptr<NetworkInterfacePrivate>> result;
    result.reserve(list.size());

    for (NetworkInterfacePrivate *ptr : list) {
        if ((ptr->flags & NetworkInterface::IsUp) == 0) {
            for (NetworkAddressEntry &addr : ptr->addressEntries) {
                addr.setDnsEligibility(NetworkAddressEntry::DnsIneligible);
            }
        }
        result.push_back(shared_ptr<NetworkInterfacePrivate>(ptr));
    }

    return result;
}

static inline char toHexUpper(uint8_t i)
{
    assert(i < 16);
    return "0123456789ABCDEF"[i];
}

string NetworkInterfacePrivate::makeHwAddress(int len, uint8_t *data)
{
    const int outLen = max(len * 2 + (len - 1) * 1, 0);
    string result(outLen, '\0');
    char *out = &result[0];
    for (int i = 0; i < len; ++i) {
        if (i) {
            *out++ = ':';
        }
        *out++ = toHexUpper(data[i] / 16);
        *out++ = toHexUpper(data[i] % 16);
    }
    return result;
}

NetworkAddressEntry::NetworkAddressEntry()
    : d(new NetworkAddressEntryPrivate)
{
}

NetworkAddressEntry::NetworkAddressEntry(const NetworkAddressEntry &other)
    : d(new NetworkAddressEntryPrivate(*other.d.get()))
{
}

NetworkAddressEntry::NetworkAddressEntry(NetworkAddressEntry &&other) noexcept
    : d(std::move(other.d))
{
}

NetworkAddressEntry &NetworkAddressEntry::operator=(const NetworkAddressEntry &other)
{
    *d.get() = *other.d.get();
    return *this;
}

NetworkAddressEntry &NetworkAddressEntry::operator=(NetworkAddressEntry &&other) noexcept
{
    d = std::move(other.d);
    return *this;
}

NetworkAddressEntry::~NetworkAddressEntry() { }

bool NetworkAddressEntry::operator==(const NetworkAddressEntry &other) const
{
    if (d == other.d)
        return true;
    if (!d || !other.d)
        return false;
    return d->address == other.d->address && d->netmask == other.d->netmask && d->broadcast == other.d->broadcast;
}

NetworkAddressEntry::DnsEligibilityStatus NetworkAddressEntry::dnsEligibility() const
{
    return d->dnsEligibility;
}

void NetworkAddressEntry::setDnsEligibility(DnsEligibilityStatus status)
{
    d->dnsEligibility = status;
}

HostAddress NetworkAddressEntry::ip() const
{
    return d->address;
}

void NetworkAddressEntry::setIp(const HostAddress &newIp)
{
    d->address = newIp;
}

HostAddress NetworkAddressEntry::netmask() const
{
    return d->netmask.address(d->address.protocol());
}

void NetworkAddressEntry::setNetmask(const HostAddress &newNetmask)
{
    if (newNetmask.protocol() != ip().protocol()) {
        d->netmask = Netmask();
        return;
    }
    d->netmask.setAddress(newNetmask);
}

int NetworkAddressEntry::prefixLength() const
{
    return d->netmask.prefixLength();
}

void NetworkAddressEntry::setPrefixLength(int length)
{
    d->netmask.setPrefixLength(d->address.protocol(), length);
}

HostAddress NetworkAddressEntry::broadcast() const
{
    return d->broadcast;
}

void NetworkAddressEntry::setBroadcast(const HostAddress &newBroadcast)
{
    d->broadcast = newBroadcast;
}

bool NetworkAddressEntry::isLifetimeKnown() const
{
    return d->lifetimeKnown;
}

void NetworkAddressEntry::setAddressLifetime(int64_t preferredMs, int64_t validityMs)
{
    d->preferredLifetimeMs = preferredMs;
    d->validityLifetimeMs = validityMs;
    d->lifetimeKnown = true;
}

void NetworkAddressEntry::clearAddressLifetime()
{
    d->preferredLifetimeMs = -1;
    d->validityLifetimeMs = -1;
    d->lifetimeKnown = false;
}

bool NetworkAddressEntry::isPermanent() const
{
    return d->validityLifetimeMs < 0;
}

NetworkInterface::NetworkInterface()
    : d(nullptr)
{
}

NetworkInterface::~NetworkInterface() { }

NetworkInterface::NetworkInterface(const NetworkInterface &other)
    : d(other.d)
{
}

NetworkInterface &NetworkInterface::operator=(const NetworkInterface &other)
{
    d = other.d;
    return *this;
}

bool NetworkInterface::isValid() const
{
    return !name().empty();
}

int NetworkInterface::index() const
{
    return d ? d->index : 0;
}

int NetworkInterface::maximumTransmissionUnit() const
{
    return d ? d->mtu : 0;
}

string NetworkInterface::name() const
{
    return d ? d->name : string();
}

string NetworkInterface::humanReadableName() const
{
    if (!d) {
        return string();
    }
    return !d->friendlyName.empty() ? d->friendlyName : name();
}

NetworkInterface::InterfaceFlags NetworkInterface::flags() const
{
    return d ? d->flags : InterfaceFlags();
}

NetworkInterface::InterfaceType NetworkInterface::type() const
{
    return d ? d->type : Unknown;
}

string NetworkInterface::hardwareAddress() const
{
    return d ? d->hardwareAddress : string();
}

vector<NetworkAddressEntry> NetworkInterface::addressEntries() const
{
    return d ? d->addressEntries : vector<NetworkAddressEntry>();
}

int NetworkInterface::interfaceIndexFromName(const string &name)
{
    if (name.empty())
        return 0;

    bool ok = false;
    int id = utils::parseInt(name, &ok);
    if (!ok)
        id = static_cast<int>(NetworkInterfaceManager::interfaceIndexFromName(name));
    return id;
}

NetworkInterface NetworkInterface::interfaceFromName(const string &name)
{
    NetworkInterface result;
    result.d = manager().interfaceFromName(name);
    return result;
}

NetworkInterface NetworkInterface::interfaceFromIndex(int index)
{
    NetworkInterface result;
    result.d = manager().interfaceFromIndex(index);
    return result;
}

string NetworkInterface::interfaceNameFromIndex(int index)
{
    if (!index)
        return string();
    return NetworkInterfaceManager::interfaceNameFromIndex(index);
}

vector<NetworkInterface> NetworkInterface::allInterfaces()
{
    const vector<shared_ptr<NetworkInterfacePrivate>> privs = manager().allInterfaces();
    vector<NetworkInterface> result;
    result.reserve(privs.size());
    for (const shared_ptr<NetworkInterfacePrivate> &p : privs) {
        NetworkInterface item;
        item.d = p;
        result.push_back(item);
    }

    return result;
}

vector<HostAddress> NetworkInterface::allAddresses()
{
    const vector<shared_ptr<NetworkInterfacePrivate>> privs = manager().allInterfaces();
    vector<HostAddress> result;
    for (const shared_ptr<NetworkInterfacePrivate> &p : privs) {
        if ((p->flags & NetworkInterface::IsUp) == 0) {
            continue;
        }
        for (const NetworkAddressEntry &entry : p->addressEntries) {
            result.push_back(entry.ip());
        }
    }

    return result;
}

}  // namespace qtng

#ifndef NG_NO_DEBUG_STREAM
static inline ostream &flagsDebug(ostream &debug, qtng::NetworkInterface::InterfaceFlags flags)
{
    if (flags & qtng::NetworkInterface::IsUp)
        debug << "IsUp ";
    if (flags & qtng::NetworkInterface::IsRunning)
        debug << "IsRunning ";
    if (flags & qtng::NetworkInterface::CanBroadcast)
        debug << "CanBroadcast ";
    if (flags & qtng::NetworkInterface::IsLoopBack)
        debug << "IsLoopBack ";
    if (flags & qtng::NetworkInterface::IsPointToPoint)
        debug << "IsPointToPoint ";
    if (flags & qtng::NetworkInterface::CanMulticast)
        debug << "CanMulticast ";
    return debug;
}

ostream &operator<<(ostream &debug, const qtng::NetworkAddressEntry &entry)
{
    debug << "(address = " << entry.ip();
    if (!entry.netmask().isNull())
        debug << ", netmask = " << entry.netmask();
    if (!entry.broadcast().isNull())
        debug << ", broadcast = " << entry.broadcast();
    debug << ')';
    return debug;
}

ostream &operator<<(ostream &debug, const qtng::NetworkInterface &networkInterface)
{
    debug << "NetworkInterface(name = " << networkInterface.name()
          << ", hardware address = " << networkInterface.hardwareAddress() << ", flags = ";
    flagsDebug(debug, networkInterface.flags());
    debug << ")\n";
    return debug;
}

#endif  // NG_NO_DEBUG_STREAM
