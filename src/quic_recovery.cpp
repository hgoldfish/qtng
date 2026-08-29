#include "qtng/private/quic_p.h"

#include <algorithm>
#include <set>

using namespace std;

namespace qtng {

// --- QuicRttStats (RFC 9002 §5.1-5.3) ---

void QuicRttStats::onAckSample(uint64_t timeSentUs, uint64_t ackDelayUs, uint64_t nowUs)
{
    if (timeSentUs == 0 || nowUs < timeSentUs) {
        return;
    }
    uint64_t latest = nowUs - timeSentUs;
    // Per RFC 9002 §5.3, only subtract ack_delay when it is less than the RTT sample.
    if (ackDelayUs < latest) {
        latest -= min(ackDelayUs, maxAckDelayUs);
    }
    latestRttUs = latest;
    if (minRttUs == 0 || latest < minRttUs) {
        minRttUs = latest;
    }
    if (smoothedRttUs == 0) {
        smoothedRttUs = latest;
        rttvarUs = latest / 2;
    } else {
        const uint64_t delta = smoothedRttUs > latest ? smoothedRttUs - latest : latest - smoothedRttUs;
        rttvarUs = 3 * rttvarUs / 4 + delta / 4;
        smoothedRttUs = 7 * smoothedRttUs / 8 + latest / 8;
    }
}

uint64_t QuicRttStats::ptoUs() const
{
    // RFC 9002 §6.2: PTO = smoothed_rtt + max(4*rttvar, granularity) + max_ack_delay.
    uint64_t base = 4 * rttvarUs;
    if (base < 1000) {
        base = 1000;  // granularity: 1ms
    }
    return smoothedRttUs + base + maxAckDelayUs;
}

// --- QuicLossRecovery ---

void QuicLossRecovery::onPacketSent(const QuicSentPacket &pkt)
{
    m_sent[pkt.space][pkt.pn] = pkt;
    if (pkt.pn > m_largestSent[pkt.space]) {
        m_largestSent[pkt.space] = pkt.pn;
    }
    if (pkt.timeSentUs > m_lastSentTimeUs[pkt.space]) {
        m_lastSentTimeUs[pkt.space] = pkt.timeSentUs;
    }
    if (pkt.inFlight) {
        m_bytesInFlight += pkt.raw.size();
    }
}

void QuicLossRecovery::onAckReceived(QuicPacketNumberSpace space, const QuicFrame &ack, uint64_t nowUs,
                                     vector<QuicSentPacket> *ackedPackets, vector<string> *lostPackets)
{
    // RFC 9000 §19.3.1: an ACK whose largest acknowledged exceeds the largest
    // packet we ever sent in this space is invalid; clamp so the ranges below
    // cannot wrap or drive a gigantic loop.
    uint64_t largest = min(ack.largestAcknowledged, m_largestSent[space]);
    auto &mapRef = m_sent[space];

    // RFC 9002 §5.1: sample RTT only when the ACK newly acknowledges a larger PN.
    const bool newLargest = largest > m_largestAcked[space];
    if (newLargest) {
        auto it = mapRef.find(largest);
        if (it != mapRef.end()) {
            m_rtt.onAckSample(it->second.timeSentUs, ack.ackDelay, nowUs);
            m_ptoCount[space] = 0;  // RFC 9002 §6.2.1: reset on new RTT sample
        }
    }

    // Expand the ack ranges into an explicit set of acknowledged PNs. The
    // range lengths come from the wire and are attacker-controlled; every
    // walk is clamped so it cannot underflow or iterate unboundedly.
    // Semantics (RFC 9000 §19.3.1): the next range's highest PN is
    // prevRangeLow - gap - 2.
    set<uint64_t> acked;
    const uint64_t firstRange = min(ack.firstAckRange, largest);
    for (uint64_t i = 0; i <= firstRange; ++i) {
        acked.insert(largest - i);
    }
    uint64_t cursor = largest - firstRange;  // lowest acked PN of the first range
    for (const QuicAckRange &r : ack.ackRanges) {
        if (r.gap + 2 > cursor) {
            break;  // would underflow below 0
        }
        cursor -= (r.gap + 2);  // highest acked PN of this range
        const uint64_t rangeLen = min(r.ackRangeLength, cursor);
        for (uint64_t i = 0; i <= rangeLen; ++i) {
            acked.insert(cursor - i);
        }
        cursor -= rangeLen;  // lowest acked PN of this range
        if (cursor == 0) {
            break;
        }
    }

    vector<QuicSentPacket> newlyAcked;
    for (uint64_t pn : acked) {
        auto it = mapRef.find(pn);
        if (it == mapRef.end()) {
            continue;
        }
        if (it->second.inFlight) {
            m_bytesInFlight -= min(m_bytesInFlight, it->second.raw.size());
            it->second.inFlight = false;
        }
        newlyAcked.push_back(it->second);
        mapRef.erase(it);
    }
    if (ackedPackets && !newlyAcked.empty()) {
        sort(newlyAcked.begin(), newlyAcked.end(),
             [](const QuicSentPacket &a, const QuicSentPacket &b) { return a.pn < b.pn; });
        ackedPackets->insert(ackedPackets->end(), newlyAcked.begin(), newlyAcked.end());
    }

    if (largest > m_largestAcked[space]) {
        m_largestAcked[space] = largest;
    }

    // Detect lost packets using both thresholds (RFC 9002 §6.1).
    vector<QuicSentPacket> lost;
    detectLostPackets(space, nowUs, &lost);
    for (const QuicSentPacket &p : lost) {
        if (lostPackets) {
            lostPackets->push_back(p.raw);
        }
    }
}

void QuicLossRecovery::detectLostPackets(QuicPacketNumberSpace space, uint64_t nowUs,
                                         vector<QuicSentPacket> *lost)
{
    auto &mapRef = m_sent[space];
    const uint64_t largestAcked = m_largestAcked[space];

    // Find the newest in-flight packet and its send time for the time threshold.
    uint64_t newestInFlightPn = 0;
    uint64_t newestTimeSentUs = 0;
    for (const auto &kv : mapRef) {
        if (kv.second.inFlight && kv.first > newestInFlightPn) {
            newestInFlightPn = kv.first;
            newestTimeSentUs = kv.second.timeSentUs;
        }
    }

    uint64_t lossDelayUs = 0;
    if (m_rtt.smoothedRttUs > 0) {
        // RFC 9002 §6.1.2: loss_delay = 9/8 * max(latest_rtt, smoothed_rtt), min 1ms.
        uint64_t maxRtt = max(m_rtt.latestRttUs, m_rtt.smoothedRttUs);
        lossDelayUs = 9 * maxRtt / kTimeThresholdDenominator;
        if (lossDelayUs < 1000) {
            lossDelayUs = 1000;
        }
    }

    uint64_t earliestLostUs = 0;
    for (auto &kv : mapRef) {
        if (!kv.second.inFlight) {
            continue;
        }
        bool isLost = false;
        // Packet threshold: 3 packets below largest acked.
        if (largestAcked >= kPacketThreshold && kv.first <= largestAcked - kPacketThreshold) {
            isLost = true;
        } else if (lossDelayUs > 0 && newestTimeSentUs > 0 && nowUs >= newestTimeSentUs + lossDelayUs
                   && kv.first < newestInFlightPn) {
            // Time threshold: packets below the newest in-flight packet that have
            // been outstanding longer than loss_delay are declared lost.
            isLost = true;
        }
        if (isLost) {
            m_bytesInFlight -= min(m_bytesInFlight, kv.second.raw.size());
            kv.second.inFlight = false;
            if (lost) {
                lost->push_back(kv.second);
            }
        } else if (lossDelayUs > 0 && newestTimeSentUs > 0 && newestInFlightPn > 0) {
            // Remember when the time threshold would declare the newest packet lost.
            const uint64_t lossTime = newestTimeSentUs + lossDelayUs;
            if (lossTime > nowUs && (earliestLostUs == 0 || lossTime < earliestLostUs)) {
                earliestLostUs = lossTime;
            }
        }
    }
    m_earliestLostTimeUs[space] = earliestLostUs;
}

vector<string> QuicLossRecovery::packetsToRetransmitOnPto(QuicPacketNumberSpace space) const
{
    vector<string> out;
    auto it = m_sent.find(space);
    if (it == m_sent.end()) {
        return out;
    }
    for (const auto &kv : it->second) {
        if (kv.second.inFlight && kv.second.ackEliciting) {
            out.push_back(kv.second.raw);
        }
    }
    return out;
}

size_t QuicLossRecovery::bytesInFlight() const
{
    return m_bytesInFlight;
}

size_t QuicLossRecovery::bytesInFlight(QuicPacketNumberSpace space) const
{
    size_t total = 0;
    auto it = m_sent.find(space);
    if (it == m_sent.end()) {
        return 0;
    }
    for (const auto &kv : it->second) {
        if (kv.second.inFlight) {
            total += kv.second.raw.size();
        }
    }
    return total;
}

bool QuicLossRecovery::hasInFlight(QuicPacketNumberSpace space) const
{
    auto it = m_sent.find(space);
    if (it == m_sent.end()) {
        return false;
    }
    for (const auto &kv : it->second) {
        if (kv.second.inFlight) {
            return true;
        }
    }
    return false;
}

uint64_t QuicLossRecovery::largestAcked(QuicPacketNumberSpace space) const
{
    return m_largestAcked[space];
}

uint64_t QuicLossRecovery::largestSent(QuicPacketNumberSpace space) const
{
    return m_largestSent[space];
}

uint64_t QuicLossRecovery::lastSentTimeUs(QuicPacketNumberSpace space) const
{
    return m_lastSentTimeUs[space];
}

void QuicLossRecovery::detectLostOnAlarm(QuicPacketNumberSpace space, uint64_t nowUs,
                                         vector<QuicSentPacket> *lost)
{
    detectLostPackets(space, nowUs, lost);
    // Clear the alarm: the detector has recomputed m_earliestLostTimeUs.
    m_earliestLostTimeUs[space] = 0;
}

uint64_t QuicLossRecovery::ptoTimeoutUs(QuicPacketNumberSpace space) const
{
    // RFC 9002 §6.2.1: when no RTT sample exists yet, use a 1s floor.
    uint64_t base = 0;
    if (m_rtt.smoothedRttUs > 0) {
        base = m_rtt.ptoUs();
    } else {
        base = 1000 * 1000;
    }
    // Exponential backoff: PTO * 2^pto_count.
    const uint64_t backoff = 1ull << min<uint64_t>(m_ptoCount[space], 8);
    return base * backoff;
}

void QuicLossRecovery::onPtoExpired(QuicPacketNumberSpace space)
{
    ++m_ptoCount[space];
}

void QuicLossRecovery::resetPtoCounters()
{
    for (int i = 0; i < 3; ++i) {
        m_ptoCount[i] = 0;
    }
}

uint64_t QuicLossRecovery::earliestLostTimeUs(QuicPacketNumberSpace space) const
{
    return m_earliestLostTimeUs[space];
}

}  // namespace qtng
