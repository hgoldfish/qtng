#include "qtng/quic.h"

#include <algorithm>
#include <limits>

using namespace std;

namespace qtng {

class QuicRenoCongestionControlPrivate
{
public:
    explicit QuicRenoCongestionControlPrivate(size_t maxDatagramSize)
        : m_mss(maxDatagramSize > 0 ? maxDatagramSize : 1200)
        , m_cwnd(initialCwnd())
        , m_ssthresh(numeric_limits<size_t>::max())
    {
    }

    size_t initialCwnd() const
    {
        // RFC 9002 §7.2: min(10 * mss, max(2 * mss, 14720)).
        const size_t a = 10 * m_mss;
        const size_t b = max(2 * m_mss, static_cast<size_t>(14720));
        return min(a, b);
    }

    size_t m_mss;
    size_t m_cwnd;
    size_t m_ssthresh;
};

QuicRenoCongestionControl::QuicRenoCongestionControl(size_t maxDatagramSize)
    : d_ptr(new QuicRenoCongestionControlPrivate(maxDatagramSize))
{
}

QuicRenoCongestionControl::~QuicRenoCongestionControl()
{
    delete d_ptr;
}

size_t QuicRenoCongestionControl::congestionWindow() const
{
    NG_D(const QuicRenoCongestionControl);
    return d->m_cwnd;
}

bool QuicRenoCongestionControl::canSend(size_t bytesInFlight) const
{
    NG_D(const QuicRenoCongestionControl);
    return bytesInFlight < d->m_cwnd;
}

void QuicRenoCongestionControl::onPacketSent(size_t bytesInFlight, size_t packetSize)
{
    (void) bytesInFlight;
    (void) packetSize;
}

void QuicRenoCongestionControl::onAckReceived(size_t bytesAcked, size_t bytesInFlight)
{
    NG_D(QuicRenoCongestionControl);
    if (bytesAcked == 0) {
        return;
    }
    if (d->m_cwnd < d->m_ssthresh) {
        // Slow start: cwnd += acked bytes (RFC 9002 §7.2.1).
        d->m_cwnd += bytesAcked;
    } else {
        // Congestion avoidance: cwnd += mss * acked / cwnd (RFC 9002 §7.2.2).
        const size_t increment = (d->m_mss * bytesAcked) / d->m_cwnd;
        d->m_cwnd += max<size_t>(increment, 1);
    }
}

void QuicRenoCongestionControl::onLossDetected(size_t lostBytes, size_t bytesInFlight)
{
    NG_D(QuicRenoCongestionControl);
    (void) lostBytes;
    // RFC 9002 §7.3: ssthresh = max(bytes_in_flight / 2, 2 * mss); cwnd = ssthresh.
    d->m_ssthresh = max(bytesInFlight / 2, 2 * d->m_mss);
    d->m_cwnd = d->m_ssthresh;
}

void QuicRenoCongestionControl::onPersistentCongestion()
{
    NG_D(QuicRenoCongestionControl);
    // RFC 9002 §7.6: cwnd = 2 * mss (minimum congestion window).
    d->m_cwnd = 2 * d->m_mss;
    d->m_ssthresh = d->m_cwnd;
}

}  // namespace qtng
