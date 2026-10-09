#ifndef KCP_BENCH_SIM_LINK_H
#define KCP_BENCH_SIM_LINK_H

// In-memory DatagramLink with a deterministic, seedable fault model.
//
// Two SimLinks are wired as a bidirectional pair (A.peer == B, B.peer == A).
// sendto() on one link schedules the datagram for delivery to the peer; recvfrom()
// on the peer blocks (yields) until the scheduled time arrives.
//
// The fault model covers the parts of a real path that matter for KCP:
//   - one-way propagation delay and jitter
//   - a serialization bottleneck (bandwidthBytesPerSec), which is what produces
//     queueing delay / bufferbloat when a flush dumps a whole window at once
//   - independent loss, reorder, duplication and corruption probabilities
//
// All randomness comes from a per-link std::mt19937, seeded through the SimLink
// constructor (the two ends of a pair need distinct seeds, so the seed cannot
// live on the shared SimConfig), so a given fault pattern can be replayed from
// its seed. Wall-clock driven loops are not bit-reproducible, but the loss /
// reorder / corruption decisions for a seed are.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>

#include "qtng/locks.h"
#include "qtng/udp.h"

namespace bench {

struct SimConfig
{
    std::uint32_t oneWayDelayMs = 0;
    std::uint32_t jitterMs = 0;
    double lossRate = 0.0;        // [0,1): packet is dropped
    double reorderRate = 0.0;     // [0,1): packet gets extra delay
    std::uint32_t reorderExtraMs = 0;
    double duplicateRate = 0.0;   // [0,1): packet is delivered twice
    double corruptRate = 0.0;     // [0,1): one payload byte is flipped
    std::uint64_t bandwidthBytesPerSec = 0;  // 0 == unthrottled serialization
};

class SimLink : public qtng::DatagramLink
{
public:
    SimLink(const std::string &path, std::shared_ptr<SimConfig> config, std::uint32_t rngSeed);
    ~SimLink() override;

    // Wire the peer. Must be called on both links before traffic starts.
    void setPeer(SimLink *peer);

    std::int32_t recvfrom(char *data, std::int32_t size, qtng::DatagramPath *who) override;
    std::int32_t sendto(const char *data, std::int32_t size, const qtng::DatagramPath &who) override;
    // Batch receive. Requires the link's path to be address-shaped
    // ("127.0.0.1:port"), so a Datagram's addr/port round-trips through
    // UdpDatagramPath exactly like a real UDP link.
    std::int32_t recvfrom(std::vector<qtng::Datagram> *datagrams) override;
    void close() override;
    void abort() override;
    bool isValid() const override;

    const std::string &path() const { return m_path; }
    std::uint64_t packetsDropped() const { return m_packetsDropped.load(); }

private:
    struct Packet
    {
        std::string bytes;
        std::string from;
    };

    // Requires m_peer->m_mutex held by the caller. Takes the payload by value so
    // the common send can move it into the queued packet instead of copying.
    void enqueueToPeerLocked(std::string payload, std::uint64_t dueUs);

    SimLink *m_peer = nullptr;
    std::string m_path;
    std::shared_ptr<SimConfig> m_config;

    std::mutex m_mutex;
    // Packets waiting to be delivered to *this* link, ordered by delivery time.
    std::multimap<std::uint64_t, Packet> m_inbound;
    // Serialization cursor: the earliest time the link is free for the next frame.
    // Only advanced by the peer's sendto (which owns this lock while enqueueing).
    std::uint64_t m_nextFreeUs = 0;
    qtng::Event m_notEmpty;
    std::atomic<bool> m_closed{false};
    std::mt19937 m_rng;

    std::atomic<std::uint64_t> m_packetsDropped{0};
};

}  // namespace bench

#endif  // KCP_BENCH_SIM_LINK_H
