#ifndef QTNG_MDNS_P_H
#define QTNG_MDNS_P_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/coroutine_utils.h"
#include "qtng/locks.h"
#include "qtng/mdns.h"
#include "qtng/socket.h"

namespace qtng {

static const std::uint16_t kMdnsPort = 5353;
static const char kMdnsV4Group[] = "224.0.0.251";
static const char kMdnsV6Group[] = "ff02::fb";

enum DnsRecordType {
    DnsTypeA = 1,
    DnsTypePTR = 12,
    DnsTypeTXT = 16,
    DnsTypeAAAA = 28,
    DnsTypeSRV = 33,
};

enum DnsClass {
    DnsClassIN = 1,
};

struct DnsQuestion
{
    DnsQuestion()
        : type(0)
        , klass(DnsClassIN)
    {
    }
    std::string name;
    std::uint16_t type;
    std::uint16_t klass;
};

// Resource record. For PTR the target is in `target`; for SRV the 6-byte
// priority/weight/port prefix is split out and the target is in `target`.
// A/AAAA/TXT and unknown types keep raw bytes in `rdata`.
struct DnsResourceRecord
{
    DnsResourceRecord()
        : type(0)
        , klass(DnsClassIN)
        , ttl(0)
        , priority(0)
        , weight(0)
        , port(0)
    {
    }
    std::string name;
    std::uint16_t type;
    std::uint16_t klass;
    std::uint32_t ttl;
    std::string rdata;  // raw bytes (A/AAAA/TXT/unknown)
    std::string target;  // decoded name for PTR/SRV rdata
    std::uint16_t priority;
    std::uint16_t weight;
    std::uint16_t port;
};

struct DnsMessage
{
    DnsMessage()
        : id(0)
        , flags(0)
    {
    }

    bool parse(const std::string &packet);
    std::string encode() const;

    std::uint16_t id;
    std::uint16_t flags;
    std::vector<DnsQuestion> questions;
    std::vector<DnsResourceRecord> answers;
    std::vector<DnsResourceRecord> authority;
    std::vector<DnsResourceRecord> additional;
};

// Name compression encoder: shares suffixes with previously written names via
// 0xC0 pointers. Appends to the caller-provided buffer, whose current size is
// the reference point for offsets.
class DnsNameEncoder
{
public:
    explicit DnsNameEncoder(std::string *out)
        : out_(out)
    {
    }
    void writeName(const std::string &name);

private:
    std::string *out_;
    std::map<std::string, std::size_t> seen_;  // lowercased suffix -> offset
};

// DNS / mDNS helpers.
std::string normalizeDnsName(const std::string &name);  // lowercase + trailing dot
bool dnsNameMatches(const std::string &a, const std::string &b);
std::string firstDnsLabel(const std::string &name);  // label before the first dot

// TXT record helpers (RFC 6763 §6).
std::map<std::string, std::string> parseTxtRecord(const std::string &rdata);
std::string encodeTxtRecord(const std::map<std::string, std::string> &txt);

struct RegisteredService
{
    RegisteredService()
        : port(0)
    {
    }
    std::string instance;
    std::string type;   // e.g. "_http._tcp.local."
    std::string host;   // SRV target, e.g. "host.local."
    std::uint16_t port;
    std::map<std::string, std::string> txt;
};

class MdnsBrowserPrivate
{
public:
    MdnsBrowserPrivate(MdnsBrowser *q, HostAddress::NetworkLayerProtocol proto);
    ~MdnsBrowserPrivate();

    bool open(std::uint16_t port);
    void close();

    std::vector<MdnsService> browse(const std::string &type, float timeoutSecs);
    std::vector<HostAddress> resolve(const MdnsService &service, float timeoutSecs);
    std::vector<HostAddress> lookup(const std::string &name, float timeoutSecs);

    std::vector<DnsResourceRecord> queryRecords(const std::string &name, std::uint16_t type,
                                                float timeoutSecs);
    DnsMessage queryOne(const std::string &name, std::uint16_t type, float timeoutSecs);
    std::uint16_t nextQueryId();

    void recvLoop();

    MdnsBrowser *q_ptr;
    HostAddress::NetworkLayerProtocol proto;
    std::shared_ptr<Socket> socket;
    std::unique_ptr<CoroutineGroup> workers;
    std::map<std::uint16_t, std::shared_ptr<ValueEvent<DnsMessage>>> waiters;
    RLock waitersLock;
    bool opened;
    bool serverSet;
    HostAddress serverAddr;
    std::uint16_t serverPort;
    std::string error;
    std::uint16_t queryCounter;
};

class MdnsResponderPrivate
{
public:
    MdnsResponderPrivate(MdnsResponder *q, HostAddress::NetworkLayerProtocol proto);
    ~MdnsResponderPrivate();

    bool open(const HostAddress &addr, std::uint16_t port);
    void close();

    void recvLoop();
    void handleQuery(const DnsMessage &query, const HostAddress &from, std::uint16_t fromPort);

    bool registerService(const std::string &instance, const std::string &type, std::uint16_t port,
                         const std::map<std::string, std::string> &txt);
    bool unregisterService(const std::string &instance, const std::string &type);
    bool registerHost(const std::string &name, const std::vector<HostAddress> &addresses);
    bool unregisterHost(const std::string &name);

    void addServiceRecords(DnsMessage *resp, const RegisteredService &svc) const;

    MdnsResponder *q_ptr;
    HostAddress::NetworkLayerProtocol proto;
    std::shared_ptr<Socket> socket;
    std::unique_ptr<CoroutineGroup> workers;
    bool opened;
    std::string error;
    std::string defaultHost;  // "host.local."
    std::map<std::string, std::vector<RegisteredService>> services;  // key: service type
    std::map<std::string, std::vector<HostAddress>> hosts;  // key: host name
};

}  // namespace qtng

#endif  // QTNG_MDNS_P_H
