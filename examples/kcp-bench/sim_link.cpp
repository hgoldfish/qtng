#include "sim_link.h"

#include <algorithm>
#include <cstring>

#include "qtng/private/udp_p.h"
#include "qtng/utils/datetime.h"

using namespace std;

namespace bench {

namespace {

inline uint64_t nowUs()
{
    return static_cast<uint64_t>(qtng::utils::DateTime::currentMSecsSinceEpoch()) * 1000ull;
}

}  // namespace

SimLink::SimLink(const string &path, shared_ptr<SimConfig> config, uint32_t rngSeed)
    : m_path(path)
    , m_config(std::move(config))
    , m_rng(rngSeed)
{
}

SimLink::~SimLink() = default;

void SimLink::setPeer(SimLink *peer)
{
    m_peer = peer;
}

void SimLink::enqueueToPeerLocked(string payload, uint64_t dueUs)
{
    SimLink *target = m_peer;
    SimLink::Packet packet;
    packet.bytes = std::move(payload);
    packet.from = m_path;
    target->m_inbound.emplace(dueUs, std::move(packet));
    target->m_notEmpty.set();
}

int32_t SimLink::sendto(const char *data, int32_t size, const qtng::DatagramPath &)
{
    if (m_closed.load() || !m_peer) {
        return -1;
    }
    if (size < 0) {
        return -1;
    }

    const SimConfig &cfg = *m_config;
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const uint64_t now = nowUs();

    // The peer's mutex guards both its inbound queue and its serialization cursor,
    // so every sender to the same target is serialized (that is the bottleneck).
    std::lock_guard<std::mutex> lock(m_peer->m_mutex);

    if (cfg.lossRate > 0.0 && unit(m_rng) < cfg.lossRate) {
        ++m_packetsDropped;
        // Drop-on-full UDP semantics: report success so KcpStream keeps going.
        return size;
    }

    uint64_t departure = now;
    if (cfg.bandwidthBytesPerSec > 0 && size > 0) {
        const uint64_t serviceUs = static_cast<uint64_t>(
                (static_cast<double>(size) * 1000000.0) / static_cast<double>(cfg.bandwidthBytesPerSec)) + 1ull;
        departure = std::max(now, m_peer->m_nextFreeUs) + serviceUs;
        m_peer->m_nextFreeUs = departure;
    }

    uint64_t extraUs = 0;
    if (cfg.jitterMs > 0) {
        std::uniform_int_distribution<uint32_t> jitter(0, cfg.jitterMs);
        extraUs += static_cast<uint64_t>(jitter(m_rng)) * 1000ull;
    }
    if (cfg.reorderRate > 0.0 && unit(m_rng) < cfg.reorderRate) {
        const uint32_t extraMs = cfg.reorderExtraMs > 0 ? cfg.reorderExtraMs : 1u;
        std::uniform_int_distribution<uint32_t> reorder(1, extraMs);
        extraUs += static_cast<uint64_t>(reorder(m_rng)) * 1000ull;
    }

    const uint64_t due = departure
            + static_cast<uint64_t>(cfg.oneWayDelayMs) * 1000ull + extraUs;

    string payload(data, static_cast<size_t>(size));
    if (cfg.corruptRate > 0.0 && !payload.empty() && unit(m_rng) < cfg.corruptRate) {
        std::uniform_int_distribution<size_t> pick(0, payload.size() - 1);
        const size_t idx = pick(m_rng);
        payload[idx] = static_cast<char>(payload[idx] ^ 0x5a);
    }

    // Duplicate first (it needs a copy); then move the original into the queue
    // so the common, non-duplicated send copies the payload only once.
    if (cfg.duplicateRate > 0.0 && unit(m_rng) < cfg.duplicateRate) {
        enqueueToPeerLocked(payload, due + 500ull);
    }
    enqueueToPeerLocked(std::move(payload), due);

    return size;
}

int32_t SimLink::recvfrom(char *data, int32_t size, qtng::DatagramPath *who)
{
    while (true) {
        uint32_t waitMs = UINT32_MAX;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const uint64_t now = nowUs();
            multimap<uint64_t, Packet>::iterator it = m_inbound.begin();
            if (it != m_inbound.end() && it->first <= now) {
                Packet packet = std::move(it->second);
                m_inbound.erase(it);
                const int32_t n = std::min<int32_t>(size, static_cast<int32_t>(packet.bytes.size()));
                if (n > 0 && data) {
                    memcpy(data, packet.bytes.data(), static_cast<size_t>(n));
                }
                if (who) {
                    *who = qtng::DatagramPath(packet.from);
                }
                return n;
            }
            if (m_closed.load()) {
                return -1;
            }
            if (it != m_inbound.end()) {
                const uint64_t deltaUs = it->first - now;
                waitMs = static_cast<uint32_t>((deltaUs + 999ull) / 1000ull);
                if (waitMs == 0) {
                    waitMs = 1;
                }
            }
            // Clear before waiting so a sendto that lands in this window re-sets it.
            // sendto() holds this same mutex, so no wakeup can slip through.
            m_notEmpty.clear();
        }
        // Waiting happens outside the lock so a plain std::mutex is never held
        // across a coroutine yield (that could block the whole OS thread).
        m_notEmpty.tryWait(waitMs);
    }
}

void SimLink::close()
{
    m_closed.store(true);
    m_notEmpty.set();
}

void SimLink::abort()
{
    close();
}

bool SimLink::isValid() const
{
    return !m_closed.load();
}

int32_t SimLink::recvfrom(std::vector<qtng::Datagram> *datagrams)
{
    if (!datagrams || datagrams->empty()) {
        return -1;
    }
    const size_t maxCount = datagrams->size();
    while (true) {
        uint32_t waitMs = UINT32_MAX;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const uint64_t now = nowUs();
            size_t count = 0;
            int32_t total = 0;
            multimap<uint64_t, Packet>::iterator it = m_inbound.begin();
            // Drain every datagram already due, without waiting in between, so a
            // whole burst costs one wakeup. Stop at the caller's capacity.
            while (it != m_inbound.end() && it->first <= now && count < maxCount) {
                qtng::Datagram &datagram = (*datagrams)[count];
                Packet packet = std::move(it->second);
                it = m_inbound.erase(it);
                const int32_t n = std::min<int32_t>(datagram.len,
                                                    static_cast<int32_t>(packet.bytes.size()));
                if (n > 0 && datagram.data) {
                    memcpy(datagram.data, packet.bytes.data(), static_cast<size_t>(n));
                }
                datagram.len = n;
                const qtng::UdpDatagramPath udp(qtng::DatagramPath(packet.from));
                datagram.addr = udp.address();
                datagram.port = udp.port();
                total += n;
                ++count;
            }
            if (count > 0) {
                datagrams->resize(count);
                return total;
            }
            if (m_closed.load()) {
                datagrams->resize(0);
                return -1;
            }
            if (it != m_inbound.end()) {
                const uint64_t deltaUs = it->first - now;
                waitMs = static_cast<uint32_t>((deltaUs + 999ull) / 1000ull);
                if (waitMs == 0) {
                    waitMs = 1;
                }
            }
            // Clear before waiting so a sendto that lands in this window re-sets it.
            m_notEmpty.clear();
        }
        m_notEmpty.tryWait(waitMs);
    }
}

}  // namespace bench
