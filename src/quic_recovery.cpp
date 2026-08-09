#include "qtng/private/quic_p.h"

#include <algorithm>
#include <set>

using namespace std;

namespace qtng {

void QuicLossRecovery::onPacketSent(const QuicSentPacket &pkt)
{
    m_sent[pkt.space][pkt.pn] = pkt;
    if (pkt.inFlight) {
        m_bytesInFlight += pkt.raw.size();
    }
}

void QuicLossRecovery::onAckReceived(QuicPacketNumberSpace space, const QuicFrame &ack, vector<string> *lostPackets)
{
    uint64_t largest = ack.largestAcknowledged;
    set<uint64_t> acked;
    for (uint64_t i = 0; i <= ack.firstAckRange; ++i) {
        acked.insert(largest - i);
    }
    uint64_t cursor = largest - ack.firstAckRange - 1;
    for (const QuicAckRange &r : ack.ackRanges) {
        cursor -= (r.gap + 1);
        for (uint64_t i = 0; i <= r.ackRangeLength; ++i) {
            acked.insert(cursor - i);
        }
        cursor -= r.ackRangeLength;
    }
    auto &mapRef = m_sent[space];
    for (uint64_t pn : acked) {
        auto it = mapRef.find(pn);
        if (it == mapRef.end()) {
            continue;
        }
        if (it->second.inFlight) {
            m_bytesInFlight -= min(m_bytesInFlight, it->second.raw.size());
            it->second.inFlight = false;
        }
        mapRef.erase(it);
    }
    if (largest >= 2) {
        const uint64_t lossThresh = largest - 2;
        vector<uint64_t> toLose;
        for (auto &kv : mapRef) {
            if (kv.first <= lossThresh && kv.second.inFlight) {
                toLose.push_back(kv.first);
            }
        }
        for (uint64_t pn : toLose) {
            if (lostPackets) {
                lostPackets->push_back(mapRef[pn].raw);
            }
            m_bytesInFlight -= min(m_bytesInFlight, mapRef[pn].raw.size());
            mapRef.erase(pn);
        }
    }
    if (largest > m_largestAcked[space]) {
        m_largestAcked[space] = largest;
    }
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

}  // namespace qtng
