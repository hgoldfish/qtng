#include "qtng/private/mdns_p.h"

#include <cstring>

using namespace std;

namespace qtng {

namespace {

uint16_t be16(const char *p)
{
    return static_cast<uint16_t>((static_cast<unsigned char>(p[0]) << 8)
                                 | static_cast<unsigned char>(p[1]));
}

uint32_t be32(const char *p)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24)
            | (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16)
            | (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8)
            | static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

void appendU16(string *out, uint16_t v)
{
    char b[2];
    ngToBigEndian(v, b);
    out->append(b, 2);
}

void appendU32(string *out, uint32_t v)
{
    char b[4];
    ngToBigEndian(v, b);
    out->append(b, 4);
}

void writeU16(string *out, size_t offset, uint16_t v)
{
    char b[2];
    ngToBigEndian(v, b);
    (*out)[offset] = b[0];
    (*out)[offset + 1] = b[1];
}

bool decodeName(const string &packet, size_t *offset, string *name)
{
    size_t pos = *offset;
    int jumps = 0;
    size_t resume = packet.size();
    string out;
    while (true) {
        if (pos >= packet.size()) {
            return false;
        }
        const uint8_t len = static_cast<uint8_t>(packet[pos]);
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= packet.size()) {
                return false;
            }
            const uint16_t ptr = static_cast<uint16_t>(((len & 0x3F) << 8)
                                                       | static_cast<uint8_t>(packet[pos + 1]));
            if (jumps == 0) {
                resume = pos + 2;
            }
            if (++jumps > 100) {
                return false;  // pointer loop protection
            }
            pos = ptr;
            continue;
        }
        if ((len & 0xC0) != 0) {
            return false;  // reserved label type
        }
        if (len == 0) {
            if (jumps == 0) {
                resume = pos + 1;
            }
            break;
        }
        if (pos + 1u + len > packet.size()) {
            return false;
        }
        if (!out.empty()) {
            out.push_back('.');
        }
        out.append(packet, pos + 1, len);
        pos += 1u + len;
    }
    if (name) {
        *name = out.empty() ? "." : out + ".";
    }
    *offset = resume;
    return true;
}

void encodeRRs(string *out, DnsNameEncoder *names, const vector<DnsResourceRecord> &rrs)
{
    for (size_t i = 0; i < rrs.size(); ++i) {
        const DnsResourceRecord &r = rrs[i];
        names->writeName(r.name);
        appendU16(out, r.type);
        appendU16(out, r.klass);
        appendU32(out, r.ttl);
        const size_t lenPos = out->size();
        appendU16(out, 0);  // rdata length placeholder
        if (r.type == DnsTypePTR) {
            names->writeName(r.target);
        } else if (r.type == DnsTypeSRV) {
            appendU16(out, r.priority);
            appendU16(out, r.weight);
            appendU16(out, r.port);
            names->writeName(r.target);
        } else {
            out->append(r.rdata);
        }
        writeU16(out, lenPos, static_cast<uint16_t>(out->size() - lenPos - 2));
    }
}

bool parseRRs(const string &packet, size_t *offset, uint16_t count, vector<DnsResourceRecord> *list)
{
    for (int i = 0; i < count; ++i) {
        DnsResourceRecord r;
        if (!decodeName(packet, offset, &r.name)) {
            return false;
        }
        if (*offset + 10 > packet.size()) {
            return false;
        }
        r.type = be16(packet.data() + *offset);
        r.klass = be16(packet.data() + *offset + 2);
        r.ttl = be32(packet.data() + *offset + 4);
        const uint16_t rdlen = be16(packet.data() + *offset + 8);
        *offset += 10;
        if (*offset + rdlen > packet.size()) {
            return false;
        }
        if (r.type == DnsTypePTR) {
            size_t sub = *offset;
            if (!decodeName(packet, &sub, &r.target)) {
                return false;
            }
            r.rdata = packet.substr(*offset, rdlen);
        } else if (r.type == DnsTypeSRV) {
            if (rdlen < 6) {
                return false;
            }
            r.priority = be16(packet.data() + *offset);
            r.weight = be16(packet.data() + *offset + 2);
            r.port = be16(packet.data() + *offset + 4);
            size_t sub = *offset + 6;
            if (!decodeName(packet, &sub, &r.target)) {
                return false;
            }
            r.rdata = packet.substr(*offset, rdlen);
        } else {
            r.rdata = packet.substr(*offset, rdlen);
        }
        *offset += rdlen;
        list->push_back(r);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// DNS helpers
// ---------------------------------------------------------------------------

string normalizeDnsName(const string &name)
{
    string out;
    out.reserve(name.size() + 1);
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        out.push_back(c);
    }
    if (out.empty() || out[out.size() - 1] != '.') {
        out.push_back('.');
    }
    return out;
}

bool dnsNameMatches(const string &a, const string &b)
{
    return normalizeDnsName(a) == normalizeDnsName(b);
}

string firstDnsLabel(const string &name)
{
    string n = name;
    while (!n.empty() && n[n.size() - 1] == '.') {
        n.pop_back();
    }
    const size_t dot = n.find('.');
    if (dot == string::npos) {
        return n;
    }
    return n.substr(0, dot);
}

map<string, string> parseTxtRecord(const string &rdata)
{
    map<string, string> txt;
    size_t pos = 0;
    while (pos < rdata.size()) {
        const uint8_t len = static_cast<uint8_t>(rdata[pos]);
        ++pos;
        if (pos + len > rdata.size()) {
            break;
        }
        const string kv = rdata.substr(pos, len);
        pos += len;
        const size_t eq = kv.find('=');
        if (eq == string::npos) {
            txt[kv] = string();
        } else {
            txt[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
    }
    return txt;
}

string encodeTxtRecord(const map<string, string> &txt)
{
    string out;
    for (map<string, string>::const_iterator it = txt.begin(); it != txt.end(); ++it) {
        string entry = it->first;
        entry.push_back('=');
        entry += it->second;
        if (entry.size() > 255) {
            continue;  // RFC 6763: single TXT string must fit in 255 bytes
        }
        out.push_back(static_cast<char>(entry.size()));
        out += entry;
    }
    return out;
}

// ---------------------------------------------------------------------------
// DNS message codec
// ---------------------------------------------------------------------------

bool DnsMessage::parse(const string &packet)
{
    questions.clear();
    answers.clear();
    authority.clear();
    additional.clear();
    if (packet.size() < 12) {
        return false;
    }
    id = be16(packet.data());
    flags = be16(packet.data() + 2);
    const uint16_t qd = be16(packet.data() + 4);
    const uint16_t an = be16(packet.data() + 6);
    const uint16_t ns = be16(packet.data() + 8);
    const uint16_t ar = be16(packet.data() + 10);
    size_t offset = 12;
    for (int i = 0; i < qd; ++i) {
        DnsQuestion q;
        if (!decodeName(packet, &offset, &q.name)) {
            return false;
        }
        if (offset + 4 > packet.size()) {
            return false;
        }
        q.type = be16(packet.data() + offset);
        q.klass = be16(packet.data() + offset + 2);
        offset += 4;
        questions.push_back(q);
    }
    if (!parseRRs(packet, &offset, an, &answers)) {
        return false;
    }
    if (!parseRRs(packet, &offset, ns, &authority)) {
        return false;
    }
    if (!parseRRs(packet, &offset, ar, &additional)) {
        return false;
    }
    return true;
}

string DnsMessage::encode() const
{
    string out(12, '\0');
    DnsNameEncoder names(&out);
    for (size_t i = 0; i < questions.size(); ++i) {
        names.writeName(questions[i].name);
        appendU16(&out, questions[i].type);
        appendU16(&out, questions[i].klass);
    }
    encodeRRs(&out, &names, answers);
    encodeRRs(&out, &names, authority);
    encodeRRs(&out, &names, additional);
    writeU16(&out, 0, id);
    writeU16(&out, 2, flags);
    writeU16(&out, 4, static_cast<uint16_t>(questions.size()));
    writeU16(&out, 6, static_cast<uint16_t>(answers.size()));
    writeU16(&out, 8, static_cast<uint16_t>(authority.size()));
    writeU16(&out, 10, static_cast<uint16_t>(additional.size()));
    return out;
}

void DnsNameEncoder::writeName(const string &name)
{
    string n = name;
    if (n.empty() || n[n.size() - 1] != '.') {
        n.push_back('.');
    }
    vector<string> labels;
    size_t start = 0;
    for (size_t i = 0; i < n.size(); ++i) {
        if (n[i] == '.') {
            if (i > start) {
                labels.push_back(n.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    for (size_t i = 0; i < labels.size(); ++i) {
        string suffix;
        for (size_t j = i; j < labels.size(); ++j) {
            if (!suffix.empty()) {
                suffix.push_back('.');
            }
            suffix += labels[j];
        }
        // mDNS names are case-insensitive: match suffixes case-insensitively but
        // write the label bytes with their original case preserved.
        string key;
        key.reserve(suffix.size());
        for (size_t k = 0; k < suffix.size(); ++k) {
            const char c = suffix[k];
            key.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }
        map<string, size_t>::iterator it = seen_.find(key);
        if (it != seen_.end()) {
            appendU16(out_, static_cast<uint16_t>(0xC000 | (it->second & 0x3FFF)));
            return;
        }
        if (labels[i].size() > 63) {
            return;  // invalid label; stop writing
        }
        seen_[key] = out_->size();
        out_->push_back(static_cast<char>(labels[i].size()));
        out_->append(labels[i]);
    }
    out_->push_back('\0');
}

// ---------------------------------------------------------------------------
// MdnsBrowser
// ---------------------------------------------------------------------------

MdnsBrowser::MdnsBrowser(HostAddress::NetworkLayerProtocol proto)
    : d_ptr(new MdnsBrowserPrivate(this, proto))
{
}

MdnsBrowser::~MdnsBrowser()
{
    close();
    delete d_ptr;
}

bool MdnsBrowser::open(uint16_t port)
{
    NG_D(MdnsBrowser);
    return d->open(port);
}

void MdnsBrowser::close()
{
    NG_D(MdnsBrowser);
    d->close();
}

bool MdnsBrowser::isOpen() const
{
    NG_D(const MdnsBrowser);
    return d->opened;
}

HostAddress MdnsBrowser::localAddress() const
{
    NG_D(const MdnsBrowser);
    return d->socket ? d->socket->localAddress() : HostAddress();
}

uint16_t MdnsBrowser::localPort() const
{
    NG_D(const MdnsBrowser);
    return d->socket ? d->socket->localPort() : 0;
}

void MdnsBrowser::setServer(const HostAddress &addr, uint16_t port)
{
    NG_D(MdnsBrowser);
    d->serverSet = !addr.isNull() && port != 0;
    d->serverAddr = addr;
    d->serverPort = port;
}

vector<MdnsService> MdnsBrowser::browse(const string &type, float timeoutSecs)
{
    NG_D(MdnsBrowser);
    return d->browse(type, timeoutSecs);
}

vector<HostAddress> MdnsBrowser::resolve(const MdnsService &service, float timeoutSecs)
{
    NG_D(MdnsBrowser);
    return d->resolve(service, timeoutSecs);
}

vector<HostAddress> MdnsBrowser::lookup(const string &name, float timeoutSecs)
{
    NG_D(MdnsBrowser);
    return d->lookup(name, timeoutSecs);
}

string MdnsBrowser::errorString() const
{
    NG_D(const MdnsBrowser);
    return d->error;
}

MdnsBrowserPrivate::MdnsBrowserPrivate(MdnsBrowser *q, HostAddress::NetworkLayerProtocol p)
    : q_ptr(q)
    , proto(p)
    , opened(false)
    , serverSet(false)
    , serverPort(0)
    , queryCounter(0)
{
}

MdnsBrowserPrivate::~MdnsBrowserPrivate() { }

bool MdnsBrowserPrivate::open(uint16_t port)
{
    if (opened) {
        return true;
    }
    socket = make_shared<Socket>(proto, Socket::UdpSocket);
    socket->setOption(Socket::AddressReusable, true);
    const HostAddress bindAddr = (proto == HostAddress::IPv6Protocol) ? HostAddress::AnyIPv6
                                                                      : HostAddress::AnyIPv4;
    if (!socket->bind(bindAddr, port)) {
        error = "UDP bind failed: " + socket->errorString();
        socket.reset();
        return false;
    }
    if (port == kMdnsPort) {
        HostAddress group;
        group.setAddress(proto == HostAddress::IPv6Protocol ? kMdnsV6Group : kMdnsV4Group);
        socket->joinMulticastGroup(group);  // best effort; LAN discovery only
    }
    workers = make_unique<CoroutineGroup>();
    workers->spawnWithName("mdns-browser-recv", [this] { recvLoop(); });
    opened = true;
    return true;
}

void MdnsBrowserPrivate::close()
{
    if (!opened && !socket) {
        return;
    }
    opened = false;
    if (socket) {
        socket->abort();
    }
    if (workers) {
        workers->killall(true);
        workers.reset();
    }
    {
        waitersLock.tryAcquire();
        waiters.clear();
        waitersLock.release();
    }
    socket.reset();
}

void MdnsBrowserPrivate::recvLoop()
{
    while (opened && socket) {
        HostAddress from;
        uint16_t fromPort = 0;
        string data = socket->recvfrom(65535, &from, &fromPort);
        if (data.empty()) {
            if (!opened) {
                break;
            }
            Coroutine::msleep(10);
            continue;
        }
        DnsMessage msg;
        if (!msg.parse(data)) {
            continue;
        }
        if ((msg.flags & 0x8000) == 0) {
            continue;  // not a response
        }
        waitersLock.tryAcquire();
        map<uint16_t, shared_ptr<ValueEvent<DnsMessage>>>::iterator it = waiters.find(msg.id);
        if (it != waiters.end()) {
            it->second->send(msg);
        }
        waitersLock.release();
    }
}

uint16_t MdnsBrowserPrivate::nextQueryId()
{
    ++queryCounter;
    if (queryCounter == 0) {
        queryCounter = 1;
    }
    return queryCounter;
}

DnsMessage MdnsBrowserPrivate::queryOne(const string &name, uint16_t type, float timeoutSecs)
{
    DnsMessage reply;
    if (!opened || !socket) {
        return reply;
    }
    const uint16_t id = nextQueryId();
    DnsMessage query;
    query.id = id;
    query.flags = 0x0000;
    DnsQuestion q;
    q.name = normalizeDnsName(name);
    q.type = type;
    q.klass = DnsClassIN;
    query.questions.push_back(q);

    shared_ptr<ValueEvent<DnsMessage>> waiter = make_shared<ValueEvent<DnsMessage>>();
    {
        waitersLock.tryAcquire();
        waiters[id] = waiter;
        waitersLock.release();
    }
    HostAddress dest;
    uint16_t destPort = kMdnsPort;
    if (serverSet) {
        dest = serverAddr;
        destPort = serverPort;
    } else {
        dest.setAddress(proto == HostAddress::IPv6Protocol ? kMdnsV6Group : kMdnsV4Group);
    }
    const string packet = query.encode();
    if (socket->sendto(packet, dest, destPort) <= 0) {
        waitersLock.tryAcquire();
        waiters.erase(id);
        waitersLock.release();
        return reply;
    }
    try {
        Timeout timeout(timeoutSecs);
        (void) timeout;
        reply = waiter->tryWait(static_cast<uint32_t>(timeoutSecs * 1000));
    } catch (TimeoutException &) {
        reply.id = 0;
    }
    waitersLock.tryAcquire();
    waiters.erase(id);
    waitersLock.release();
    return reply;
}

vector<DnsResourceRecord> MdnsBrowserPrivate::queryRecords(const string &name, uint16_t type,
                                                           float timeoutSecs)
{
    vector<DnsResourceRecord> out;
    const DnsMessage reply = queryOne(name, type, timeoutSecs);
    if (reply.id == 0) {
        return out;
    }
    const string qname = normalizeDnsName(name);
    for (size_t i = 0; i < reply.answers.size(); ++i) {
        if (dnsNameMatches(reply.answers[i].name, qname)
            && (type == 0 || reply.answers[i].type == type)) {
            out.push_back(reply.answers[i]);
        }
    }
    for (size_t i = 0; i < reply.additional.size(); ++i) {
        if (dnsNameMatches(reply.additional[i].name, qname)
            && (type == 0 || reply.additional[i].type == type)) {
            out.push_back(reply.additional[i]);
        }
    }
    return out;
}

vector<MdnsService> MdnsBrowserPrivate::browse(const string &type, float timeoutSecs)
{
    vector<MdnsService> services;
    const string qtype = normalizeDnsName(type);
    const DnsMessage reply = queryOne(qtype, DnsTypePTR, timeoutSecs);
    if (reply.id == 0) {
        return services;
    }
    vector<DnsResourceRecord> all;
    all.insert(all.end(), reply.answers.begin(), reply.answers.end());
    all.insert(all.end(), reply.authority.begin(), reply.authority.end());
    all.insert(all.end(), reply.additional.begin(), reply.additional.end());

    for (size_t i = 0; i < all.size(); ++i) {
        const DnsResourceRecord &r = all[i];
        if (r.type != DnsTypePTR || !dnsNameMatches(r.name, qtype)) {
            continue;
        }
        MdnsService svc;
        svc.type = qtype;
        svc.instance = firstDnsLabel(r.target);
        const string instanceFull = normalizeDnsName(r.target);
        for (size_t j = 0; j < all.size(); ++j) {
            const DnsResourceRecord &rr = all[j];
            if (rr.type == DnsTypeSRV && dnsNameMatches(rr.name, instanceFull)) {
                svc.host = normalizeDnsName(rr.target);
                svc.port = rr.port;
            } else if (rr.type == DnsTypeTXT && dnsNameMatches(rr.name, instanceFull)) {
                svc.txt = parseTxtRecord(rr.rdata);
            } else if (!svc.host.empty() && dnsNameMatches(rr.name, svc.host)
                       && (rr.type == DnsTypeA || rr.type == DnsTypeAAAA)) {
                HostAddress addr;
                if (rr.type == DnsTypeA && rr.rdata.size() >= 4) {
                    addr.setAddress(be32(rr.rdata.data()));
                    svc.addresses.push_back(addr);
                } else if (rr.type == DnsTypeAAAA && rr.rdata.size() >= 16) {
                    addr.setAddress(reinterpret_cast<const uint8_t *>(rr.rdata.data()));
                    svc.addresses.push_back(addr);
                }
            }
        }
        services.push_back(svc);
    }
    return services;
}

vector<HostAddress> MdnsBrowserPrivate::resolve(const MdnsService &service, float timeoutSecs)
{
    vector<HostAddress> out;
    if (!service.addresses.empty()) {
        return service.addresses;
    }
    if (service.host.empty()) {
        return out;
    }
    const vector<DnsResourceRecord> a = queryRecords(service.host, DnsTypeA, timeoutSecs);
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].rdata.size() >= 4) {
            HostAddress addr;
            addr.setAddress(be32(a[i].rdata.data()));
            out.push_back(addr);
        }
    }
    const vector<DnsResourceRecord> aaaa = queryRecords(service.host, DnsTypeAAAA, timeoutSecs);
    for (size_t i = 0; i < aaaa.size(); ++i) {
        if (aaaa[i].rdata.size() >= 16) {
            HostAddress addr;
            addr.setAddress(reinterpret_cast<const uint8_t *>(aaaa[i].rdata.data()));
            out.push_back(addr);
        }
    }
    return out;
}

vector<HostAddress> MdnsBrowserPrivate::lookup(const string &name, float timeoutSecs)
{
    vector<HostAddress> out;
    const vector<DnsResourceRecord> a = queryRecords(name, DnsTypeA, timeoutSecs);
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].rdata.size() >= 4) {
            HostAddress addr;
            addr.setAddress(be32(a[i].rdata.data()));
            out.push_back(addr);
        }
    }
    const vector<DnsResourceRecord> aaaa = queryRecords(name, DnsTypeAAAA, timeoutSecs);
    for (size_t i = 0; i < aaaa.size(); ++i) {
        if (aaaa[i].rdata.size() >= 16) {
            HostAddress addr;
            addr.setAddress(reinterpret_cast<const uint8_t *>(aaaa[i].rdata.data()));
            out.push_back(addr);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// MdnsResponder
// ---------------------------------------------------------------------------

MdnsResponder::MdnsResponder(HostAddress::NetworkLayerProtocol proto)
    : d_ptr(new MdnsResponderPrivate(this, proto))
{
}

MdnsResponder::~MdnsResponder()
{
    close();
    delete d_ptr;
}

bool MdnsResponder::open(const HostAddress &addr, uint16_t port)
{
    NG_D(MdnsResponder);
    return d->open(addr, port);
}

bool MdnsResponder::open(uint16_t port)
{
    NG_D(MdnsResponder);
    const HostAddress addr =
            (d->proto == HostAddress::IPv6Protocol) ? HostAddress::AnyIPv6 : HostAddress::AnyIPv4;
    return d->open(addr, port);
}

void MdnsResponder::close()
{
    NG_D(MdnsResponder);
    d->close();
}

bool MdnsResponder::isOpen() const
{
    NG_D(const MdnsResponder);
    return d->opened;
}

HostAddress MdnsResponder::localAddress() const
{
    NG_D(const MdnsResponder);
    return d->socket ? d->socket->localAddress() : HostAddress();
}

uint16_t MdnsResponder::localPort() const
{
    NG_D(const MdnsResponder);
    return d->socket ? d->socket->localPort() : 0;
}

void MdnsResponder::setHostName(const string &name)
{
    NG_D(MdnsResponder);
    d->defaultHost = normalizeDnsName(name);
}

string MdnsResponder::hostName() const
{
    NG_D(const MdnsResponder);
    return d->defaultHost;
}

bool MdnsResponder::registerService(const string &instance, const string &type, uint16_t port,
                                    const map<string, string> &txt)
{
    NG_D(MdnsResponder);
    return d->registerService(instance, type, port, txt);
}

bool MdnsResponder::unregisterService(const string &instance, const string &type)
{
    NG_D(MdnsResponder);
    return d->unregisterService(instance, type);
}

bool MdnsResponder::registerHost(const string &name, const vector<HostAddress> &addresses)
{
    NG_D(MdnsResponder);
    return d->registerHost(name, addresses);
}

bool MdnsResponder::unregisterHost(const string &name)
{
    NG_D(MdnsResponder);
    return d->unregisterHost(name);
}

string MdnsResponder::errorString() const
{
    NG_D(const MdnsResponder);
    return d->error;
}

MdnsResponderPrivate::MdnsResponderPrivate(MdnsResponder *q, HostAddress::NetworkLayerProtocol p)
    : q_ptr(q)
    , proto(p)
    , opened(false)
    , defaultHost("qtng-mdns.local.")
{
}

MdnsResponderPrivate::~MdnsResponderPrivate() { }

bool MdnsResponderPrivate::open(const HostAddress &addr, uint16_t port)
{
    if (opened) {
        return true;
    }
    socket = make_shared<Socket>(proto, Socket::UdpSocket);
    socket->setOption(Socket::AddressReusable, true);
    if (!socket->bind(addr, port)) {
        error = "UDP bind failed: " + socket->errorString();
        socket.reset();
        return false;
    }
    if (port == kMdnsPort) {
        HostAddress group;
        group.setAddress(proto == HostAddress::IPv6Protocol ? kMdnsV6Group : kMdnsV4Group);
        socket->joinMulticastGroup(group);  // best effort; LAN discovery only
    }
    workers = make_unique<CoroutineGroup>();
    workers->spawnWithName("mdns-responder-recv", [this] { recvLoop(); });
    opened = true;
    return true;
}

void MdnsResponderPrivate::close()
{
    if (!opened && !socket) {
        return;
    }
    opened = false;
    if (socket) {
        socket->abort();
    }
    if (workers) {
        workers->killall(true);
        workers.reset();
    }
    socket.reset();
}

void MdnsResponderPrivate::recvLoop()
{
    while (opened && socket) {
        HostAddress from;
        uint16_t fromPort = 0;
        string data = socket->recvfrom(65535, &from, &fromPort);
        if (data.empty()) {
            if (!opened) {
                break;
            }
            Coroutine::msleep(10);
            continue;
        }
        DnsMessage msg;
        if (!msg.parse(data)) {
            continue;
        }
        if ((msg.flags & 0x8000) != 0) {
            continue;  // responses are ignored
        }
        handleQuery(msg, from, fromPort);
    }
}

bool MdnsResponderPrivate::registerService(const string &instance, const string &type, uint16_t port,
                                           const map<string, string> &txt)
{
    const string itype = normalizeDnsName(type);
    if (instance.empty()) {
        return false;
    }
    vector<RegisteredService> &list = services[itype];
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].instance == instance) {
            return false;  // duplicate
        }
    }
    RegisteredService svc;
    svc.instance = instance;
    svc.type = itype;
    svc.host = defaultHost;
    svc.port = port;
    svc.txt = txt;
    list.push_back(svc);
    return true;
}

bool MdnsResponderPrivate::unregisterService(const string &instance, const string &type)
{
    const string itype = normalizeDnsName(type);
    map<string, vector<RegisteredService>>::iterator it = services.find(itype);
    if (it == services.end()) {
        return false;
    }
    for (size_t i = 0; i < it->second.size(); ++i) {
        if (it->second[i].instance == instance) {
            it->second.erase(it->second.begin() + static_cast<ptrdiff_t>(i));
            if (it->second.empty()) {
                services.erase(it);
            }
            return true;
        }
    }
    return false;
}

bool MdnsResponderPrivate::registerHost(const string &name, const vector<HostAddress> &addresses)
{
    if (addresses.empty()) {
        return false;
    }
    hosts[normalizeDnsName(name)] = addresses;
    return true;
}

bool MdnsResponderPrivate::unregisterHost(const string &name)
{
    map<string, vector<HostAddress>>::iterator it = hosts.find(normalizeDnsName(name));
    if (it == hosts.end()) {
        return false;
    }
    hosts.erase(it);
    return true;
}

void MdnsResponderPrivate::addServiceRecords(DnsMessage *resp, const RegisteredService &svc) const
{
    const string instanceFull = svc.instance + "." + svc.type;

    DnsResourceRecord srv;
    srv.name = instanceFull;
    srv.type = DnsTypeSRV;
    srv.klass = DnsClassIN;
    srv.ttl = 120;
    srv.priority = 0;
    srv.weight = 0;
    srv.port = svc.port;
    srv.target = svc.host;
    resp->additional.push_back(srv);

    DnsResourceRecord txt;
    txt.name = instanceFull;
    txt.type = DnsTypeTXT;
    txt.klass = DnsClassIN;
    txt.ttl = 120;
    txt.rdata = encodeTxtRecord(svc.txt);
    resp->additional.push_back(txt);

    const string hostName = normalizeDnsName(svc.host);
    map<string, vector<HostAddress>>::const_iterator hit = hosts.find(hostName);
    if (hit != hosts.end()) {
        for (size_t i = 0; i < hit->second.size(); ++i) {
            const HostAddress &addr = hit->second[i];
            DnsResourceRecord a;
            a.name = hostName;
            a.klass = DnsClassIN;
            a.ttl = 120;
            if (addr.isIPv4()) {
                a.type = DnsTypeA;
                bool ok = false;
                const uint32_t ip = addr.toIPv4Address(&ok);
                char b[4];
                ngToBigEndian(ok ? ip : 0u, b);
                a.rdata.assign(b, 4);
            } else {
                a.type = DnsTypeAAAA;
                const IPv6Address v6 = addr.toIPv6Address();
                a.rdata.assign(reinterpret_cast<const char *>(v6.data()), 16);
            }
            resp->additional.push_back(a);
        }
    }
}

void MdnsResponderPrivate::handleQuery(const DnsMessage &query, const HostAddress &from,
                                       uint16_t fromPort)
{
    DnsMessage resp;
    resp.id = query.id;
    resp.flags = 0x8400;  // QR=1, AA=1
    bool answered = false;
    for (size_t qi = 0; qi < query.questions.size(); ++qi) {
        const DnsQuestion &q = query.questions[qi];
        const string qname = normalizeDnsName(q.name);
        if (q.type == DnsTypePTR) {
            map<string, vector<RegisteredService>>::const_iterator it = services.find(qname);
            if (it == services.end()) {
                continue;
            }
            for (size_t i = 0; i < it->second.size(); ++i) {
                const RegisteredService &svc = it->second[i];
                DnsResourceRecord ptr;
                ptr.name = qname;
                ptr.type = DnsTypePTR;
                ptr.klass = DnsClassIN;
                ptr.ttl = 120;
                ptr.target = svc.instance + "." + qname;
                resp.answers.push_back(ptr);
                addServiceRecords(&resp, svc);
                answered = true;
            }
        } else if (q.type == DnsTypeSRV || q.type == DnsTypeTXT) {
            for (map<string, vector<RegisteredService>>::const_iterator it = services.begin();
                 it != services.end(); ++it) {
                for (size_t i = 0; i < it->second.size(); ++i) {
                    const RegisteredService &svc = it->second[i];
                    const string instanceFull = svc.instance + "." + it->first;
                    if (!dnsNameMatches(instanceFull, qname)) {
                        continue;
                    }
                    if (q.type == DnsTypeSRV) {
                        DnsResourceRecord srv;
                        srv.name = instanceFull;
                        srv.type = DnsTypeSRV;
                        srv.klass = DnsClassIN;
                        srv.ttl = 120;
                        srv.priority = 0;
                        srv.weight = 0;
                        srv.port = svc.port;
                        srv.target = svc.host;
                        resp.answers.push_back(srv);
                    } else {
                        DnsResourceRecord txt;
                        txt.name = instanceFull;
                        txt.type = DnsTypeTXT;
                        txt.klass = DnsClassIN;
                        txt.ttl = 120;
                        txt.rdata = encodeTxtRecord(svc.txt);
                        resp.answers.push_back(txt);
                    }
                    answered = true;
                }
            }
        } else if (q.type == DnsTypeA || q.type == DnsTypeAAAA) {
            map<string, vector<HostAddress>>::const_iterator it = hosts.find(qname);
            if (it == hosts.end()) {
                continue;
            }
            for (size_t i = 0; i < it->second.size(); ++i) {
                const HostAddress &addr = it->second[i];
                DnsResourceRecord a;
                a.name = qname;
                a.klass = DnsClassIN;
                a.ttl = 120;
                if (q.type == DnsTypeA && addr.isIPv4()) {
                    a.type = DnsTypeA;
                    bool ok = false;
                    const uint32_t ip = addr.toIPv4Address(&ok);
                    char b[4];
                    ngToBigEndian(ok ? ip : 0u, b);
                    a.rdata.assign(b, 4);
                } else if (q.type == DnsTypeAAAA && addr.protocol() == HostAddress::IPv6Protocol) {
                    a.type = DnsTypeAAAA;
                    const IPv6Address v6 = addr.toIPv6Address();
                    a.rdata.assign(reinterpret_cast<const char *>(v6.data()), 16);
                } else {
                    continue;
                }
                resp.answers.push_back(a);
                answered = true;
            }
        }
    }
    if (answered && socket) {
        socket->sendto(resp.encode(), from, fromPort);
    }
}

}  // namespace qtng
