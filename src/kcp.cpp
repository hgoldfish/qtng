#include "qtng/kcp.h"
#include "qtng/private/udp_p.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtng/coroutine_utils.h"
#include "qtng/network_interface.h"
#include "qtng/private/socket_p.h"
#include "qtng/random.h"
#include "qtng/socket_utils.h"
#include "qtng/utils/datetime.h"
#include "qtng/utils/logging.h"
#include "qtng/utils/string_utils.h"
#include "./kcp/ikcp.h"
#include "./kcp/tuner_budget.h"

using namespace std;

NG_LOGGER("qtng.kcp_stream");

namespace qtng {

const char PACKET_TYPE_UNCOMPRESSED_DATA = 0x01;
const char PACKET_TYPE_CREATE_MULTIPATH = 0x02;
const char PACKET_TYPE_CLOSE = 0X03;
const char PACKET_TYPE_KEEPALIVE = 0x04;

// ikcp segment commands (see IKCP_CMD_* in ikcp.c). They share the unified cmd byte
// (wire offset 0) with KcpStream / upper-layer types.
//
// Zero-copy recv layout (doReceive / doAccept -> handleDatagram):
//
//   buf:  [ headroom: 4 bytes ][ wire: cmd | payload... ]
//          ^                    ^
//          buf[0]               buf[kIkcpConvHeadroom]  <-- recvfrom writes here
//                               |
//                               wire pointer passed to handleDatagram
//
// DatagramLink delivers [cmd][payload...] with no leading ikcp conv. The 4-byte
// headroom lets handleDatagram synthesize a zero conv in place for ikcp_input
// (protocol v2) instead of allocating and memcpy'ing a new segment.
// cmd 0x51-0x54 are the upstream KCP segment commands; 0x55 is the mKCP
// compact ACK frame added in src/kcp/ikcp.c.
const uint8_t PACKET_TYPE_KCP_PUSH = 81;  // 0x51
const uint8_t PACKET_TYPE_KCP_ACK = 82;   // 0x52
const uint8_t PACKET_TYPE_KCP_WASK = 83;  // 0x53
const uint8_t PACKET_TYPE_KCP_WINS = 84;  // 0x54
const uint8_t PACKET_TYPE_KCP_ACKN = 85;  // 0x55, compact ack frame (mKCP)

// Size of the writable prefix reserved before every DatagramLink wire payload in
// MasterKcpStreamPrivate recv buffers. Must stay in sync with handleDatagram,
// which may write a zero conv at (buf - kIkcpConvHeadroom).
static constexpr size_t kIkcpConvHeadroom = 4;

// KcpStream convention: a datagram on the wire is capped at 64 KiB. A receive
// slot is budgeted at exactly that, with the kIkcpConvHeadroom scratch prefix
// reserved inside the budget, so the slot's wire capacity is
// (kMaxDatagramSize - kIkcpConvHeadroom). That still covers the largest payload
// a real UDP datagram can carry (65507 bytes), so nothing a link delivers is
// truncated in practice.
static constexpr size_t kMaxDatagramSize = 1024 * 64;

static bool isIkcpCommand(uint8_t cmd)
{
    return cmd == PACKET_TYPE_KCP_PUSH || cmd == PACKET_TYPE_KCP_ACK || cmd == PACKET_TYPE_KCP_WASK
            || cmd == PACKET_TYPE_KCP_WINS || cmd == PACKET_TYPE_KCP_ACKN;
}

//#define DEBUG_PROTOCOL 1

class SlaveKcpStreamPrivate;
static constexpr uint32_t kDefaultMtu = 1400;
static constexpr uint64_t kDefaultSendBufferLimit = 16ull * 1024ull * 1024ull;  // 16 MiB
static constexpr uint32_t kInitialSendBudgetSegs = 256;                          // former Internet waterLine
static constexpr uint32_t kSegOverheadBytes = 72;                                // ikcp overhead + path framing budget
static constexpr size_t kReorderSampleCap = 32;

// Datagrams drained per batched link->recvfrom() call. One kMaxDatagramSize
// (64 KiB) slice per slot in a single scratch block, so a MasterKcpStreamPrivate
// holds kRecvBatchSize * 64KiB (~2 MiB at 32) for its whole receive lifetime.
// Larger batches cut recvmmsg syscalls on bulk transfers; smaller batches save
// memory on connections that only trickle. Tunable here on purpose.
static constexpr size_t kRecvBatchSize = 32;

// BBRv1-flavoured send-budget controller (see KcpTuner). Capacity comes from a
// bandwidth estimate plus a propagation-delay floor; loss is never read as a
// capacity signal (see stepKcpSendBudgetBbr for the loss policy).

// BtlBw is the maximum of the delivery-rate samples over a recent window, not
// an average, so a single low period cannot drag the estimate under the path's
// capacity. That is only half the story: rate = window / RTT still makes the BDP
// a fixed point of the window, and on a low-RTT path (srttMin of a few ms) the
// window is the binding constraint, so bdpSegs tracks whatever window is in use
// instead of growing past it. kBwWindow bounds how many periods the maximum
// looks back over.
static constexpr size_t kBwWindow = 10;

// Startup's ramp factor: Startup doubles toward bdpSegs * this until the
// bandwidth estimate stops growing, and Drain then clamps back to bdpSegs
// (kBbrDrainGain). Both read bdpSegs, and bdpSegs is derived from the achieved
// delivery rate, which the window itself sets -- so on a low-RTT path the
// feedback bites: srttMin of a few ms against the ~200ms sampling period implies
// a BDP below the window in use, so the target lands under the current window and
// the rate falls with it. ProbeBW is the state that escapes this, because its
// window comes from a ceiling that is not derived from bdpSegs at all (see
// stepKcpProbeCeiling); that is where the up-probe lives, and raising this
// constant is not the way to fix a window that will not grow. BBRv1 uses 2.0
// here too. See kcp-bench README section P5 for the measurements.
static constexpr double kBbrCwndGain = 2.0;
static constexpr double kBbrDrainGain = 1.0;

// Startup exits when the windowed-max bandwidth fails to grow by this factor
// for kBbrFullBwRounds periods (tcp_bbr.c bbr_check_full_bw_reached).
static constexpr double kBbrBwGrowThresh = 1.25;
static constexpr uint32_t kBbrFullBwRounds = 3;

// Floor for the Tuner's send budget, and the window ProbeRTT drains to. There is
// no floor inside ikcp itself: ikcp_wndsize clamps snd_wnd only from above and
// ikcp_send never consults it. The bound comes from here -- the stepper never
// returns less than this, and applySendWindow floors the peer's rmt_wnd term at
// the same value. Because ProbeRTT zeroes the loss deduction and clamps the
// budget to this, the probe's window is exactly this, which is what makes the
// "drained" exit test in runTuner satisfiable. Loss only ever deducts the
// segments it actually lost, so it cannot push the tracked budget below this.
static constexpr uint32_t kBbrMinSegs = 8;

// ProbeRTT drains in-flight data for kBbrProbeRttDurationMs, at most once per
// kBbrProbeRttIntervalMs, so the propagation-delay floor is re-measured on an
// empty queue instead of a standing one. BBRv1 computes a small probe window
// from its own gain; here the drain target is kBbrMinSegs, the floor the rest of
// the Tuner also bottoms out at. The same constant is the "drained" exit test,
// so target and test cannot drift apart and leave the probe unable to exit.
static constexpr uint64_t kBbrProbeRttDurationMs = 200;
static constexpr uint64_t kBbrProbeRttIntervalMs = 10000;

// Counts every real heap allocation made for ikcp segments. A pooled reuse does
// not count; only a pool miss that has to call malloc does. Exposed through
// qtng::kcpSegmentOsAllocationCount() so the benchmark can show the pool's effect.
static std::atomic<uint64_t> g_segmentOsAllocs{0};

// Per-KcpStream pool for ikcp segments.
//
// ikcp allocates one segment per data packet in each direction (ikcp_send and
// ikcp_input), so a bulk transfer churns the allocator hard. KcpStream owns one
// of these and installs it through ikcp_set_segment_allocator(), making the pool
// strictly per-kcp: no cross-stream sharing and no lock.
//
// Every allocation carries a small header recording its true size, so a freed
// block can be matched against the pooled class (the full-size mss segment) and
// recycled; other sizes pass straight through to malloc/free. The class size is
// set once mtu/mss are known and refreshed when mtu changes (a stale class would
// hand out undersized buffers), which setClassSize() does by draining.
class KcpSegmentPool
{
public:
    KcpSegmentPool() = default;
    ~KcpSegmentPool() { drain(); }
    NG_DISABLE_COPY(KcpSegmentPool)

    void setClassSize(size_t bytes)
    {
        if (bytes != m_classSize) {
            drain();
            m_classSize = bytes;
        }
    }

    void *allocate(size_t size)
    {
        if (size == m_classSize && !m_free.empty()) {
            void *base = m_free.back();
            m_free.pop_back();
            return static_cast<char *>(base) + sizeof(Header);
        }
        void *base = malloc(sizeof(Header) + size);
        if (!base) {
            return nullptr;
        }
        g_segmentOsAllocs.fetch_add(1);
        reinterpret_cast<Header *>(base)->size = size;
        return static_cast<char *>(base) + sizeof(Header);
    }

    void deallocate(void *ptr)
    {
        if (!ptr) {
            return;
        }
        Header *header = reinterpret_cast<Header *>(static_cast<char *>(ptr) - sizeof(Header));
        if (header->size == m_classSize && m_free.size() < kFreeLimit) {
            m_free.push_back(header);
            return;
        }
        free(header);
    }

    static void *allocThunk(void *user, size_t size)
    {
        return static_cast<KcpSegmentPool *>(user)->allocate(size);
    }

    static void freeThunk(void *user, void *ptr)
    {
        static_cast<KcpSegmentPool *>(user)->deallocate(ptr);
    }

private:
    // alignas(max_align_t): the payload handed to ikcp must keep the alignment
    // malloc guarantees, so size the header to a full alignment unit.
    struct alignas(std::max_align_t) Header
    {
        size_t size;
    };
    static constexpr size_t kFreeLimit = 2048;

    void drain()
    {
        for (size_t i = 0; i < m_free.size(); ++i) {
            free(m_free[i]);
        }
        m_free.clear();
    }

    std::vector<void *> m_free;
    size_t m_classSize = 0;
};

// BBRv1 controller states. Startup ramps the window multiplicatively until the
// bandwidth estimate stops growing; Drain then pulls in-flight data back down to
// the BDP; ProbeBW holds cwnd_gain, keeping the window above the BDP so the path
// stays probed; ProbeRTT periodically drains the queue to re-measure the
// propagation delay.
enum class BbrState {
    Startup = 0,
    Drain,
    ProbeBw,
    ProbeRtt,
};

// Windowed maximum of delivery-rate samples. BBR keeps the maximum over a recent
// window (tcp_bbr.c bbr_update_bw) instead of an average, because an average
// sits below the path's capacity by construction and a window derived from it
// can never reach the real BDP.
struct BwSampler {
    double samples[kBwWindow] = {};
    uint64_t stamps[kBwWindow] = {};
    size_t next = 0;
    double maxBps = 0;

    void record(double bps, uint64_t now)
    {
        samples[next] = bps;
        stamps[next] = now;
        next = (next + 1) % kBwWindow;
    }

    // Recompute the maximum over samples newer than maxAgeMs. Once every sample
    // has aged out there is no bandwidth evidence left, so the maximum is zero
    // and the window falls back to the floor. Periods that skip record() are the
    // app-limited ones, where the stepper refuses to shrink, so this only bites
    // when a non-app-limited path genuinely stops delivering.
    void refresh(uint64_t now, uint64_t maxAgeMs)
    {
        double best = 0;
        for (size_t i = 0; i < kBwWindow; ++i) {
            if (stamps[i] == 0 || now < stamps[i] || now - stamps[i] > maxAgeMs) {
                continue;
            }
            if (samples[i] > best) {
                best = samples[i];
            }
        }
        maxBps = best;
    }
};

struct KcpTuner {
    uint64_t lastSampleTs = 0;
    uint32_t lastSndUna = 0;
    uint32_t lastLossRto = 0;
    uint32_t lastSentSegs = 0;
    uint32_t lastFastResend = 16;
    uint32_t inflateStreak = 0;
    double lossRate = 0;
    uint32_t srttMin = 0;
    // Wall-clock stamp of the newest srttMin minimum. BBR's RTprop is a minimum
    // over a 10s window; a minimum older than that is allowed to decay.
    uint64_t srttMinStamp = 0;
    uint32_t reorderUs[kReorderSampleCap] = {};
    size_t reorderCount = 0;
    size_t reorderNext = 0;
    // Receive direction. Kept apart from the send sampler: that one sizes the
    // send budget, and a bulk downlink must not inflate it.
    uint32_t lastRcvNxt = 0;

    // BBRv1 controller state.
    BbrState state = BbrState::Startup;
    BwSampler sendBw;
    BwSampler rcvBw;
    // Startup exit bookkeeping (tcp_bbr.c bbr_check_full_bw_reached): the
    // bandwidth the current growth round started from, and how many consecutive
    // periods failed to beat it by kBbrBwGrowThresh.
    double fullBwBaseline = 0;
    uint32_t fullBwCount = 0;
    // ProbeRTT scheduling, and the smallest RTT seen while drained (the fresh
    // propagation-delay measurement). The budget itself needs no saving across
    // the probe: ProbeBW recomputes it from the ceiling, and ProbeRTT always
    // exits into ProbeBW (see runTuner).
    uint64_t lastProbeRtt = 0;
    uint64_t probeRttUntil = 0;
    uint32_t probeRttMinSrtt = 0;
    // ProbeBW's window ceiling: the only up-probe, and deliberately independent
    // of bdpSegs. Loss never moves it (see stepKcpProbeCeiling).
    uint32_t probeCeiling = 0;
};

class KcpStreamPrivate
{
public:
    KcpStreamPrivate(KcpStream *q, uint32_t sessionId = 0, uint32_t mtu = kDefaultMtu);
    virtual ~KcpStreamPrivate();
public:
    virtual Socket::SocketError getError() const = 0;
    virtual string getErrorString() const = 0;
    virtual bool isValid() const = 0;
public:
    virtual KcpStream *accept() = 0;
    virtual KcpStream *accept(const DatagramPath &remote) = 0;
    virtual bool connect(const DatagramPath &remote) = 0;
    virtual bool close(bool force) = 0;
    virtual bool listen(int backlog) = 0;
public:
    void applyFixedPolicy(uint32_t mtu);
    void recalculateMemoryCap();
    void applySendWindow();
    void runTuner(uint64_t now);
    KcpStreamStats snapshotStats() const;
    int32_t send(const char *data, int32_t size, bool all);
    int32_t recv(char *data, int32_t size, bool all);
    int32_t peek(char *data, int32_t size);
    // Feed one DatagramLink packet into this KcpStream.
    //
    // Contract: `buf` points at the on-wire payload ([cmd][...]), and the caller
    // MUST leave kIkcpConvHeadroom writable bytes immediately before `buf`
    // (see doReceive / doAccept). handleDatagram may write into that headroom
    // (v2: zero conv prefix) or mutate `buf` in place (v1: clear sessionId overlay)
    // so ikcp_input can run without an extra heap copy.
    // `remote` is the peer path for this datagram; CLOSE is ignored unless it matches
    // the recorded remotePath (spoofed closes from other paths are dropped).
    bool handleDatagram(char *buf, uint32_t len, const DatagramPath &remote);
    void updateKcp();
    void updateStatus();
    void doUpdate();
    virtual int32_t rawSend(const char *data, int32_t size) = 0;

    string makeDataPacket(const char *data, int32_t size);
    string makeShutdownPacket(uint32_t sessionId);
    string makeKeepalivePacket();
    string makeMultiPathPacket(uint32_t sessionId);
    void negotiateVersion(uint8_t peerVersion);
public:
    KcpStream * const q_ptr;
    NG_DECLARE_PUBLIC(KcpStream)
public:
    CoroutineGroup *operations;
    string errorString;
    Socket::SocketState state;
    Socket::SocketError error;

    Event sendingQueueNotFull;
    Event sendingQueueEmpty;
    Event receivingQueueNotEmpty;
    mutable RLock kcpLock;
    Gate forceToUpdate;
    string receivingBuffer;

    const uint64_t zeroTimestamp;
    uint64_t lastActiveTimestamp;
    uint64_t lastKeepaliveTimestamp;
    uint64_t tearDownTime;
    ikcpcb *kcp;
    uint64_t sendBufferLimitBytes;
    uint32_t memoryCapSegs;
    uint32_t sendBudgetSegs;
    uint32_t sessionId;
    uint8_t protocolVersion;
    bool tunerEnabled;
    bool pendingForceClose = false;
    // Set while the "update_kcp" coroutine is alive. updateKcp() runs on every
    // send and every received datagram, and CoroutineGroup::get() is a linear
    // scan with a string compare, so short-circuit with this flag.
    std::atomic<bool> updateRunning{false};
    KcpTuner tuner;

    // Per-kcp ikcp segment pool; installed into kcp via ikcp_set_segment_allocator
    // and re-sized whenever mtu/mss changes.
    KcpSegmentPool segmentPool;

    DatagramPath remotePath;
};


class MasterKcpStreamPrivate : public KcpStreamPrivate
{
public:
    MasterKcpStreamPrivate(shared_ptr<DatagramLink> link, KcpStream *q, uint32_t sessionId);
    virtual ~MasterKcpStreamPrivate() override;
public:
    virtual Socket::SocketError getError() const override;
    virtual string getErrorString() const override;
    virtual bool isValid() const override;
public:
    virtual KcpStream *accept() override;
    virtual KcpStream *accept(const DatagramPath &remote) override;
    virtual bool connect(const DatagramPath &remote) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
public:
    virtual int32_t rawSend(const char *data, int32_t size) override;
public:
    void removeSlave(const string &originalHostAndPort) { receiversByHostAndPort.erase(originalHostAndPort); }
    void removeSlave(uint32_t sessionId) { receiversBySessionId.erase(sessionId); }
    uint32_t nextSessionId();
    void doReceive();
    void doAccept();
    // Handle one datagram whose wire payload sits at `wire` with kIkcpConvHeadroom
    // writable bytes before it. Return false to stop the receive loop (session
    // closed); true to continue with the next datagram.
    bool receiveOneDatagram(char *wire, uint32_t len, const DatagramPath &who);
    bool acceptOneDatagram(char *wire, uint32_t len, const DatagramPath &who);
    bool startReceivingCoroutine();
public:
    map<string, SlaveKcpStreamPrivate *> receiversByHostAndPort;
    map<uint32_t, SlaveKcpStreamPrivate *> receiversBySessionId;
    shared_ptr<DatagramLink> link;
    Queue<KcpStream *> pendingSlaves;
    // rawSend() calls startReceivingCoroutine() and CoroutineGroup::get() is a
    // linear scan with a string compare; keep the spawn state in a flag instead.
    std::atomic<bool> receivingStarted{false};
};

class SlaveKcpStreamPrivate : public KcpStreamPrivate
{
public:
    SlaveKcpStreamPrivate(MasterKcpStreamPrivate *parent, const DatagramPath &remote, KcpStream *q);
    virtual ~SlaveKcpStreamPrivate() override;
public:
    static KcpStream *create(KcpStreamPrivate *d, const DatagramPath &remote);
    static SlaveKcpStreamPrivate *getPrivateHelper(KcpStream *s);
public:
    virtual Socket::SocketError getError() const override;
    virtual string getErrorString() const override;
    virtual bool isValid() const override;
public:
    virtual KcpStream *accept() override;
    virtual KcpStream *accept(const DatagramPath &remote) override;
    virtual bool connect(const DatagramPath &remote) override;
    virtual bool close(bool force) override;
    virtual bool listen(int backlog) override;
public:
    virtual int32_t rawSend(const char *data, int32_t size) override;
public:
    string originalHostAndPort;
    MasterKcpStreamPrivate *parent;
};

KcpStream *SlaveKcpStreamPrivate::create(KcpStreamPrivate *d, const DatagramPath &remote)
{
    return new KcpStream(d, remote);
}

SlaveKcpStreamPrivate *SlaveKcpStreamPrivate::getPrivateHelper(KcpStream *s)
{
    return static_cast<SlaveKcpStreamPrivate *>(s->d_ptr);
}

int kcp_callback(const char *buf, int len, ikcpcb *, void *user)
{
    KcpStreamPrivate *p = static_cast<KcpStreamPrivate *>(user);
    if (!p || !buf || len > 65535) {
        ngWarning() << "kcp_callback got invalid data.";
        return -1;
    }
    int32_t sentBytes;
    int32_t expectedBytes;
    if (p->protocolVersion == KcpStream::Version2) {
        // ikcp outputs [conv][cmd|...]. v2 strips the unused 4-byte conv, so the
        // datagram is the tail of kcp->buffer; send it straight from there instead
        // of copying it into a std::string first.
        if (len < 5) {
            return -1;
        }
        expectedBytes = len - 4;
        sentBytes = p->rawSend(buf + 4, expectedBytes);
    } else {
        const string &packet = p->makeDataPacket(buf, len);
        expectedBytes = static_cast<int32_t>(packet.size());
        sentBytes = p->rawSend(packet.data(), packet.size());
    }
    // Incomplete send (0 / -1 / short write) = link dead. Callers that want
    // UDP-style drop-on-full must still return `size` so we do not tear down.
    if (sentBytes != expectedBytes) {
        if (p->error == Socket::NoError) {
            p->error = Socket::SocketAccessError;
            p->errorString = "can not send udp packet";
        }
#ifdef DEBUG_PROTOCOL
        ngWarning() << "can not send packet.";
#endif
        // Do not close() here: ikcp_update holds kcpLock. close() → killall can
        // switch to waiters that need the same lock (e.g. MultiStream::doSend),
        // and link send paths must not run teardown under that lock.
        p->pendingForceClose = true;
        return -1;
    }
    return sentBytes;
}

KcpStreamPrivate::KcpStreamPrivate(KcpStream *q, uint32_t sessionId, uint32_t mtu)
    : q_ptr(q)
    , operations(new CoroutineGroup)
    , state(Socket::UnconnectedState)
    , error(Socket::NoError)
    , zeroTimestamp(static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch()))
    , lastActiveTimestamp(zeroTimestamp)
    , lastKeepaliveTimestamp(zeroTimestamp)
    , tearDownTime(1000 * 30)
    , sendBufferLimitBytes(kDefaultSendBufferLimit)
    , memoryCapSegs(kInitialSendBudgetSegs)
    , sendBudgetSegs(kInitialSendBudgetSegs)
    , sessionId(sessionId)
    , protocolVersion(KcpStream::Version1)
    , tunerEnabled(true)
{
    kcp = ikcp_create(0, this);
    ikcp_setoutput(kcp, kcp_callback);
    ikcp_set_segment_allocator(kcp, &segmentPool, KcpSegmentPool::allocThunk, KcpSegmentPool::freeThunk);
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
    receivingQueueNotEmpty.clear();
    q_ptr->busy.clear();
    q_ptr->notBusy.set();
    applyFixedPolicy(mtu);
}

KcpStreamPrivate::~KcpStreamPrivate()
{
    delete operations;
    ikcp_release(kcp);
}

void KcpStreamPrivate::applyFixedPolicy(uint32_t mtu)
{
    // Fixed Internet strategy. nocwnd=1 is load-bearing: ikcp only consumes
    // cwnd at ikcp.c:1088 (`if (nocwnd == 0) cwnd = min(cwnd, ...)`). With
    // nc=1 the cwnd path is unused and rate is governed by snd_wnd / rmt_wnd
    // (Tuner). Do NOT change nc back to 0 — on multipath links reorder is
    // treated as loss, cwnd collapses to 1, and dead_link can tear down a
    // healthy connection.
    const uint32_t clampedMtu = max(mtu, 50u);
    // resend=0 disables fast retransmit. Tuner-on keeps the default (16, then
    // Tuner adapts). Tuner-off (SLOW multipath) forces 0: reorder is not loss.
    const int resend = tunerEnabled ? 16 : 0;
    ikcp_nodelay(kcp, /*nodelay=*/1, /*interval=*/10, /*resend=*/resend, /*nc=*/1);
    if (!tunerEnabled) {
        tuner.lastFastResend = 0;
    }
    ikcp_setmtu(kcp, static_cast<int>(clampedMtu));
    ikcp_wndsize(kcp, 1024, 1024);
    // The pool class size tracks mss: full-size segments (the only pooled class)
    // are exactly sizeof(IKCPSEG) + mss.
    segmentPool.setClassSize(sizeof(IKCPSEG) + kcp->mss);
    kcp->rx_minrto = 30;
    kcp->dead_link = 10;
    recalculateMemoryCap();
    applySendWindow();
}

void KcpStreamPrivate::recalculateMemoryCap()
{
    const uint32_t segBytes = kcp->mss + kSegOverheadBytes;
    if (segBytes == 0) {
        memoryCapSegs = kInitialSendBudgetSegs;
        return;
    }
    uint64_t segs = sendBufferLimitBytes / segBytes;
    if (segs < 8) {
        segs = 8;
    }
    if (segs > 0xFFFFu) {
        segs = 0xFFFFu;
    }
    memoryCapSegs = static_cast<uint32_t>(segs);
}

void KcpStreamPrivate::applySendWindow()
{
    // App backpressure (updateStatus) and ikcp flight share the same limit:
    // snd_wnd = min(Tuner budget, memory cap, peer rmt_wnd). rcv_wnd is independent.
    // Must re-run whenever rmt_wnd moves — otherwise a peer window open stays
    // invisible until the next Tuner period (up to ~1s).
    //
    // Loss does not subtract from this: the budget itself is the only window
    // signal, and it is sized from the bandwidth estimate rather than from the
    // loss rate (see KcpTuner and stepKcpSendBudgetBbr).
    const uint32_t sndTarget = min<uint32_t>({0xFFFFu, sendBudgetSegs, memoryCapSegs,
                                              max<uint32_t>(static_cast<uint32_t>(kcp->rmt_wnd), 8u)});
    if (kcp->snd_wnd == sndTarget) {
        return;
    }
    // ikcp_wndsize treats sndwnd==0 as "leave unchanged" (same as rcvwnd).
    // External multipath backpressure needs a real zero so queue→buf stops.
    if (sndTarget == 0) {
        kcp->snd_wnd = 0;
    } else {
        ikcp_wndsize(kcp, static_cast<int>(sndTarget), 0);
    }
}

static uint32_t reorderP95Us(KcpTuner &t)
{
    if (t.reorderCount == 0) {
        return 0;
    }
    uint32_t tmp[kReorderSampleCap];
    const size_t n = t.reorderCount;
    for (size_t i = 0; i < n; ++i) {
        tmp[i] = t.reorderUs[i];
    }
    sort(tmp, tmp + n);
    const size_t idx = min(n - 1, (n * 95) / 100);
    return tmp[idx];
}

void KcpStreamPrivate::runTuner(uint64_t now)
{
    if (!tunerEnabled) {
        return;
    }
    const uint32_t srtt = kcp->rx_srtt > 0 ? static_cast<uint32_t>(kcp->rx_srtt) : 200u;
    const uint32_t period = max(200u, min(1000u, srtt));
    // wall-clock can step backwards; same guard as tearDown/keepalive below.
    // Refresh differentials so a later catch-up does not dump a huge Δuna into a tiny dtMs.
    if (tuner.lastSampleTs != 0 && now <= tuner.lastSampleTs) {
        tuner.lastSampleTs = now;
        tuner.lastSndUna = kcp->snd_una;
        tuner.lastLossRto = kcp->loss_rto;
        tuner.lastSentSegs = kcp->sent_segs;
        tuner.lastRcvNxt = kcp->rcv_nxt;
        return;
    }
    if (tuner.lastSampleTs != 0 && (now - tuner.lastSampleTs) < period) {
        return;
    }

    const uint64_t dtMs = tuner.lastSampleTs == 0 ? period : max<uint64_t>(1, now - tuner.lastSampleTs);
    const uint32_t sndUna = kcp->snd_una;
    const uint32_t deltaUna = tuner.lastSampleTs == 0 ? 0 : (sndUna - tuner.lastSndUna);
    const uint32_t deltaSent = tuner.lastSampleTs == 0 ? 0 : (kcp->sent_segs - tuner.lastSentSegs);
    const uint32_t deltaLoss = tuner.lastSampleTs == 0 ? 0 : (kcp->loss_rto - tuner.lastLossRto);
    const uint32_t segBytes = kcp->mss + kSegOverheadBytes;

    const int waitsndNow = ikcp_waitsnd(kcp);
    const uint32_t wndForLimit = kcp->snd_wnd > 0 ? kcp->snd_wnd : sendBudgetSegs;
    // Empty queue, or this period sent far less than the window allows: the
    // application (or the peer) is not filling the pipe. Do not treat that
    // achieved rate as the path's capacity.
    const bool appLimited = waitsndNow <= 0
            || (wndForLimit > 0 && deltaSent * 4u < wndForLimit);

    // Delivery rate, send direction: record this period's goodput as a BtlBw
    // sample. App-limited periods carry no information about the path, so they
    // are skipped rather than recorded as a low sample.
    if (!appLimited && deltaUna > 0 && dtMs > 0) {
        const double sampleBps = (static_cast<double>(deltaUna) * segBytes * 8.0 * 1000.0)
                / static_cast<double>(dtMs);
        tuner.sendBw.record(sampleBps, now);
    }
    tuner.sendBw.refresh(now, kBwWindow * static_cast<uint64_t>(period));

    if (deltaSent > 0) {
        const double sampleLoss = static_cast<double>(deltaLoss) / static_cast<double>(deltaSent);
        tuner.lossRate = tuner.lastSampleTs == 0 ? sampleLoss : (tuner.lossRate * 7.0 + sampleLoss) / 8.0;
    }
    // Receive-rate evidence. A download-only stream never fills its send queue,
    // so every period looks app-limited and the send sampler stays empty; the
    // receive window then has no way to grow (see the rcv_wnd block below).
    // rcv_nxt only advances on in-order new data, so this is goodput, not wire
    // rate. A windowed maximum for the same reason as the send side.
    const uint32_t rcvNxt = kcp->rcv_nxt;
    const uint32_t deltaRcv = tuner.lastSampleTs == 0 ? 0 : (rcvNxt - tuner.lastRcvNxt);
    if (deltaRcv > 0 && dtMs > 0) {
        const double sampleRcvBps = (static_cast<double>(deltaRcv) * segBytes * 8.0 * 1000.0)
                / static_cast<double>(dtMs);
        tuner.rcvBw.record(sampleRcvBps, now);
    }
    tuner.rcvBw.refresh(now, kBwWindow * static_cast<uint64_t>(period));

    // RTprop floor: a running minimum that ages out. A new low resets the stamp;
    // once the stamp is older than the ProbeRTT interval the minimum is stale,
    // so let it drift up rather than pin a low value the path no longer has.
    // ProbeRTT is the primary re-measurement -- it drains the queue and adopts
    // the RTT it measures there, which may be higher than this floor. The drift
    // is the backstop for a connection that stays app-limited and never probes.
    if (kcp->rx_srtt > 0) {
        const uint32_t cur = static_cast<uint32_t>(kcp->rx_srtt);
        if (tuner.srttMin == 0 || cur < tuner.srttMin) {
            tuner.srttMin = cur;
            tuner.srttMinStamp = now;
        } else if (cur > tuner.srttMin && tuner.srttMinStamp != 0
                   && now - tuner.srttMinStamp >= kBbrProbeRttIntervalMs) {
            tuner.srttMin += max(1u, (cur - tuner.srttMin) / 16);
            tuner.srttMinStamp = now;
        }
    }
    bool haveNewReorder = false;
    if (kcp->reorder_samples > 0) {
        tuner.reorderUs[tuner.reorderNext] = kcp->reorder_us_max;
        tuner.reorderNext = (tuner.reorderNext + 1) % kReorderSampleCap;
        if (tuner.reorderCount < kReorderSampleCap) {
            tuner.reorderCount++;
        }
        kcp->reorder_us_max = 0;
        kcp->reorder_samples = 0;
        haveNewReorder = true;
    }

    const uint32_t srttForBdp = tuner.srttMin > 0 ? tuner.srttMin : max(srtt, 1u);
    const double btlBps = tuner.sendBw.maxBps;
    uint32_t bdpSegs = kBbrMinSegs;
    if (btlBps > 0 && segBytes > 0) {
        const double bdpBytes = btlBps * static_cast<double>(srttForBdp) / 8000.0;
        bdpSegs = max(kBbrMinSegs, static_cast<uint32_t>(bdpBytes / segBytes + 0.999));
    }
    const uint32_t inflight = kcp->snd_nxt - kcp->snd_una;

    // Queuing delay: srtt above the RTprop floor. This is the controller's only
    // congestion signal; loss is deliberately not used to infer capacity (see
    // stepKcpSendBudgetBbr).
    const int32_t rttVar = kcp->rx_rttval > 0 ? kcp->rx_rttval : 0;
    const uint32_t qdelay = (kcp->rx_srtt > 0 && tuner.srttMin > 0
                             && static_cast<uint32_t>(kcp->rx_srtt) > tuner.srttMin)
            ? (static_cast<uint32_t>(kcp->rx_srtt) - tuner.srttMin)
            : 0;
    const uint32_t qThreshold = max(static_cast<uint32_t>(4 * max(rttVar, 1)), max(srttForBdp / 2, 1u));
    bool delayReduce = false;
    if (qdelay > qThreshold) {
        tuner.inflateStreak++;
        if (tuner.inflateStreak >= 2) {
            delayReduce = true;
        }
    } else {
        tuner.inflateStreak = 0;
    }

    // --- BBRv1 state machine: transitions ---------------------------------
    //
    // Startup/Drain/ProbeBW/ProbeRTT replace the old capacity probe, which raised
    // a ceiling by 2x whenever the rate inched up and reset it to zero on any
    // congestion -- a sawtooth between the floor and an overshoot. Congestion now
    // pulls the window back to the BDP instead of discarding the bandwidth
    // estimate, and the window target is always derived from that estimate
    // (below).
    if (tuner.lastProbeRtt == 0) {
        tuner.lastProbeRtt = now;
    }
    if (tuner.state == BbrState::Startup) {
        if (!appLimited && btlBps > 0) {
            // Exit once the windowed maximum stops growing
            // (tcp_bbr.c bbr_check_full_bw_reached).
            if (btlBps >= tuner.fullBwBaseline * kBbrBwGrowThresh) {
                tuner.fullBwBaseline = btlBps;
                tuner.fullBwCount = 0;
            } else if (++tuner.fullBwCount >= kBbrFullBwRounds) {
                tuner.state = BbrState::Drain;
                tuner.fullBwCount = 0;
            }
        }
    } else if (tuner.state == BbrState::Drain) {
        // Drain is done once in-flight data is back down to the BDP.
        if (inflight <= bdpSegs) {
            tuner.state = BbrState::ProbeBw;
            // Seed the up-probe from the BDP this run measured. Done here rather
            // than every period so a ProbeRTT that restores the budget does not
            // also reset what the probe learned.
            if (tuner.probeCeiling == 0) {
                tuner.probeCeiling = static_cast<uint32_t>(min<double>(memoryCapSegs,
                        static_cast<double>(bdpSegs) * kBbrCwndGain + 0.999));
            }
        }
    } else if (tuner.state == BbrState::ProbeBw) {
        if (!appLimited && now - tuner.lastProbeRtt >= kBbrProbeRttIntervalMs) {
            tuner.probeRttMinSrtt = 0;
            tuner.probeRttUntil = now + kBbrProbeRttDurationMs;
            tuner.state = BbrState::ProbeRtt;
        } else {
            // The ceiling is the only up-probe, and it moves on evidence about
            // the queue alone: raise it while the pipe is full and the queue
            // empty, trim it when a queue stands. Loss is not an input -- a
            // lossy path is exactly the one that must keep probing, because the
            // rate-derived BDP it would otherwise fall back on is dragged down
            // by the same loss.
            const bool pipeFull = tuner.probeCeiling > 0 && inflight >= tuner.probeCeiling;
            const bool grow = !appLimited && pipeFull && !delayReduce;
            tuner.probeCeiling = stepKcpProbeCeiling(tuner.probeCeiling, bdpSegs, memoryCapSegs,
                                                    grow, delayReduce);
        }
    } else {  // ProbeRtt: drain, tracking the RTT the drained queue actually shows.
        if (kcp->rx_srtt > 0) {
            const uint32_t cur = static_cast<uint32_t>(kcp->rx_srtt);
            tuner.probeRttMinSrtt = tuner.probeRttMinSrtt == 0 ? cur : min(tuner.probeRttMinSrtt, cur);
        }
        // Done once the drain has run its time and in-flight data is back down
        // to the probe window (kBbrMinSegs -- the same value the drain clamps to
        // below, so this test cannot be unsatisfiable).
        if (now >= tuner.probeRttUntil && inflight <= kBbrMinSegs) {
            tuner.lastProbeRtt = now;
            tuner.srttMinStamp = now;
            // The probe drained the queue precisely to re-measure the
            // propagation delay, so adopt what it measured -- including a
            // higher value. A floor that can only ratchet down keeps qdelay
            // (srtt - srttMin) inflated after a route change raises the path's
            // RTT, which then starves the window at a BDP computed from a stale
            // minimum. A genuinely lower sample still pulls it back down on the
            // next period's minimum.
            if (tuner.probeRttMinSrtt > 0) {
                tuner.srttMin = tuner.probeRttMinSrtt;
            }
            tuner.probeRttMinSrtt = 0;
            tuner.state = BbrState::ProbeBw;
        }
    }

    // --- apply the (possibly just-updated) state to the window -------------
    if (tuner.state == BbrState::ProbeRtt) {
        // A deliberate, temporary drain: clamp the budget straight to the probe
        // window. The next period is ProbeBW, which sizes the budget from the
        // ceiling again, so nothing needs saving across the probe. There is no
        // target here either: the probe size is not derived from the bandwidth
        // estimate, and walking to it would not drain promptly.
        sendBudgetSegs = kBbrMinSegs;
    } else if (tuner.state == BbrState::ProbeBw) {
        // Follow the ceiling directly. Its own step (x5/4 up, x9/10 down) is
        // already the gentle probe, so running it through the stepper's 1/8
        // gap-closing would cascade two rate limiters and the slower one would
        // decide: the ceiling climbs at 25% per period while the budget trails
        // at ~7%, so the up-probe never reaches the path. Nothing to do when
        // app-limited either: the ceiling already holds in that case.
        //
        // The max against bdpSegs is not redundant: the ceiling is only advanced
        // inside this branch, so on the period that ProbeRTT exits into ProbeBw
        // it is stale, and a BDP that grew during the probe must not be
        // undercut.
        sendBudgetSegs = min(memoryCapSegs, max(kBbrMinSegs, max(bdpSegs, tuner.probeCeiling)));
    } else {
        // Drain clamps to the BDP; Startup ramps on the BDP.
        uint32_t targetSegs;
        if (tuner.state == BbrState::Startup && btlBps <= 0) {
            // No bandwidth sample yet: ramp toward the memory cap, like BBR's
            // Startup before the first measurement lands.
            targetSegs = memoryCapSegs;
        } else {
            const double gain = tuner.state == BbrState::Drain ? kBbrDrainGain : kBbrCwndGain;
            targetSegs = static_cast<uint32_t>(min<double>(memoryCapSegs,
                    static_cast<double>(bdpSegs) * gain + 0.999));
        }

        const bool startupGrow = tuner.state == BbrState::Startup;
        sendBudgetSegs = stepKcpSendBudgetBbr(sendBudgetSegs, targetSegs, memoryCapSegs, appLimited,
                                             startupGrow, kBbrMinSegs);
    }

    // Adapt fastresend only when this period produced new reorder evidence,
    // stepping at most ±2 toward the P95-derived target. Gating on the
    // cumulative ring (reorderCount>0) used to keep walking toward a stale
    // near-zero P95 long after reorder stopped.
    // Multipath (tuner off) never reaches here. Single-path adapts fastresend
    // when this period produced new reorder evidence.
    if (haveNewReorder) {
        const uint32_t reorderMs = reorderP95Us(tuner) / 1000u;
        const uint32_t interval = max<uint32_t>(static_cast<uint32_t>(kcp->interval), 1u);
        const uint32_t target = min(32u, max(1u, reorderMs / interval + 1u));
        uint32_t applied = tuner.lastFastResend;
        if (target > tuner.lastFastResend) {
            applied = tuner.lastFastResend + min(2u, target - tuner.lastFastResend);
        } else if (target < tuner.lastFastResend) {
            applied = tuner.lastFastResend - min(2u, tuner.lastFastResend - target);
        }
        if (applied != tuner.lastFastResend) {
            ikcp_nodelay(kcp, -1, -1, static_cast<int>(applied), -1);
            tuner.lastFastResend = applied;
        }
    }

    // Grow rcv_wnd with the receive-direction BDP; never shrink. Shrinking below
    // in-flight capacity deadlocks sequential sendall→recvall once the peer
    // advertises wnd=0.
    //
    // rcv_wnd starts at the fixed 1024 segments from applyFixedPolicy. Sizing it
    // from the send direction alone pinned every download-only stream at those
    // 1024 segments (≈1.3MB at the default MTU), so downlink throughput was capped
    // at 1024 segments / RTT no matter how fast the path was. bdpSegs above is
    // derived from the send sampler alone, which stays empty for a bulk downlink
    // (every period is app-limited, see above), so recompute only when the
    // receive rate is the larger of the two.
    const double rcvRateBps = max(tuner.rcvBw.maxBps, btlBps);
    uint32_t rcvBdpSegs = bdpSegs;
    if (rcvRateBps > btlBps && segBytes > 0) {
        const double rcvBdpBytes = rcvRateBps * static_cast<double>(srttForBdp) / 8000.0;
        rcvBdpSegs = max(8u, static_cast<uint32_t>(rcvBdpBytes / segBytes + 0.999));
    }
    if (rcvRateBps > 0) {
        const uint32_t computed = min(0xFFFFu, max(128u /* IKCP_WND_RCV */,
                                                  min(memoryCapSegs, (rcvBdpSegs * 3) / 2)));
        if (computed > kcp->rcv_wnd && (computed - kcp->rcv_wnd) * 5 >= kcp->rcv_wnd) {
            ikcp_wndsize(kcp, 0, static_cast<int>(computed));
            kcp->probe |= IKCP_ASK_TELL;
        }
    }

    applySendWindow();

    tuner.lastSampleTs = now;
    tuner.lastSndUna = sndUna;
    tuner.lastLossRto = kcp->loss_rto;
    tuner.lastSentSegs = kcp->sent_segs;
    tuner.lastRcvNxt = rcvNxt;
}

KcpStreamStats KcpStreamPrivate::snapshotStats() const
{
    KcpStreamStats s;
    s.sndWnd = kcp ? kcp->snd_wnd : 0;
    s.sendBudgetSegs = sendBudgetSegs;
    s.memoryCapSegs = memoryCapSegs;
    s.rtoResends = kcp ? kcp->xmit : 0;
    s.fastResends = kcp ? kcp->xmit_fast : 0;
    s.fastresend = kcp ? static_cast<uint32_t>(max(kcp->fastresend, 0)) : 0;
    s.lossRate = tuner.lossRate;
    s.deliveryBps = tuner.sendBw.maxBps;
    s.waitsnd = kcp ? static_cast<uint32_t>(ikcp_waitsnd(kcp)) : 0;
    s.sndUna = kcp ? kcp->snd_una : 0;
    s.sndNxt = kcp ? kcp->snd_nxt : 0;
    s.sentSegs = kcp ? kcp->sent_segs : 0;
    s.rmtWnd = kcp ? kcp->rmt_wnd : 0;
    s.rcvWnd = kcp ? kcp->rcv_wnd : 0;
    s.rxSrtt = kcp && kcp->rx_srtt > 0 ? static_cast<uint32_t>(kcp->rx_srtt) : 0;
    s.srttMin = tuner.srttMin;
    s.mss = kcp ? kcp->mss : 0;
    return s;
}

int32_t KcpStreamPrivate::send(const char *data, int32_t size, bool all)
{
    if (size <= 0 || !isValid()) {
        return -1;
    }

    sendingQueueEmpty.clear();

    int count = 0;
    while (count < size) {
        if (state != Socket::ConnectedState) {
            error = Socket::SocketAccessError;
            errorString = "KcpStream is not connected.";
            return -1;
        }
        uint32_t mss = 0;
        uint32_t room = 0;
        {
            ScopedLock<RLock> l(kcpLock);
            applySendWindow();
            // snd_wnd==0 is intentional path backpressure: stop accepting app
            // data so undrainable snd_queue does not accumulate (close() waits
            // on sendingQueueEmpty; queue never moves to buf while wnd is 0).
            const uint32_t sndWnd = kcp->snd_wnd;
            const int waiting = ikcp_waitsnd(kcp);
            mss = kcp->mss;
            if (sndWnd == 0) {
                room = 0;
            } else {
                const uint32_t busyThreshold = sndWnd + max(8u, sndWnd / 5u);
                // updateStatus() only clears sendingQueueNotFull once waitsnd is
                // *past* busyThreshold. Stopping at == leaves the event set, and
                // tryWait returns immediately — sendall then spins until tearDown.
                if (waiting >= 0 && static_cast<uint32_t>(waiting) <= busyThreshold) {
                    room = busyThreshold - static_cast<uint32_t>(waiting) + 1u;
                }
            }
            if (room == 0) {
                // Level-triggered: a set() from when the window was open stays
                // latched after snd_wnd drops to 0. tryWait() would then return
                // without yielding and this coroutine would pin the thread
                // (SIGINT shutdown never runs). Clear under the same lock that
                // observed no room; nothing runs before tryWait() below.
                sendingQueueNotFull.clear();
            }
        }
        if (room == 0) {
            const uint32_t pollMs = max(200u, min(1000u, static_cast<uint32_t>(tearDownTime / 10)));
            const uint64_t waitDeadline =
                    static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch()) + tearDownTime;
            bool ok = false;
            while (!ok) {
                if (state != Socket::ConnectedState) {
                    error = Socket::SocketAccessError;
                    errorString = "KcpStream is not connected.";
                    return count > 0 ? count : -1;
                }
                ok = sendingQueueNotFull.tryWait(pollMs);
                if (ok) {
                    break;
                }
                const uint64_t now = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
                if (now >= waitDeadline) {
                    error = Socket::SocketTimeoutError;
                    errorString = "KcpStream send buffer is full.";
                    return count > 0 ? count : -1;
                }
            }
            continue;
        }
        if (mss == 0) {
            mss = 1;
        }
        // Fill the open window in one pass. send() still returns after one
        // segment; sendall() keeps going until the window or the buffer ends,
        // and only then waits.
        bool failed = false;
        uint32_t filled = 0;
        while (count < size && filled < room) {
            const int32_t nextBlockSize = min<int32_t>(static_cast<int32_t>(mss), size - count);
            int result;
            {
                ScopedLock<RLock> l(kcpLock);
                result = ikcp_send(kcp, data + count, nextBlockSize);
            }
            if (result < 0) {
                failed = true;
                break;
            }
            count += nextBlockSize;
            ++filled;
            if (!all) {
                break;
            }
        }
        updateStatus();
        updateKcp();
        if (failed) {
            ngWarning() << "why this happened?";
            return count > 0 ? count : -1;
        }
        if (!all) {
            return isValid() ? count : -1;
        }
    }
    assert(all);
    return isValid() ? count : -1;
}

int32_t KcpStreamPrivate::recv(char *data, int32_t size, bool all)
{
    while (true) {
        if (state != Socket::ConnectedState) {
            error = Socket::SocketAccessError;
            errorString = "KcpStream is not connected.";
            return -1;
        }
        if (!receivingBuffer.empty()) {
            if (!all || receivingBuffer.size() >= size) {
                int32_t len = static_cast<int32_t>(min(static_cast<size_t>(size), receivingBuffer.size()));
                memcpy(data, receivingBuffer.data(), static_cast<size_t>(len));
                receivingBuffer.erase(0, static_cast<size_t>(len));
                return len;
            }
        }
        const int peeksize = ikcp_peeksize(kcp);
        if (peeksize > 0) {
            if (!all && peeksize <= size) {
                // The next message fits the caller's buffer: let ikcp_recv fill it
                // directly, skipping the temp string + append + memcpy.
                ScopedLock<RLock> l(kcpLock);
                const int readBytes = ikcp_recv(kcp, data, size);
                if (readBytes > 0) {
                    return readBytes;
                }
            }
            string buf(peeksize, '\0');
            int readBytes;
            {
                ScopedLock<RLock> l(kcpLock);
                readBytes = ikcp_recv(kcp, &buf[0], buf.size());
            }
            assert(readBytes == peeksize);
            receivingBuffer.append(buf);
            continue;
        }
        receivingQueueNotEmpty.clear();
        bool ok = receivingQueueNotEmpty.tryWait();
        if (!ok) {
            ngDebug() << "not receivingQueueNotEmpty->tryWait()";
            return -1;
        }
    }
}

int32_t KcpStreamPrivate::peek(char *data, int32_t size)
{
    if (state != Socket::ConnectedState) {
        return -1;
    }
    if (!receivingBuffer.empty()) {
        int32_t len = static_cast<int32_t>(min(static_cast<size_t>(size), receivingBuffer.size()));
        memcpy(data, receivingBuffer.data(), static_cast<size_t>(len));
        return len;
    }
    return 0;
}

bool KcpStreamPrivate::handleDatagram(char *buf, uint32_t len, const DatagramPath &remote)
{
    auto feedIkcp = [this, &remote](const char *segment, uint32_t segmentLen) -> bool {
        int result;
        {
            ScopedLock<RLock> l(kcpLock);
            result = ikcp_input(kcp, segment, static_cast<long>(segmentLen));
        }
        if (result < 0) {
            if (result == -2) {
                ngWarning() << "ikcp_input dropped a segment due to allocation failure.";
            }
#ifdef DEBUG_PROTOCOL
            ngDebug() << "invalid datagram. kcp returns" << result;
#endif
            return true;
        }
        lastActiveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
        receivingQueueNotEmpty.set();
        updateKcp();
        remotePath = remote;
        return true;
    };

    // Wire: [cmd][payload...] (no leading conv). Requires headroom before buf —
    // see kIkcpConvHeadroom and the layout comment at the top of this file.
    if (len < 1) {
        return true;
    }
    const uint8_t cmd = static_cast<uint8_t>(buf[0]);
    if (isIkcpCommand(cmd)) {
        negotiateVersion(KcpStream::Version2);
    } else if (cmd == PACKET_TYPE_UNCOMPRESSED_DATA) {
        negotiateVersion(KcpStream::Version1);
    }
    if (isIkcpCommand(cmd)) {
        // v2: write zero conv into the caller's headroom, then feed
        // [0][0][0][0][cmd][payload...] without allocating a temporary string.
        ngToBigEndian<uint32_t>(0, reinterpret_cast<uint8_t *>(buf - kIkcpConvHeadroom));
        return feedIkcp(buf - kIkcpConvHeadroom, len + static_cast<uint32_t>(kIkcpConvHeadroom));
    }
    switch (cmd) {
    case PACKET_TYPE_UNCOMPRESSED_DATA:
        // Legacy v1: [0x01][ikcp segment; conv carries sessionId overlay].
        // Clear conv in place (caller already read sessionId) and feed from buf+1.
        if (len < 6) {
            return true;
        }
        ngToBigEndian<uint32_t>(0, reinterpret_cast<uint8_t *>(buf + 1));
        return feedIkcp(buf + 1, len - 1);
    case PACKET_TYPE_CREATE_MULTIPATH:
        remotePath = remote;
        if (len >= 5) {
            const uint32_t sid = ngFromBigEndian<uint32_t>(buf + 1);
            if (sid != 0) {
                sessionId = sid;
            }
        }
        return true;
    case PACKET_TYPE_CLOSE:
        if (remote == remotePath) {
            close(true);
            return false;
        }
        // Ignore CLOSE from a path that is not the recorded peer.
        return true;
    case PACKET_TYPE_KEEPALIVE:
        lastActiveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
        remotePath = remote;
        return true;
    default:
        // Unknown / upper-layer cmd: ignore at KcpStream.
        return true;
    }
}

void KcpStreamPrivate::doUpdate()
{
    // in close(), state is set to Socket::UnconnectedState but error = NoError.
    while (state == Socket::ConnectedState || (state == Socket::UnconnectedState && error == Socket::NoError)) {
        uint64_t now = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
        // now and lastActiveTimestamp both are unsigned int, we should check which is larger before apply minus
        // operator to them.
        if (now > lastActiveTimestamp && (now - lastActiveTimestamp > tearDownTime)
            && state == Socket::ConnectedState) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "kcp socket tearDown!";
#endif
            error = Socket::SocketTimeoutError;
            errorString = "KcpStream is timeout.";
            close(true);
            return;
        }
        uint32_t current = static_cast<uint32_t>(now - zeroTimestamp);  // impossible to overflow.
        bool needForceClose = false;
        {
            ScopedLock<RLock> l(kcpLock);

            ikcp_update(kcp, current);  // flush → kcp_callback (must not close under lock)
            needForceClose = pendingForceClose;
            pendingForceClose = false;
            if (kcp->state != 0) {
                error = Socket::SocketTimeoutError;
                errorString = "KcpStream dead link.";
                needForceClose = true;
            } else if (!needForceClose) {
                runTuner(now);
            }
        }
        // close() outside kcpLock: teardown killall/abort must not run while send()
        // waiters are blocked on the same lock.
        if (needForceClose) {
            close(true);
            return;
        }
        if (!(state == Socket::ConnectedState || (state == Socket::UnconnectedState && error == Socket::NoError))) {
            return;
        }

        // now and lastKeepaliveTimestamp both are unsigned int, we should check which is larger before apply minus
        // operator to them.
        if (now > lastKeepaliveTimestamp && (now - lastKeepaliveTimestamp > 1000 * 5)
            && state == Socket::ConnectedState) {
            const string &packet = makeKeepalivePacket();
            if (rawSend(packet.data(), packet.size()) != packet.size()) {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "can not send keep alive packet.";
#endif
                close(true);
                return;
            } else {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "keep alive packet sent.";
#endif
            }
        }

        updateStatus();

        uint32_t ts = ikcp_check(kcp, current);
        uint32_t interval = ts - current;
        if (interval > 0) {
            forceToUpdate.close();
            forceToUpdate.tryWait(interval);  // timeout continue
        }
    }
}

void KcpStreamPrivate::updateKcp()
{
    // replace=false: CoroutineGroup already keeps one "update_kcp". A second
    // call just returns that coroutine; waking forceToUpdate is enough.
    // The updateRunning flag avoids the string-keyed lookup on this hot path
    // (called for every send and every received datagram).
    if (!updateRunning.load()) {
        operations->spawnWithName("update_kcp", [this] {
            doUpdate();
            updateRunning.store(false);
        }, false);
        updateRunning.store(true);
    }
    kcp->updated = 0;
    forceToUpdate.open();
}

void KcpStreamPrivate::updateStatus()
{
    int sendingQueueSize;
    uint32_t sndWnd;
    {
        ScopedLock<RLock> l(kcpLock);
        applySendWindow();
        sendingQueueSize = ikcp_waitsnd(kcp);
        sndWnd = kcp->snd_wnd;
    }
    if (sendingQueueSize <= 0) {
        sendingQueueEmpty.set();
    } else {
        sendingQueueEmpty.clear();
    }
    // snd_wnd==0: never advertise room — even with an empty queue — or send()
    // will see notFull set + room==0 and spin on tryWait.
    if (sndWnd == 0) {
        sendingQueueNotFull.clear();
        q_ptr->busy.set();
        q_ptr->notBusy.clear();
        return;
    }
    if (sendingQueueSize <= 0) {
        sendingQueueNotFull.set();
        q_ptr->busy.clear();
        q_ptr->notBusy.set();
        return;
    }
    const uint32_t busyThreshold = sndWnd + max(8u, sndWnd / 5u);
    if (static_cast<uint32_t>(sendingQueueSize) > busyThreshold) {
        sendingQueueNotFull.clear();
        q_ptr->busy.set();
        q_ptr->notBusy.clear();
    } else if (static_cast<uint32_t>(sendingQueueSize) > sndWnd) {
        q_ptr->busy.set();
        q_ptr->notBusy.clear();
    } else {
        sendingQueueNotFull.set();
        q_ptr->busy.clear();
        q_ptr->notBusy.set();
    }
}

string KcpStreamPrivate::makeDataPacket(const char *data, int32_t size)
{
    if (protocolVersion == KcpStream::Version2) {
        // ikcp outputs [conv][cmd|frg|wnd|...]; strip the unused conv on the wire.
        if (size < 5) {
            return string();
        }
        return string(data + 4, static_cast<size_t>(size - 4));
    }
    string packet(static_cast<size_t>(size) + 1, '\0');
    packet[0] = PACKET_TYPE_UNCOMPRESSED_DATA;
    memcpy(&packet[1], data, static_cast<size_t>(size));
    ngToBigEndian<uint32_t>(this->sessionId, &packet[1]);
    return packet;
}

string KcpStreamPrivate::makeShutdownPacket(uint32_t sessionId)
{
    // Pad to a random length in [5, 64) so control packets are not fixed-size.
    const int size = 5 + static_cast<int>(RandomGenerator::global().bounded(64 - 5));
    string packet = randomBytes(size);
    packet[0] = PACKET_TYPE_CLOSE;
    ngToBigEndian<uint32_t>(sessionId, &packet[1]);
    return packet;
}

string KcpStreamPrivate::makeKeepalivePacket()
{
    const int size = 5 + static_cast<int>(RandomGenerator::global().bounded(64 - 5));
    string packet = randomBytes(size);
    packet[0] = PACKET_TYPE_KEEPALIVE;
    ngToBigEndian<uint32_t>(this->sessionId, &packet[1]);
    return packet;
}

string KcpStreamPrivate::makeMultiPathPacket(uint32_t sessionId)
{
    const int size = 5 + static_cast<int>(RandomGenerator::global().bounded(64 - 5));
    string packet = randomBytes(size);
    packet[0] = PACKET_TYPE_CREATE_MULTIPATH;
    ngToBigEndian<uint32_t>(sessionId, &packet[1]);
    return packet;
}

void KcpStreamPrivate::negotiateVersion(uint8_t peerVersion)
{
    if (peerVersion != KcpStream::Version1 && peerVersion != KcpStream::Version2) {
        return;
    }
    protocolVersion = static_cast<uint8_t>(min(static_cast<uint8_t>(protocolVersion), peerVersion));
    // mKCP: the compact ACK frame (cmd 0x55) only exists on the v2 wire
    // (conv is stripped at the outer framing layer), so it must be enabled
    // and disabled together with the negotiated version. Old / asymmetric
    // peers (v1, or a v2 peer without ackn support) would silently drop or
    // mis-parse 0x55 frames.
    ikcp_ackn_mode(kcp, protocolVersion == KcpStream::Version2);
}

MasterKcpStreamPrivate::MasterKcpStreamPrivate(shared_ptr<DatagramLink> link, KcpStream *q, uint32_t sessionId)
    : KcpStreamPrivate(q, sessionId)
    , link(link)
{
}


MasterKcpStreamPrivate::~MasterKcpStreamPrivate()
{
    MasterKcpStreamPrivate::close(true);
}

Socket::SocketError MasterKcpStreamPrivate::getError() const
{
    if (error != Socket::NoError) {
        return error;
    } else {
        return link->error();
    }
}

string MasterKcpStreamPrivate::getErrorString() const
{
    if (!errorString.empty()) {
        return errorString;
    } else {
        return link->errorString();
    }
}

bool MasterKcpStreamPrivate::isValid() const
{
    return state == Socket::ConnectedState || state == Socket::BoundState || state == Socket::ListeningState;
}


bool MasterKcpStreamPrivate::close(bool force)
{
    receivingStarted.store(false);
    // if `force` is true, must not block. see doUpdate()
    // Never leave Connected→Unconnected without the cleanup below: an early
    // return after the state flip made later close()/destructor skip killall.
    if (state == Socket::UnconnectedState) {
        return true;
    } else if (state == Socket::ConnectedState) {
        state = Socket::UnconnectedState;
        if (!force && error == Socket::NoError) {
            try {
                bool drained = sendingQueueEmpty.isSet();
                if (!drained) {
                    updateKcp();
                    drained = sendingQueueEmpty.tryWait(3000);
                }
                if (drained) {
                    const string &packet = makeShutdownPacket(this->sessionId);
                    rawSend(packet.data(), packet.size());
                }
            } catch (...) {
                // close() can run from a destructor while the current coroutine is being killed.
                // Waiting on the queue would raise the kill exception out of the destructor
                // (noexcept -> terminate), so fall through to the non-blocking cleanup below.
            }
        }
    } else if (state == Socket::ListeningState) {
        state = Socket::UnconnectedState;
        vector<SlaveKcpStreamPrivate *> receivers;
        receivers.reserve(receiversByHostAndPort.size());
        for (const pair<const string, SlaveKcpStreamPrivate *> &item : receiversByHostAndPort) {
            if (item.second) {
                receivers.push_back(item.second);
            }
        }
        this->receiversByHostAndPort.clear();
        if (force) {
            // force-close is called from destructors / doUpdate() and must not block:
            // CoroutineGroup::each() joins, and joining from a coroutine that is being
            // killed raises CoroutineExitException out of a destructor (noexcept -> terminate).
            // SlaveKcpStreamPrivate::close(true) is non-blocking, so close inline.
            for (SlaveKcpStreamPrivate *receiver : receivers) {
                if (receiver) {
                    receiver->close(true);
                }
            }
        } else {
            try {
                CoroutineGroup::each<SlaveKcpStreamPrivate *>([force](SlaveKcpStreamPrivate *receiver) {
                    if (receiver) {
                        receiver->close(force);
                    }
                }, receivers, 10);
            } catch (...) {
                // close() can run from a destructor while the current coroutine is being killed.
                // each() joins, and joining raises the kill exception out of the destructor. The
                // streams are closed here non-blockingly instead; close(true) is idempotent, so
                // streams already closed by the aborted each() are simply skipped.
                for (SlaveKcpStreamPrivate *receiver : receivers) {
                    if (receiver) {
                        receiver->close(true);
                    }
                }
            }
        }
        receiversBySessionId.clear();
    } else {  // BoundState
        state = Socket::UnconnectedState;
        link->abort();
        return true;
    }

    while (!pendingSlaves.isEmpty()) {
        delete pendingSlaves.get();
    }
    pendingSlaves.put(nullptr);

    // connected and listen state would do more cleaning work.
    operations->killall();
    // always kill operations before release resources.
    link->abort();
    // awake all pending recv()/send()
    receivingQueueNotEmpty.set();
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
#ifdef DEBUG_PROTOCOL
    ngDebug() << "MasterKcpStreamPrivate::close() done";
#endif
    return true;
}

bool MasterKcpStreamPrivate::listen(int backlog)
{
    if (state != Socket::BoundState || backlog <= 0) {
        return false;
    }
    state = Socket::ListeningState;
    pendingSlaves.setCapacity(static_cast<uint32_t>(backlog));
    return true;
}

uint32_t MasterKcpStreamPrivate::nextSessionId()
{
    uint32_t id;
    do {
        const string bytes = randomBytes(4);
        memcpy(&id, bytes.data(), sizeof(id));
    } while (receiversBySessionId.find(id) != receiversBySessionId.end());
    return id;
}

void MasterKcpStreamPrivate::doReceive()
{
    // Batch receive. One contiguous heap block holds kRecvBatchSize slots of
    // kMaxDatagramSize bytes each (~2 MiB at 32) — far too big for the stack,
    // and a single allocation beats kRecvBatchSize of them. Each slot reserves
    // kIkcpConvHeadroom inside it in front of the wire payload, so the link
    // writes straight where handleDatagram expects it (see the file-top layout
    // comment) and one recvmmsg fills many slots.
    const size_t recvBufSize = kMaxDatagramSize;
    unique_ptr<char[]> scratch(new (std::nothrow) char[kRecvBatchSize * recvBufSize]);
    if (!scratch) {
        MasterKcpStreamPrivate::close(true);
        return;
    }
    vector<Datagram> datagrams(kRecvBatchSize);
    while (true) {
        // recvfrom shrinks the vector; rebuild it for the next round.
        datagrams.resize(kRecvBatchSize);
        for (size_t i = 0; i < kRecvBatchSize; ++i) {
            datagrams[i].data = scratch.get() + i * recvBufSize + kIkcpConvHeadroom;
            datagrams[i].len = static_cast<int32_t>(recvBufSize - kIkcpConvHeadroom);
        }
        if (link->recvfrom(&datagrams) < 0) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "KcpStream can not receive packet." << link->errorString();
#endif
            MasterKcpStreamPrivate::close(true);
            return;
        }
        for (size_t i = 0; i < datagrams.size(); ++i) {
            const Datagram &datagram = datagrams[i];
            if (datagram.len < 1) {
                continue;
            }
            // Prefer the datagram's own address. A link whose DatagramPath key
            // is opaque reports none, and a connected stream has exactly one
            // peer, so fall back to the path recorded at connect() instead of
            // dropping the session.
            DatagramPath who = UdpDatagramPath(datagram.addr, datagram.port).toPath();
            if (who.isNull()) {
                who = remotePath;
            }
            if (who.isNull()) {
                MasterKcpStreamPrivate::close(true);
                return;
            }
            // datagram.data points at its slot + kIkcpConvHeadroom.
            if (!receiveOneDatagram(datagram.data, static_cast<uint32_t>(datagram.len), who)) {
                return;
            }
        }
    }
}

bool MasterKcpStreamPrivate::receiveOneDatagram(char *wire, uint32_t len, const DatagramPath &who)
{
    const uint8_t cmd = static_cast<uint8_t>(wire[0]);
    if (cmd == PACKET_TYPE_CREATE_MULTIPATH && len >= 5) {
        const uint32_t packetSessionId = ngFromBigEndian<uint32_t>(wire + 1);
        if (packetSessionId == 0) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "the kcp server side returns an invalid packet with zero session id.";
#endif
            return true;
        }
        if (this->sessionId == 0) {
            this->sessionId = packetSessionId;
        } else if (packetSessionId != this->sessionId) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "the kcp server side returns an invalid packet with mismatched session id.";
#endif
            return true;
        }
    } else if (cmd == PACKET_TYPE_UNCOMPRESSED_DATA && len >= 5) {
        const uint32_t packetSessionId = ngFromBigEndian<uint32_t>(wire + 1);
        if (packetSessionId == 0) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "the kcp server side returns an invalid packet with zero session id.";
#endif
            return true;
        }
        if (this->sessionId == 0) {
            this->sessionId = packetSessionId;
        } else if (packetSessionId != this->sessionId) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "the kcp server side returns an invalid packet with mismatched session id.";
#endif
            return true;
        }
    }

    // wire still has kIkcpConvHeadroom writable bytes before it (the scratch).
    return handleDatagram(wire, len, who);
}

void MasterKcpStreamPrivate::doAccept()
{
    // Same contiguous batch + headroom layout as doReceive.
    const size_t recvBufSize = kMaxDatagramSize;
    unique_ptr<char[]> scratch(new (std::nothrow) char[kRecvBatchSize * recvBufSize]);
    if (!scratch) {
        MasterKcpStreamPrivate::close(true);
        return;
    }
    vector<Datagram> datagrams(kRecvBatchSize);
    while (true) {
        datagrams.resize(kRecvBatchSize);
        for (size_t i = 0; i < kRecvBatchSize; ++i) {
            datagrams[i].data = scratch.get() + i * recvBufSize + kIkcpConvHeadroom;
            datagrams[i].len = static_cast<int32_t>(recvBufSize - kIkcpConvHeadroom);
        }
        if (link->recvfrom(&datagrams) < 0) {
#ifdef DEBUG_PROTOCOL
            ngDebug() << "KcpStream can not receive packet." << link->errorString();
#endif
            MasterKcpStreamPrivate::close(true);
            return;
        }
        for (size_t i = 0; i < datagrams.size(); ++i) {
            const Datagram &datagram = datagrams[i];
            if (datagram.len < 1) {
                continue;
            }
            const DatagramPath who = UdpDatagramPath(datagram.addr, datagram.port).toPath();
            if (who.isNull()) {
                // accept() demuxes a new slave by the datagram's own peer, and a
                // listening master has no single recorded path to fall back on
                // (remotePath is only set by connect()). A batch link that
                // cannot report an address per datagram is unsupported here.
                MasterKcpStreamPrivate::close(true);
                return;
            }
            if (!acceptOneDatagram(datagram.data, static_cast<uint32_t>(datagram.len), who)) {
                return;
            }
        }
    }
}

bool MasterKcpStreamPrivate::acceptOneDatagram(char *wire, uint32_t len, const DatagramPath &who)
{
    const uint8_t cmd = static_cast<uint8_t>(wire[0]);
    uint32_t sessionId = 0;
    if (len >= 5) {
        if (cmd == PACKET_TYPE_CREATE_MULTIPATH || cmd == PACKET_TYPE_UNCOMPRESSED_DATA) {
            sessionId = ngFromBigEndian<uint32_t>(wire + 1);
        }
    }
    const string &key = who.key();
    SlaveKcpStreamPrivate *receiver = nullptr;
    map<string, SlaveKcpStreamPrivate *>::iterator hostIt = receiversByHostAndPort.find(key);
    if (hostIt != receiversByHostAndPort.end()) {
        receiver = hostIt->second;
    }
    if (receiver) {
        if (sessionId != 0) {
            if (receiver->sessionId == 0) {
                // only if the slave was created by accept(path), we had zero id.
                // if this sessionId is unique in client. we add it to the receiversBySessionId map.
                // if it is not, say sorry, and disable the multipath feature.
                if (receiversBySessionId.find(sessionId) == receiversBySessionId.end()) {
                    // only happened in the newly accept(path) connections.
                    // or remote create new conn with the same path as old, and the old packet is received.
                    receiver->sessionId = sessionId;
                    receiversBySessionId[sessionId] = receiver;
                }
            } else if (sessionId != receiver->sessionId) {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "the client sent a invalid session id";
#endif
                return true;
            }
        }
        // wire still has kIkcpConvHeadroom writable bytes before it (the scratch).
        // remotePath is updated inside handleDatagram for DATA/KEEPALIVE/CREATE_MULTIPATH.
        if (!receiver->handleDatagram(wire, len, who)) {
            receiversByHostAndPort.erase(receiver->originalHostAndPort);
            receiversBySessionId.erase(receiver->sessionId);
        }
        return true;
    }

    if (sessionId != 0) {  // a multipath packet.
        const map<uint32_t, SlaveKcpStreamPrivate *>::iterator it = receiversBySessionId.find(sessionId);
        receiver = (it != receiversBySessionId.end()) ? it->second : nullptr;
        if (!receiver) {
            // it must be bad packet.
            const string &closePacket = makeShutdownPacket(sessionId);
            if (link->sendto(closePacket.data(), closePacket.size(), who) != static_cast<int32_t>(closePacket.size())) {
                if (error == Socket::NoError) {
                    error = Socket::SocketResourceError;
                    errorString = "KcpStream can not send packet.";
                }
#ifdef DEBUG_PROTOCOL
                ngDebug() << errorString;
#endif
                return false;
            }
        } else {
            assert(sessionId == receiver->sessionId);
            // Same headroom contract as the primary-path handleDatagram call above.
            if (!receiver->handleDatagram(wire, len, who)) {
#ifdef DEBUG_PROTOCOL
                ngDebug() << "can not handle multipath packet.";
#endif
                receiversByHostAndPort.erase(receiver->originalHostAndPort);
                receiversBySessionId.erase(receiver->sessionId);
            }
        }
        return true;
    }

    if (pendingSlaves.size() < pendingSlaves.capacity()) {  // not full. process new connection.
        unique_ptr<KcpStream> slave(SlaveKcpStreamPrivate::create(this, who));
        SlaveKcpStreamPrivate *d = SlaveKcpStreamPrivate::getPrivateHelper(slave.get());
        d->originalHostAndPort = key;
        d->sessionId = nextSessionId();
        // First packet of a new slave; wire still sits after the shared recv headroom.
        if (d->handleDatagram(wire, len, who)) {
            receiversByHostAndPort[key] = d;
            receiversBySessionId[d->sessionId] = d;
            pendingSlaves.put(slave.release());
            const string &multiPathPacket = makeMultiPathPacket(d->sessionId);
            if (link->sendto(multiPathPacket.data(), multiPathPacket.size(), who) != static_cast<int32_t>(multiPathPacket.size())) {
                if (error == Socket::NoError) {
                    error = Socket::SocketResourceError;
                    errorString = "KcpStream can not send packet.";
                }
#ifdef DEBUG_PROTOCOL
                ngDebug() << errorString;
#endif
                return false;
            }
        }
    }
    return true;
}

bool MasterKcpStreamPrivate::startReceivingCoroutine()
{
    if (receivingStarted.load()) {
        return true;
    }
    switch (state) {
    case Socket::UnconnectedState:
    case Socket::BoundState:
    case Socket::ConnectingState:
    case Socket::HostLookupState:
    case Socket::ClosingState:
        return false;
    case Socket::ConnectedState:
        operations->spawnWithName("receiving", [this] {
            doReceive();
            receivingStarted.store(false);
        });
        break;
    case Socket::ListeningState:
        operations->spawnWithName("receiving", [this] {
            doAccept();
            receivingStarted.store(false);
        });
        break;
    }
    receivingStarted.store(true);
    return true;
}

KcpStream *MasterKcpStreamPrivate::accept()
{
    if (state != Socket::ListeningState) {
        return nullptr;
    }
    startReceivingCoroutine();
    return pendingSlaves.get();
}

KcpStream *MasterKcpStreamPrivate::accept(const DatagramPath &remote)
{
    if (state != Socket::ListeningState || remote.isNull()) {
        return nullptr;
    }
    startReceivingCoroutine();
    const string &key = remote.key();
    SlaveKcpStreamPrivate *receiver;
    receiver = receiversByHostAndPort.at(key);
    if (receiver && receiver->isValid()) {
        return nullptr;
    } else {
        unique_ptr<KcpStream> slave(SlaveKcpStreamPrivate::create(this, remote));
        SlaveKcpStreamPrivate *d = SlaveKcpStreamPrivate::getPrivateHelper(slave.get());
        d->originalHostAndPort = key;
        d->updateKcp();
        receiversByHostAndPort[key] = d;
        // the sessionId is generated in server side. accept() is acually a connect().
        // receiversBySessionId[d->sessionId] = d;
        return slave.release();
    }
}


bool MasterKcpStreamPrivate::connect(const DatagramPath &remote)
{
    if ((state != Socket::UnconnectedState && state != Socket::BoundState) || remote.isNull()) {
        return false;
    }
    remotePath = remote;
    state = Socket::ConnectedState;
    // Start pumping the link immediately: a lazily-started receive loop (first
    // rawSend) deadlocks the side that receives before it sends.
    startReceivingCoroutine();
    return true;
}


int32_t MasterKcpStreamPrivate::rawSend(const char *data, int32_t size)
{
    lastKeepaliveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
    startReceivingCoroutine();
    return link->sendto(data, size, remotePath);
}


SlaveKcpStreamPrivate::SlaveKcpStreamPrivate(MasterKcpStreamPrivate *parent, const DatagramPath &remote, KcpStream *q)
    : KcpStreamPrivate(q, 0, parent ? static_cast<uint32_t>(parent->kcp->mtu) : kDefaultMtu)
    , parent(parent)
{
    remotePath = remote;
    state = Socket::ConnectedState;
    if (parent) {
        protocolVersion = parent->protocolVersion;
        // Snapshot, same as MTU: later setTunerEnabled() on the master
        // does not reach slaves already accepted.
        tunerEnabled = parent->tunerEnabled;
        if (!tunerEnabled && kcp) {
            ikcp_nodelay(kcp, -1, -1, 0, -1);
            tuner.lastFastResend = 0;
        }
    }
}

SlaveKcpStreamPrivate::~SlaveKcpStreamPrivate()
{
    SlaveKcpStreamPrivate::close(true);
}

Socket::SocketError SlaveKcpStreamPrivate::getError() const
{
    if (error != Socket::NoError) {
        return error;
    } else {
        if (parent) {
            return parent->link->error();
        } else {
            return Socket::SocketAccessError;
        }
    }
}

string SlaveKcpStreamPrivate::getErrorString() const
{
    if (!errorString.empty()) {
        return errorString;
    } else {
        if (parent) {
            return parent->link->errorString();
        } else {
            return "Invalid socket descriptor";
        }
    }
}

bool SlaveKcpStreamPrivate::isValid() const
{
    return state == Socket::ConnectedState && parent;
}


bool SlaveKcpStreamPrivate::close(bool force)
{
    // if `force` is true, must not block. it is called by doUpdate()
    // Cleanup below is idempotent (guarded by parent). Always run it — do not
    // return after flipping to Unconnected without removeSlave/killall.
    if (state == Socket::ConnectedState) {
        state = Socket::UnconnectedState;
        if (!force && error == Socket::NoError) {
            bool drained = sendingQueueEmpty.isSet();
            if (!drained) {
                updateKcp();
                drained = sendingQueueEmpty.tryWait(3000);
            }
            if (drained) {
                const string &packet = makeShutdownPacket(this->sessionId);
                rawSend(packet.data(), packet.size());
            }
        }
    } else {
        state = Socket::UnconnectedState;
    }
    operations->killall();
    if (parent) {
        parent->removeSlave(originalHostAndPort);
        parent->removeSlave(sessionId);
        parent = nullptr;
    }
    // await all pending recv()/send()
    receivingQueueNotEmpty.set();
    sendingQueueEmpty.set();
    sendingQueueNotFull.set();
    q_ptr->notBusy.set();
    q_ptr->busy.set();
#ifdef DEBUG_PROTOCOL
    ngDebug() << "SlaveKcpStreamPrivate::close() done.";
#endif
    return true;
}

bool SlaveKcpStreamPrivate::listen(int)
{
    return false;
}

KcpStream *SlaveKcpStreamPrivate::accept()
{
    return nullptr;
}

KcpStream *SlaveKcpStreamPrivate::accept(const DatagramPath &)
{
    return nullptr;
}


bool SlaveKcpStreamPrivate::connect(const DatagramPath &)
{
    return false;
}


int32_t SlaveKcpStreamPrivate::rawSend(const char *data, int32_t size)
{
    if (!parent) {
        return -1;
    } else {
        lastKeepaliveTimestamp = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
        return parent->link->sendto(data, size, remotePath);
    }
}

KcpStream::KcpStream(shared_ptr<DatagramLink> link, uint32_t sessionId)
    : d_ptr(new MasterKcpStreamPrivate(link, this, sessionId))
{
}


KcpStream::KcpStream(KcpStreamPrivate *parent, const DatagramPath &remote)
    : d_ptr(new SlaveKcpStreamPrivate(static_cast<MasterKcpStreamPrivate *>(parent), remote, this))
{
}

KcpStream::~KcpStream()
{
    delete d_ptr;
}

void KcpStream::setPacketSize(uint32_t udpPacketSize)
{
    NG_D(KcpStream);
    if (udpPacketSize >= 65535 || udpPacketSize < 50) {
        return;
    }
    ScopedLock<RLock> l(d->kcpLock);
    if (ikcp_waitsnd(d->kcp) > 0) {
        return;
    }
    if (ikcp_setmtu(d->kcp, static_cast<int>(udpPacketSize)) == 0) {
        // waitsnd == 0 above means no segment is in flight, so re-classing the
        // pool (which drains it) cannot strand a live segment.
        d->segmentPool.setClassSize(sizeof(IKCPSEG) + d->kcp->mss);
        d->recalculateMemoryCap();
        d->applySendWindow();
    }
}

uint32_t KcpStream::packetSize() const
{
    NG_D(const KcpStream);
    return d->kcp->mtu;
}

void KcpStream::setSendBufferLimit(uint64_t bytes)
{
    NG_D(KcpStream);
    if (bytes == 0) {
        bytes = kDefaultSendBufferLimit;
    }
    ScopedLock<RLock> l(d->kcpLock);
    d->sendBufferLimitBytes = bytes;
    d->recalculateMemoryCap();
    d->sendBudgetSegs = min(d->sendBudgetSegs, d->memoryCapSegs);
    d->applySendWindow();
}

uint64_t KcpStream::sendBufferLimit() const
{
    NG_D(const KcpStream);
    return d->sendBufferLimitBytes;
}

uint32_t KcpStream::payloadSizeHint() const
{
    NG_D(const KcpStream);
    return d->kcp->mss;
}

KcpStreamStats KcpStream::stats() const
{
    NG_D(const KcpStream);
    ScopedLock<RLock> l(d->kcpLock);
    return d->snapshotStats();
}

uint64_t kcpSegmentOsAllocationCount()
{
    return g_segmentOsAllocs.load();
}

void KcpStream::setTearDownTime(float secs)
{
    NG_D(KcpStream);
    if (secs > 0) {
        d->tearDownTime = static_cast<uint64_t>(secs * 1000);
        if (d->tearDownTime < 1000) {
            d->tearDownTime = 1000;
        }
    }
}

float KcpStream::tearDownTime() const
{
    NG_D(const KcpStream);
    return d->tearDownTime / 1000.0f;
}

Socket::SocketError KcpStream::error() const
{
    NG_D(const KcpStream);
    return d->getError();
}

string KcpStream::errorString() const
{
    NG_D(const KcpStream);
    return d->getErrorString();
}

bool KcpStream::isValid() const
{
    NG_D(const KcpStream);
    return d->isValid();
}

Socket::SocketState KcpStream::state() const
{
    NG_D(const KcpStream);
    return d->state;
}

KcpStream *KcpStream::accept()
{
    NG_D(KcpStream);
    return d->accept();
}

void KcpStream::close()
{
    NG_D(KcpStream);
    d->close(false);
}

void KcpStream::abort()
{
    NG_D(KcpStream);
    d->close(true);
}

bool KcpStream::listen(int backlog)
{
    NG_D(KcpStream);
    return d->listen(backlog);
}

int32_t KcpStream::peek(char *data, int32_t size)
{
    NG_D(KcpStream);
    return d->peek(data, size);
}


int32_t KcpStream::recv(char *data, int32_t size)
{
    NG_D(KcpStream);
    return d->recv(data, size, false);
}

int32_t KcpStream::recvall(char *data, int32_t size)
{
    NG_D(KcpStream);
    return d->recv(data, size, true);
}

int32_t KcpStream::send(const char *data, int32_t size)
{
    NG_D(KcpStream);
    int32_t bytesSent = d->send(data, size, false);
    if (bytesSent == 0 && !d->isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}

int32_t KcpStream::sendall(const char *data, int32_t size)
{
    NG_D(KcpStream);
    return d->send(data, size, true);
}

string KcpStream::recv(int32_t size)
{
    NG_D(KcpStream);
    string bs(size, '\0');

    int32_t bytes = d->recv(&bs[0], bs.size(), false);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return string();
}

string KcpStream::recvall(int32_t size)
{
    NG_D(KcpStream);
    string bs(size, '\0');

    int32_t bytes = d->recv(&bs[0], bs.size(), true);
    if (bytes > 0) {
        bs.resize(bytes);
        return bs;
    }
    return string();
}

int32_t KcpStream::send(const string &data)
{
    NG_D(KcpStream);
    int32_t bytesSent = d->send(data.data(), data.size(), false);
    if (bytesSent == 0 && !d->isValid()) {
        return -1;
    } else {
        return bytesSent;
    }
}

int32_t KcpStream::sendall(const string &data)
{
    NG_D(KcpStream);
    return d->send(data.data(), data.size(), true);
}

shared_ptr<DatagramLink> KcpStream::link() const
{
    NG_D(const KcpStream);
    const MasterKcpStreamPrivate *master = dynamic_cast<const MasterKcpStreamPrivate *>(d);
    if (master) {
        return master->link;
    }
    const SlaveKcpStreamPrivate *slave = dynamic_cast<const SlaveKcpStreamPrivate *>(d);
    if (slave && slave->parent) {
        return slave->parent->link;
    }
    return shared_ptr<DatagramLink>();
}

void KcpStream::setProtocolVersion(uint8_t version)
{
    NG_D(KcpStream);
    if (version == Version1 || version == Version2) {
        d->protocolVersion = version;
    }
}

uint8_t KcpStream::protocolVersion() const
{
    NG_D(const KcpStream);
    return d->protocolVersion;
}

void KcpStream::setTunerEnabled(bool enabled)
{
    NG_D(KcpStream);
    ScopedLock<RLock> l(d->kcpLock);
    d->tunerEnabled = enabled;
    if (!d->kcp) {
        return;
    }
    if (enabled) {
        ikcp_nodelay(d->kcp, -1, -1, 16, -1);
        d->tuner.lastFastResend = 16;
    } else {
        ikcp_nodelay(d->kcp, -1, -1, 0, -1);
        d->tuner.lastFastResend = 0;
    }
}

bool KcpStream::tunerEnabled() const
{
    NG_D(const KcpStream);
    ScopedLock<RLock> l(d->kcpLock);
    return d->tunerEnabled;
}

bool KcpStream::setSendBudgetSegs(uint32_t segs)
{
    NG_D(KcpStream);
    {
        ScopedLock<RLock> l(d->kcpLock);
        if (d->tunerEnabled) {
            return false;
        }
        // 0 is allowed: external budget exhausted → snd_wnd=0, stop pulling new
        // segments into snd_buf. Do not floor to 8 — that keeps emitting while the
        // path is full.
        if (segs > 0xFFFFu) {
            segs = 0xFFFFu;
        }
        d->sendBudgetSegs = segs;
        d->applySendWindow();
    }
    // Wake / block send() waiters: applySendWindow alone does not touch
    // sendingQueueNotFull (0 → clear; >0 → maybe set).
    d->updateStatus();
    return true;
}

bool KcpStream::plaintextLooksCritical(const char *data, int32_t size)
{
    if (!data || size < 1) {
        return false;
    }
    const uint8_t t = static_cast<uint8_t>(data[0]);
    // CLOSE / KEEPALIVE tear down or probe the session; ACK / ACKN must not
    // stall behind a single lossy path.
    if (t == static_cast<uint8_t>(PACKET_TYPE_CLOSE)
            || t == static_cast<uint8_t>(PACKET_TYPE_KEEPALIVE)
            || t == PACKET_TYPE_KCP_ACK
            || t == PACKET_TYPE_KCP_ACKN) {
        return true;
    }
    // Legacy DATA (0x01) still carries a full ikcp segment; cmd sits at offset 5.
    if (t == static_cast<uint8_t>(PACKET_TYPE_UNCOMPRESSED_DATA) && size >= 6) {
        const uint8_t cmd = static_cast<uint8_t>(data[5]);
        if (cmd == PACKET_TYPE_KCP_ACK || cmd == PACKET_TYPE_KCP_ACKN) {
            return true;
        }
    }
    return false;
}

uint32_t KcpStream::sessionId() const
{
    NG_D(const KcpStream);
    return d->sessionId;
}

void KcpStream::setSessionId(uint32_t id)
{
    NG_D(KcpStream);
    d->sessionId = id;
}

DatagramPath KcpStream::peerPath() const
{
    NG_D(const KcpStream);
    return d->remotePath;
}

bool KcpStream::connect(const DatagramPath &remote)
{
    NG_D(KcpStream);
    return d->connect(remote);
}


bool KcpStream::markBound()
{
    NG_D(KcpStream);
    if (d->state != Socket::UnconnectedState) {
        return false;
    }
    d->state = Socket::BoundState;
    return true;
}

KcpStream *KcpStream::accept(const DatagramPath &remote)
{
    NG_D(KcpStream);
    return d->accept(remote);
}

class KcpSocketPrivate
{
public:
    KcpSocketPrivate(shared_ptr<DatagramLink> link, shared_ptr<UdpDatagramLink> udp, shared_ptr<KcpStream> stream)
        : link(std::move(link))
        , udp(std::move(udp))
        , stream(std::move(stream))
    {
    }

    shared_ptr<DatagramLink> link;
    shared_ptr<UdpDatagramLink> udp;  // may be null for non-UDP DatagramLink
    shared_ptr<KcpStream> stream;
    // Optional per-socket filter, invoked from KcpSocket::filter() (which the UDP link
    // filter forwards to). Lets Qt-binding code install a filter without subclassing.
    function<bool(char *, int32_t *, HostAddress *, uint16_t *)> filterCallback;
    // True when this KcpSocket installed the UDP recv filter (owns the link lifetime).
    bool ownsFilter = false;
};

static void installFilter(KcpSocket *socket, KcpSocketPrivate *d)
{
    UdpDatagramLink *udp = d->udp.get();
    if (!udp) {
        return;
    }
    udp->setFilter([socket](char *data, int32_t *len, HostAddress *addr, uint16_t *port) {
        return socket->filter(data, len, addr, port);
    });
    d->ownsFilter = true;
}

static void uninstallFilter(KcpSocketPrivate *d)
{
    if (!d->ownsFilter || !d->udp) {
        return;
    }
    d->udp->setFilter({});
    d->ownsFilter = false;
}

static KcpSocketPrivate *makePrivateRaw(shared_ptr<UdpDatagramLink> udp)
{
    shared_ptr<KcpStream> stream = make_shared<KcpStream>(udp, 0);
    stream->setProtocolVersion(KcpStream::Version1);
    return new KcpSocketPrivate(udp, udp, stream);
}

KcpSocket::KcpSocket(HostAddress::NetworkLayerProtocol protocol)
    : d_ptr(makePrivateRaw(make_shared<UdpDatagramLink>(protocol)))
{
    installFilter(this, d_ptr);
}

KcpSocket::KcpSocket(intptr_t socketDescriptor)
    : d_ptr(makePrivateRaw(make_shared<UdpDatagramLink>(socketDescriptor)))
{
    installFilter(this, d_ptr);
}

KcpSocket::KcpSocket(shared_ptr<Socket> rawSocket)
    : d_ptr(makePrivateRaw(make_shared<UdpDatagramLink>(rawSocket)))
{
    installFilter(this, d_ptr);
}

KcpSocket::KcpSocket(shared_ptr<KcpStream> stream)
    : d_ptr(new KcpSocketPrivate(stream->link(), dynamic_pointer_cast<UdpDatagramLink>(stream->link()), stream))
{
    // Do not install a recv filter here. Accepted slave streams share the master's
    // UdpDatagramLink; installing would replace the listener's filter and leave a
    // dangling KcpSocket* after the accepted socket is destroyed (Master doAccept crash).
}

KcpSocket *wrapKcpStreamAsSocket(shared_ptr<KcpStream> stream)
{
    if (!stream) {
        return nullptr;
    }
    return new KcpSocket(std::move(stream));
}

KcpSocket::~KcpSocket()
{
    uninstallFilter(d_ptr);
    delete d_ptr;
}

void KcpSocket::setUdpPacketSize(uint32_t udpPacketSize)
{
    d_ptr->stream->setPacketSize(udpPacketSize);
}

uint32_t KcpSocket::udpPacketSize() const
{
    return d_ptr->stream->packetSize();
}

void KcpSocket::setSendBufferLimit(uint64_t bytes)
{
    d_ptr->stream->setSendBufferLimit(bytes);
}

uint64_t KcpSocket::sendBufferLimit() const
{
    return d_ptr->stream->sendBufferLimit();
}

KcpStreamStats KcpSocket::stats() const
{
    return d_ptr->stream->stats();
}

uint32_t KcpSocket::payloadSizeHint() const
{
    return d_ptr->stream->payloadSizeHint();
}

void KcpSocket::setTearDownTime(float secs)
{
    d_ptr->stream->setTearDownTime(secs);
}

float KcpSocket::tearDownTime() const
{
    return d_ptr->stream->tearDownTime();
}

Socket::SocketError KcpSocket::error() const
{
    return d_ptr->stream->error();
}

string KcpSocket::errorString() const
{
    return d_ptr->stream->errorString();
}

bool KcpSocket::isValid() const
{
    return d_ptr->stream->isValid();
}

HostAddress KcpSocket::localAddress() const
{
    return d_ptr->udp ? d_ptr->udp->localAddress() : HostAddress();
}

uint16_t KcpSocket::localPort() const
{
    return d_ptr->udp ? d_ptr->udp->localPort() : 0;
}

HostAddress KcpSocket::peerAddress() const
{
    return UdpDatagramPath(d_ptr->stream->peerPath()).address();
}

string KcpSocket::peerName() const
{
    return UdpDatagramPath(d_ptr->stream->peerPath()).address().toString();
}

uint16_t KcpSocket::peerPort() const
{
    return UdpDatagramPath(d_ptr->stream->peerPath()).port();
}

Socket::SocketType KcpSocket::type() const
{
    return Socket::KcpSocket;
}

Socket::SocketState KcpSocket::state() const
{
    return d_ptr->stream->state();
}

HostAddress::NetworkLayerProtocol KcpSocket::protocol() const
{
    return d_ptr->udp ? d_ptr->udp->protocol() : HostAddress::UnknownNetworkLayerProtocol;
}

string KcpSocket::localAddressURI() const
{
    const HostAddress &addr = localAddress();
    string host = (addr.protocol() == HostAddress::IPv6Protocol)
            ? utils::formatMessage("[%1]", {addr.toString()})
            : addr.toString();
    return utils::formatMessage("%1:%2", {host, utils::number(localPort())});
}

string KcpSocket::peerAddressURI() const
{
    const HostAddress &addr = peerAddress();
    string host = (addr.protocol() == HostAddress::IPv6Protocol)
            ? utils::formatMessage("[%1]", {addr.toString()})
            : addr.toString();
    return utils::formatMessage("%1:%2", {host, utils::number(peerPort())});
}

KcpSocket *KcpSocket::accept()
{
    return wrapKcpStreamAsSocket(shared_ptr<KcpStream>(d_ptr->stream->accept()));
}

KcpSocket *KcpSocket::accept(const HostAddress &addr, uint16_t port)
{
    return wrapKcpStreamAsSocket(
            shared_ptr<KcpStream>(d_ptr->stream->accept(UdpDatagramPath(addr, port).toPath())));
}

KcpSocket *KcpSocket::accept(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    vector<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.push_back(t);
    } else if (!dnsCache) {
        addresses = Socket::resolve(hostName);
    } else {
        addresses = dnsCache->resolve(hostName);
    }
    const HostAddress::NetworkLayerProtocol prefer = protocol();
    for (const HostAddress &addr : addresses) {
        if (prefer == HostAddress::IPv4Protocol && addr.protocol() == HostAddress::IPv6Protocol) {
            continue;
        }
        if (prefer == HostAddress::IPv6Protocol && addr.protocol() == HostAddress::IPv4Protocol) {
            continue;
        }
        return accept(addr, port);
    }
    return nullptr;
}

bool KcpSocket::bind(const HostAddress &address, uint16_t port, Socket::BindMode mode)
{
    if (!d_ptr->udp || !d_ptr->udp->bind(address, port, mode)) {
        return false;
    }
    return d_ptr->stream->markBound();
}

bool KcpSocket::bind(uint16_t port, Socket::BindMode mode)
{
    if (!d_ptr->udp || !d_ptr->udp->bind(port, mode)) {
        return false;
    }
    return d_ptr->stream->markBound();
}

bool KcpSocket::connect(const HostAddress &addr, uint16_t port)
{
    return d_ptr->stream->connect(UdpDatagramPath(addr, port).toPath());
}

bool KcpSocket::connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache)
{
    vector<HostAddress> addresses;
    HostAddress t;
    if (t.setAddress(hostName)) {
        addresses.push_back(t);
    } else if (!dnsCache) {
        addresses = Socket::resolve(hostName);
    } else {
        addresses = dnsCache->resolve(hostName);
    }
    const HostAddress::NetworkLayerProtocol prefer = protocol();
    for (const HostAddress &addr : addresses) {
        if (prefer == HostAddress::IPv4Protocol && addr.protocol() == HostAddress::IPv6Protocol) {
            continue;
        }
        if (prefer == HostAddress::IPv6Protocol && addr.protocol() == HostAddress::IPv4Protocol) {
            continue;
        }
        if (connect(addr, port)) {
            return true;
        }
    }
    return false;
}

void KcpSocket::close()
{
    d_ptr->stream->close();
}

void KcpSocket::abort()
{
    d_ptr->stream->abort();
}

bool KcpSocket::listen(int backlog)
{
    return d_ptr->stream->listen(backlog);
}

bool KcpSocket::setOption(Socket::SocketOption option, int value)
{
    return d_ptr->udp ? d_ptr->udp->setOption(option, value) : false;
}

int KcpSocket::option(Socket::SocketOption option) const
{
    return d_ptr->udp ? d_ptr->udp->option(option) : -1;
}

bool KcpSocket::joinMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->udp ? d_ptr->udp->joinMulticastGroup(groupAddress, iface) : false;
}

bool KcpSocket::leaveMulticastGroup(const HostAddress &groupAddress, const NetworkInterface &iface)
{
    return d_ptr->udp ? d_ptr->udp->leaveMulticastGroup(groupAddress, iface) : false;
}

NetworkInterface KcpSocket::multicastInterface() const
{
    return d_ptr->udp ? d_ptr->udp->multicastInterface() : NetworkInterface();
}

bool KcpSocket::setMulticastInterface(const NetworkInterface &iface)
{
    return d_ptr->udp ? d_ptr->udp->setMulticastInterface(iface) : false;
}

int32_t KcpSocket::peek(char *data, int32_t size)
{
    return d_ptr->stream->peek(data, size);
}

int32_t KcpSocket::peekRaw(char *data, int32_t size)
{
    return d_ptr->udp ? d_ptr->udp->peek(data, size) : -1;
}

int32_t KcpSocket::recv(char *data, int32_t size)
{
    return d_ptr->stream->recv(data, size);
}

int32_t KcpSocket::recvall(char *data, int32_t size)
{
    return d_ptr->stream->recvall(data, size);
}

int32_t KcpSocket::send(const char *data, int32_t size)
{
    return d_ptr->stream->send(data, size);
}

int32_t KcpSocket::sendall(const char *data, int32_t size)
{
    return d_ptr->stream->sendall(data, size);
}

string KcpSocket::recv(int32_t size)
{
    return d_ptr->stream->recv(size);
}

string KcpSocket::recvall(int32_t size)
{
    return d_ptr->stream->recvall(size);
}

int32_t KcpSocket::send(const string &data)
{
    return d_ptr->stream->send(data);
}

int32_t KcpSocket::sendall(const string &data)
{
    return d_ptr->stream->sendall(data);
}

bool KcpSocket::filter(char *data, int32_t *len, HostAddress *addr, uint16_t *port)
{
    if (d_ptr->filterCallback) {
        return d_ptr->filterCallback(data, len, addr, port);
    }
    return false;
}

void KcpSocket::setFilter(function<bool(char *, int32_t *, HostAddress *, uint16_t *)> callback)
{
    d_ptr->filterCallback = std::move(callback);
}

int32_t KcpSocket::udpSend(const char *data, int32_t size, const HostAddress &addr, uint16_t port)
{
    return d_ptr->udp ? d_ptr->udp->sendto(data, size, UdpDatagramPath(addr, port).toPath()) : -1;
}

KcpSocket *KcpSocket::createConnection(const HostAddress &host, uint16_t port, Socket::SocketError *error,
                                       int allowProtocol)
{
    return qtng::createConnection<KcpSocket>(host, port, error, allowProtocol, MakeSocketType<KcpSocket>);
}

KcpSocket *KcpSocket::createConnection(const string &hostName, uint16_t port, Socket::SocketError *error,
                                       shared_ptr<SocketDnsCache> dnsCache, int allowProtocol)
{
    return qtng::createConnection<KcpSocket>(hostName, port, error, dnsCache, allowProtocol, MakeSocketType<KcpSocket>);
}

KcpSocket *KcpSocket::createServer(const HostAddress &host, uint16_t port, int backlog)
{
    return qtng::createServer<KcpSocket>(host, port, backlog, MakeSocketType<KcpSocket>);
}

class KcpSocketLikeImpl : public SocketLike
{
public:
    KcpSocketLikeImpl(shared_ptr<KcpSocket> s)
        : s(s)
    {
    }

    virtual Socket::SocketError error() const override { return s->error(); }
    virtual string errorString() const override { return s->errorString(); }
    virtual bool isValid() const override { return s->isValid(); }
    virtual HostAddress localAddress() const override { return s->localAddress(); }
    virtual uint16_t localPort() const override { return s->localPort(); }
    virtual HostAddress peerAddress() const override { return s->peerAddress(); }
    virtual string peerName() const override { return s->peerName(); }
    virtual uint16_t peerPort() const override { return s->peerPort(); }
    virtual intptr_t fileno() const override { return -1; }
    virtual Socket::SocketType type() const override { return s->type(); }
    virtual Socket::SocketState state() const override { return s->state(); }
    virtual HostAddress::NetworkLayerProtocol protocol() const override { return s->protocol(); }
    virtual string localAddressURI() const override { return s->localAddressURI(); }
    virtual string peerAddressURI() const override { return s->peerAddressURI(); }
    virtual Socket *acceptRaw() override { return nullptr; }
    virtual shared_ptr<SocketLike> accept() override { return asSocketLike(s->accept()); }
    virtual bool bind(const HostAddress &address, uint16_t port, Socket::BindMode mode) override
    {
        return s->bind(address, port, mode);
    }
    virtual bool bind(uint16_t port, Socket::BindMode mode) override { return s->bind(port, mode); }
    virtual bool connect(const HostAddress &addr, uint16_t port) override { return s->connect(addr, port); }
    virtual bool connect(const string &hostName, uint16_t port, shared_ptr<SocketDnsCache> dnsCache) override
    {
        return s->connect(hostName, port, dnsCache);
    }
    virtual void close() override { s->close(); }
    virtual void abort() override { s->abort(); }
    virtual bool listen(int backlog) override { return s->listen(backlog); }
    virtual bool setOption(Socket::SocketOption option, int value) override { return s->setOption(option, value); }
    virtual int option(Socket::SocketOption option) const override { return s->option(option); }
    virtual int32_t peek(char *data, int32_t size) override { return s->peek(data, size); }
    virtual int32_t peekRaw(char *data, int32_t size) override { return s->peekRaw(data, size); }
    virtual int32_t recv(char *data, int32_t size) override { return s->recv(data, size); }
    virtual int32_t recvall(char *data, int32_t size) override { return s->recvall(data, size); }
    virtual int32_t send(const char *data, int32_t size) override { return s->send(data, size); }
    virtual int32_t sendall(const char *data, int32_t size) override { return s->sendall(data, size); }
    virtual string recv(int32_t size) override { return s->recv(size); }
    virtual string recvall(int32_t size) override { return s->recvall(size); }
    virtual int32_t send(const string &data) override { return s->send(data); }
    virtual int32_t sendall(const string &data) override { return s->sendall(data); }

    shared_ptr<KcpSocket> s;
};

shared_ptr<SocketLike> asSocketLike(shared_ptr<KcpSocket> s)
{
    if (!s) {
        return shared_ptr<SocketLike>();
    }
    return make_shared<KcpSocketLikeImpl>(s);
}

shared_ptr<KcpSocket> convertSocketLikeToKcpSocket(shared_ptr<SocketLike> socket)
{
    shared_ptr<KcpSocketLikeImpl> impl = dynamic_pointer_cast<KcpSocketLikeImpl>(socket);
    if (impl) {
        return impl->s;
    }
    return shared_ptr<KcpSocket>();
}

}  // namespace qtng
