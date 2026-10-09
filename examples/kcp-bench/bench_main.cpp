// kcp-bench: a single-process KCP benchmark harness.
//
// Both KCP endpoints live in one process and one event loop, so a run needs no
// root, no netns and no second binary. The datagram path is either:
//   --link sim   : an in-memory SimLink pair (deterministic, seedable faults)
//   --link real  : a real UDP pair on 127.0.0.1 (kernel syscalls and buffers)
//
// Modes:
//   throughput : time-boxed bulk transfer, reports goodput and KCP stats
//   latency    : ping-pong round trips, reports RTT percentiles
//   probe      : bulk transfer with periodic probes, reports load-time RTT
//   fault      : integrity check under loss/reorder/dup (property-based)
//
// All app traffic uses a 5-byte framed protocol so message boundaries survive,
// payloads can be verified and probes can share the bulk stream.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "qtng/coroutine.h"
#include "qtng/coroutine_utils.h"
#include "qtng/eventloop.h"
#include "qtng/hostaddress.h"
#include "qtng/kcp.h"
#include "qtng/locks.h"
#include "qtng/private/udp_p.h"
#include "qtng/socket.h"
#include "qtng/udp.h"
#include "qtng/utils/datetime.h"

#include "bench_stats.h"
#include "sim_link.h"

using namespace std;
using namespace qtng;

namespace {

// ---------------------------------------------------------------------------
// Frame protocol
// ---------------------------------------------------------------------------

const uint8_t FRAME_DATA = 0;
const uint8_t FRAME_PROBE = 1;
const uint8_t FRAME_PROBE_ACK = 2;
const uint8_t FRAME_END = 3;
const size_t FRAME_HEADER = 5;  // type(1) + big-endian u32
const size_t FRAME_PROBE_SIZE = 13;  // type(1) + u32 seq + u64 t0us
const size_t FRAME_END_SIZE = 17;    // type(1) + u64 totalLen + u64 hash

inline void appendU32(string &s, uint32_t v)
{
    s.push_back(static_cast<char>((v >> 24) & 0xff));
    s.push_back(static_cast<char>((v >> 16) & 0xff));
    s.push_back(static_cast<char>((v >> 8) & 0xff));
    s.push_back(static_cast<char>(v & 0xff));
}

inline void appendU64(string &s, uint64_t v)
{
    for (int i = 7; i >= 0; --i) {
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
}

inline uint32_t readU32(const char *p)
{
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24)
            | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16)
            | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8)
            | static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

inline uint64_t readU64(const char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint8_t>(p[i]);
    }
    return v;
}

// Deterministic payload: depends only on the byte's absolute offset in the stream.
inline uint8_t patternByte(uint64_t offset)
{
    uint64_t x = offset * 2654435761ull + 0x9e3779b97f4a7c15ull;
    x ^= x >> 29;
    return static_cast<uint8_t>(x & 0xff);
}

const uint64_t FNV_OFFSET = 1469598103934665603ull;
const uint64_t FNV_PRIME = 1099511628211ull;

inline uint64_t fnvUpdate(uint64_t h, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= FNV_PRIME;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

enum class Mode { Throughput, Latency, Probe, Fault };
enum class Direction { Up, Down, Bidi };
enum class LinkKind { Sim, Real };

struct Options
{
    Mode mode = Mode::Throughput;
    LinkKind link = LinkKind::Sim;
    Direction direction = Direction::Up;
    string role = "both";
    uint32_t rttMs = 0;
    uint32_t jitterMs = 0;
    uint32_t reorderExtraMs = 5;
    double loss = 0.0;
    double reorder = 0.0;
    double dup = 0.0;
    double corrupt = 0.0;
    uint64_t rate = 0;  // bottleneck bytes/sec; 0 = unlimited
    uint32_t seed = 1;

    uint64_t bytes = 64ull * 1024ull * 1024ull;
    double seconds = 5.0;
    uint32_t mtu = 1400;
    int version = 2;
    bool tuner = true;
    uint64_t sndBuf = 0;  // 0 = leave kernel default
    uint32_t conns = 1;
    double warmupSecs = 1.0;
    uint32_t samples = 1;

    uint32_t frameBytes = 64;     // latency mode message size
    uint32_t count = 2000;        // latency mode round trips
    uint32_t probeIntervalMs = 100;
    uint32_t iterations = 1;      // fault mode iterations
    double timeoutSecs = 120.0;
    double tearDownSecs = 3.0;
    uint64_t sendBufferLimit = 16ull * 1024ull * 1024ull;
    bool profileAlloc = false;
    // When > 0, force the Tuner off and pin sendBudgetSegs to this many segments,
    // to study how throughput depends on the window independent of the BDP guess.
    uint32_t budgetSegs = 0;

    string jsonPath;
};

const char *modeName(Mode m)
{
    switch (m) {
    case Mode::Throughput: return "throughput";
    case Mode::Latency: return "latency";
    case Mode::Probe: return "probe";
    case Mode::Fault: return "fault";
    }
    return "?";
}

const char *directionName(Direction d)
{
    switch (d) {
    case Direction::Up: return "up";
    case Direction::Down: return "down";
    case Direction::Bidi: return "bidi";
    }
    return "?";
}

const char *linkName(LinkKind k)
{
    return k == LinkKind::Sim ? "sim" : "real";
}

void printUsage()
{
    printf(
        "kcp-bench: single-process KCP benchmark\n"
        "\n"
        "Usage:\n"
        "  kcp-bench [options]\n"
        "\n"
        "Core:\n"
        "  --mode throughput|latency|probe|fault   (default throughput)\n"
        "  --link sim|real                          (default sim)\n"
        "  --direction up|down|bidi                 (default up)\n"
        "  --seconds N        measure window in seconds (throughput/probe/fault)\n"
        "  --bytes N          payload budget (fault mode; suffix K/M/G)\n"
        "  --mtu N            KCP MTU (default 1400)\n"
        "  --version 1|2      protocol version (default 2)\n"
        "  --tuner on|off     enable the send-budget Tuner (default on)\n"
        "  --conns N          parallel connections (default 1)\n"
        "  --warmup N         warmup seconds excluded from results (default 1)\n"
        "\n"
        "Simulated path (--link sim):\n"
        "  --rtt N            round-trip propagation delay in ms (split one-way)\n"
        "  --jitter N         one-way jitter in ms\n"
        "  --loss F           loss probability in [0,1)\n"
        "  --reorder F        reorder probability in [0,1)\n"
        "  --reorder-extra N  extra delay for reordered packets in ms\n"
        "  --dup F            duplication probability in [0,1)\n"
        "  --corrupt F        corruption probability in [0,1)\n"
        "  --rate N           bottleneck bandwidth (bytes/sec; suffix K/M/G)\n"
        "  --seed N           RNG seed for reproducibility\n"
        "\n"
        "Real path (--link real):\n"
        "  --sndbuf N|auto    SO_SNDBUF/SO_RCVBUF in bytes (default auto)\n"
        "\n"
        "Mode tuning:\n"
        "  --frame-bytes N    latency message size (default 64)\n"
        "  --count N          latency round trips (default 2000)\n"
        "  --probe-interval N probe period in ms (default 100)\n"
        "  --iterations N     fault-mode iterations (default 1)\n"
        "  --timeout N        watchdog seconds (default 120)\n"
        "  --tear-down N      KcpStream teardown seconds (default 3)\n"
        "  --send-buffer-limit N   KcpStream send buffer in bytes\n"
        "\n"
        "Output:\n"
        "  --json PATH        append JSONL samples and a summary line\n"
        "  --profile-alloc    count OS heap allocations for ikcp segments (off by default)\n"
        "  --budget N         pin sendBudgetSegs to N segments (forces the Tuner off)\n"
        "  -h, --help\n");
}

uint64_t parseSize(const string &text, bool *ok)
{
    *ok = false;
    if (text.empty()) {
        return 0;
    }
    size_t end = text.size();
    uint64_t mult = 1;
    const char last = text[end - 1];
    if (last == 'k' || last == 'K') {
        mult = 1024ull;
        end -= 1;
    } else if (last == 'm' || last == 'M') {
        mult = 1024ull * 1024ull;
        end -= 1;
    } else if (last == 'g' || last == 'G') {
        mult = 1024ull * 1024ull * 1024ull;
        end -= 1;
    }
    const string digits = text.substr(0, end);
    if (digits.empty()) {
        return 0;
    }
    char *parseEnd = nullptr;
    const unsigned long long value = strtoull(digits.c_str(), &parseEnd, 10);
    if (parseEnd == digits.c_str() || *parseEnd != '\0') {
        return 0;
    }
    *ok = true;
    return static_cast<uint64_t>(value) * mult;
}

bool parseDouble(const string &text, double *out)
{
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    const double v = strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

bool parseU32(const string &text, uint32_t *out)
{
    bool ok = false;
    const uint64_t v = parseSize(text, &ok);
    if (!ok || v > 0xffffffffull) {
        return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

// Accepts "5", "5s", "1.5s".
bool parseSeconds(const string &text, double *out)
{
    string t = text;
    if (!t.empty() && (t.back() == 's' || t.back() == 'S')) {
        t.pop_back();
    }
    return parseDouble(t, out);
}

bool parseArgs(int argc, char **argv, Options *o, string *error)
{
    for (int i = 1; i < argc; ++i) {
        const string arg(argv[i]);
        auto needValue = [&](const string &name) -> bool {
            if (i + 1 >= argc) {
                *error = name + " requires a value.";
                return false;
            }
            return true;
        };
        auto value = [&]() -> string { return string(argv[++i]); };

        if (arg == "-h" || arg == "--help") {
            printUsage();
            exit(0);
        } else if (arg == "--mode") {
            if (!needValue(arg)) return false;
            const string v = value();
            if (v == "throughput") o->mode = Mode::Throughput;
            else if (v == "latency") o->mode = Mode::Latency;
            else if (v == "probe") o->mode = Mode::Probe;
            else if (v == "fault") o->mode = Mode::Fault;
            else { *error = "unknown mode `" + v + "`."; return false; }
        } else if (arg == "--link") {
            if (!needValue(arg)) return false;
            const string v = value();
            if (v == "sim") o->link = LinkKind::Sim;
            else if (v == "real") o->link = LinkKind::Real;
            else { *error = "unknown link `" + v + "`."; return false; }
        } else if (arg == "--direction") {
            if (!needValue(arg)) return false;
            const string v = value();
            if (v == "up") o->direction = Direction::Up;
            else if (v == "down") o->direction = Direction::Down;
            else if (v == "bidi") o->direction = Direction::Bidi;
            else { *error = "unknown direction `" + v + "`."; return false; }
        } else if (arg == "--role") {
            if (!needValue(arg)) return false;
            o->role = value();
        } else if (arg == "--rtt") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->rttMs)) { *error = "bad --rtt."; return false; }
        } else if (arg == "--jitter") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->jitterMs)) { *error = "bad --jitter."; return false; }
        } else if (arg == "--reorder-extra") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->reorderExtraMs)) { *error = "bad --reorder-extra."; return false; }
        } else if (arg == "--loss") {
            if (!needValue(arg)) return false;
            if (!parseDouble(value(), &o->loss)) { *error = "bad --loss."; return false; }
        } else if (arg == "--reorder") {
            if (!needValue(arg)) return false;
            if (!parseDouble(value(), &o->reorder)) { *error = "bad --reorder."; return false; }
        } else if (arg == "--dup") {
            if (!needValue(arg)) return false;
            if (!parseDouble(value(), &o->dup)) { *error = "bad --dup."; return false; }
        } else if (arg == "--corrupt") {
            if (!needValue(arg)) return false;
            if (!parseDouble(value(), &o->corrupt)) { *error = "bad --corrupt."; return false; }
        } else if (arg == "--rate") {
            if (!needValue(arg)) return false;
            bool ok = false;
            o->rate = parseSize(value(), &ok);
            if (!ok) { *error = "bad --rate."; return false; }
        } else if (arg == "--seed") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->seed)) { *error = "bad --seed."; return false; }
        } else if (arg == "--bytes") {
            if (!needValue(arg)) return false;
            bool ok = false;
            o->bytes = parseSize(value(), &ok);
            if (!ok || o->bytes == 0) { *error = "bad --bytes."; return false; }
        } else if (arg == "--seconds") {
            if (!needValue(arg)) return false;
            if (!parseSeconds(value(), &o->seconds) || o->seconds <= 0) { *error = "bad --seconds."; return false; }
        } else if (arg == "--mtu") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->mtu)) { *error = "bad --mtu."; return false; }
        } else if (arg == "--version") {
            if (!needValue(arg)) return false;
            const string v = value();
            if (v == "1") o->version = 1;
            else if (v == "2") o->version = 2;
            else { *error = "bad --version."; return false; }
        } else if (arg == "--tuner") {
            if (!needValue(arg)) return false;
            const string v = value();
            if (v == "on") o->tuner = true;
            else if (v == "off") o->tuner = false;
            else { *error = "bad --tuner."; return false; }
        } else if (arg == "--sndbuf") {
            if (!needValue(arg)) return false;
            const string v = value();
            if (v == "auto") {
                o->sndBuf = 0;
            } else {
                bool ok = false;
                o->sndBuf = parseSize(v, &ok);
                if (!ok) { *error = "bad --sndbuf."; return false; }
            }
        } else if (arg == "--conns") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->conns) || o->conns == 0) { *error = "bad --conns."; return false; }
        } else if (arg == "--warmup") {
            if (!needValue(arg)) return false;
            if (!parseSeconds(value(), &o->warmupSecs)) { *error = "bad --warmup."; return false; }
        } else if (arg == "--samples") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->samples)) { *error = "bad --samples."; return false; }
        } else if (arg == "--frame-bytes") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->frameBytes) || o->frameBytes < FRAME_HEADER) {
                *error = "bad --frame-bytes.";
                return false;
            }
        } else if (arg == "--count") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->count) || o->count == 0) { *error = "bad --count."; return false; }
        } else if (arg == "--probe-interval") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->probeIntervalMs)) { *error = "bad --probe-interval."; return false; }
        } else if (arg == "--iterations") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->iterations) || o->iterations == 0) { *error = "bad --iterations."; return false; }
        } else if (arg == "--timeout") {
            if (!needValue(arg)) return false;
            if (!parseSeconds(value(), &o->timeoutSecs)) { *error = "bad --timeout."; return false; }
        } else if (arg == "--tear-down") {
            if (!needValue(arg)) return false;
            if (!parseSeconds(value(), &o->tearDownSecs)) { *error = "bad --tear-down."; return false; }
        } else if (arg == "--send-buffer-limit") {
            if (!needValue(arg)) return false;
            bool ok = false;
            o->sendBufferLimit = parseSize(value(), &ok);
            if (!ok) { *error = "bad --send-buffer-limit."; return false; }
        } else if (arg == "--json") {
            if (!needValue(arg)) return false;
            o->jsonPath = value();
        } else if (arg == "--profile-alloc") {
            o->profileAlloc = true;
        } else if (arg == "--budget") {
            if (!needValue(arg)) return false;
            if (!parseU32(value(), &o->budgetSegs) || o->budgetSegs == 0) {
                *error = "bad --budget.";
                return false;
            }
        } else {
            *error = "unknown argument `" + arg + "`.";
            return false;
        }
    }
    if (o->version != 1 && o->version != 2) {
        *error = "version must be 1 or 2.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shared run state
// ---------------------------------------------------------------------------

struct Shared
{
    atomic<bool> stop{false};
    atomic<bool> warmOver{false};
    atomic<uint64_t> txPayload{0};
    atomic<uint64_t> rxPayload{0};
    atomic<uint64_t> txFrames{0};
    atomic<uint64_t> rxFrames{0};
    // Integrity (fault mode): single writer each, read after the run.
    uint64_t txHash = FNV_OFFSET;
    uint64_t rxHash = FNV_OFFSET;
    atomic<bool> integrityOk{true};
    atomic<bool> sawEnd{false};
    atomic<uint64_t> endTotalLen{0};
    atomic<uint64_t> endHash{0};
    atomic<bool> done{false};
    Event doneEvent;
    mutex rttMutex;
    vector<double> probeRttUs;
    atomic<uint64_t> probesSent{0};
    atomic<uint64_t> probesAcked{0};
    uint64_t warmMarkRx = 0;
};

int32_t payloadCap(const KcpStream &s)
{
    const int32_t mss = static_cast<int32_t>(s.payloadSizeHint());
    const int32_t cap = mss - static_cast<int32_t>(FRAME_HEADER);
    return cap > 0 ? cap : 1;
}

// ---------------------------------------------------------------------------
// Endpoint setup
// ---------------------------------------------------------------------------

struct EndpointSet
{
    shared_ptr<bench::SimLink> simA;
    shared_ptr<bench::SimLink> simB;
    shared_ptr<UdpDatagramLink> udpA;
    shared_ptr<UdpDatagramLink> udpB;
    shared_ptr<KcpStream> a;
    shared_ptr<KcpStream> b;
};

void configureStream(const shared_ptr<KcpStream> &s, const Options &o)
{
    s->setPacketSize(o.mtu);
    s->setProtocolVersion(o.version == 1 ? KcpStream::Version1 : KcpStream::Version2);
    if (o.version == 1) {
        // Version 1 overlays the session id on the ikcp conv and the receiving
        // master drops data whose session id is 0. The normal client/server path
        // gets a nonzero id from accept(); this two-master harness must set one
        // explicitly, and both ends must agree on it.
        s->setSessionId(0x11223344u);
    }
    s->setTunerEnabled(o.tuner);
    s->setTearDownTime(static_cast<float>(o.tearDownSecs));
    s->setSendBufferLimit(o.sendBufferLimit);
    if (o.budgetSegs > 0) {
        // Pin the window: the Tuner must be off for setSendBudgetSegs to take.
        s->setTunerEnabled(false);
        s->setSendBudgetSegs(o.budgetSegs);
    }
}

bool makeSimEndpoints(const Options &o, uint32_t seedBase, EndpointSet *out, string *error)
{
    auto cfg = make_shared<bench::SimConfig>();
    cfg->oneWayDelayMs = o.rttMs / 2;
    cfg->jitterMs = o.jitterMs;
    cfg->lossRate = o.loss;
    cfg->reorderRate = o.reorder;
    cfg->reorderExtraMs = o.reorderExtraMs;
    cfg->duplicateRate = o.dup;
    cfg->corruptRate = o.corrupt;
    cfg->bandwidthBytesPerSec = o.rate;

    // Address-shaped paths so the batch path (which round-trips a Datagram's
    // addr/port through UdpDatagramPath) can be exercised on the sim link too.
    out->simA = make_shared<bench::SimLink>("127.0.0.1:1001", cfg, seedBase * 2 + 1);
    out->simB = make_shared<bench::SimLink>("127.0.0.1:1002", cfg, seedBase * 2 + 2);
    out->simA->setPeer(out->simB.get());
    out->simB->setPeer(out->simA.get());

    out->a = make_shared<KcpStream>(out->simA);
    out->b = make_shared<KcpStream>(out->simB);
    configureStream(out->a, o);
    configureStream(out->b, o);
    if (!out->a->connect(DatagramPath("127.0.0.1:1002"))
        || !out->b->connect(DatagramPath("127.0.0.1:1001"))) {
        *error = "failed to connect simulated KcpStreams.";
        return false;
    }
    return true;
}

bool makeRealEndpoints(const Options &o, EndpointSet *out, string *error)
{
    out->udpA = make_shared<UdpDatagramLink>(HostAddress::IPv4Protocol);
    out->udpB = make_shared<UdpDatagramLink>(HostAddress::IPv4Protocol);
    if (o.sndBuf > 0) {
        out->udpA->setOption(Socket::ReceiveBufferSizeSocketOption, static_cast<int>(o.sndBuf));
        out->udpA->setOption(Socket::SendBufferSizeSocketOption, static_cast<int>(o.sndBuf));
        out->udpB->setOption(Socket::ReceiveBufferSizeSocketOption, static_cast<int>(o.sndBuf));
        out->udpB->setOption(Socket::SendBufferSizeSocketOption, static_cast<int>(o.sndBuf));
    }
    const HostAddress loopback(HostAddress::LocalHost);
    if (!out->udpA->bind(loopback, 0, Socket::DefaultForPlatform)
        || !out->udpB->bind(loopback, 0, Socket::DefaultForPlatform)) {
        *error = "failed to bind real UDP sockets.";
        return false;
    }
    const uint16_t portA = out->udpA->localPort();
    const uint16_t portB = out->udpB->localPort();

    out->a = make_shared<KcpStream>(out->udpA);
    out->b = make_shared<KcpStream>(out->udpB);
    configureStream(out->a, o);
    configureStream(out->b, o);
    const DatagramPath pathA = UdpDatagramPath(loopback, portA).toPath();
    const DatagramPath pathB = UdpDatagramPath(loopback, portB).toPath();
    if (!out->a->connect(pathB) || !out->b->connect(pathA)) {
        *error = "failed to connect real KcpStreams.";
        return false;
    }
    return true;
}

bool makeEndpoints(const Options &o, uint32_t seedBase, EndpointSet *out, string *error)
{
    if (o.link == LinkKind::Sim) {
        return makeSimEndpoints(o, seedBase, out, error);
    }
    return makeRealEndpoints(o, out, error);
}

// ---------------------------------------------------------------------------
// Loops
// ---------------------------------------------------------------------------

string buildDataFrame(uint32_t seq, int32_t cap, uint64_t payloadOffsetBase)
{
    string frame;
    frame.resize(static_cast<size_t>(FRAME_HEADER) + static_cast<size_t>(cap));
    frame[0] = static_cast<char>(FRAME_DATA);
    frame[1] = static_cast<char>((seq >> 24) & 0xff);
    frame[2] = static_cast<char>((seq >> 16) & 0xff);
    frame[3] = static_cast<char>((seq >> 8) & 0xff);
    frame[4] = static_cast<char>(seq & 0xff);
    for (int32_t i = 0; i < cap; ++i) {
        frame[FRAME_HEADER + static_cast<size_t>(i)] =
                static_cast<char>(patternByte(payloadOffsetBase + static_cast<uint64_t>(i)));
    }
    return frame;
}

// Echo a PROBE back as a PROBE_ACK carrying the probe's seq and t0, so the
// sender can compute RTT. Caller guarantees buffer[0] == FRAME_PROBE and that at
// least FRAME_PROBE_SIZE bytes were read.
void sendProbeAck(KcpStream &stream, const char *buffer)
{
    string ack;
    ack.push_back(static_cast<char>(FRAME_PROBE_ACK));
    ack.append(&buffer[1], FRAME_PROBE_SIZE - 1);
    stream.send(ack.data(), static_cast<int32_t>(ack.size()));
}

void statsLoop(const vector<EndpointSet> &eps, const Options &o, Shared &sh, bench::StatsWriter *writer)
{
    (void) o;
    const uint32_t periodMs = 200;
    while (!sh.stop.load()) {
        const bool warm = !sh.warmOver.load();
        const uint64_t tx = sh.txPayload.load();
        const uint64_t rx = sh.rxPayload.load();
        if (writer) {
            for (size_t i = 0; i < eps.size(); ++i) {
                // Sample both ends: under --direction down the sender is b, so
                // recording only a would report the wrong direction's window.
                for (int which = 0; which < 2; ++which) {
                    const KcpStream *stream = (which == 0) ? eps[i].a.get() : eps[i].b.get();
                    bench::Sample s = bench::sampleFromStream(stream, tx, rx);
                    s.warm = warm;
                    ostringstream role;
                    role << (which == 0 ? 'a' : 'b') << i;
                    writer->writeSample(role.str(), s);
                }
            }
        }
        Coroutine::msleep(periodMs);
    }
}

// Cooperative shutdown: no coroutine is killed. Setting stop lets the send/receive
// loops leave on their own; abort() unblocks any that are parked inside KcpStream
// send()/recv(), and joinall() then waits for them to unwind. Killing a driver
// coroutine instead would unwind it through KCP internals and corrupt the event loop.
void shutdownWorkers(CoroutineGroup &workers, const vector<EndpointSet> &eps, Shared &sh)
{
    sh.stop.store(true);
    for (size_t i = 0; i < eps.size(); ++i) {
        if (eps[i].a) {
            eps[i].a->abort();
        }
        if (eps[i].b) {
            eps[i].b->abort();
        }
    }
    workers.joinall();
}

// Bulk sender. Accumulates a block of frames and ships it with one sendall, so a
// single KCP flush carries many segments and the batch send path is exercised
// (a per-frame send() would batch a single datagram every time).
void throughputTx(const shared_ptr<KcpStream> &s, const Options &o, Shared &sh)
{
    (void) o;
    const int32_t cap = payloadCap(*s);
    const size_t blockBytes = 256 * 1024;
    string block;
    uint32_t seq = 0;
    uint64_t offset = 0;
    while (!sh.stop.load()) {
        block.clear();
        while (block.size() < blockBytes) {
            block += buildDataFrame(seq, cap, offset);
            offset += static_cast<uint64_t>(cap);
            ++seq;
        }
        const int32_t n = s->sendall(block.data(), static_cast<int32_t>(block.size()));
        if (n <= 0) {
            break;
        }
        const uint64_t frames = static_cast<uint64_t>(n) / static_cast<uint64_t>(cap + FRAME_HEADER);
        sh.txPayload.fetch_add(frames * static_cast<uint64_t>(cap));
        sh.txFrames.fetch_add(frames);
    }
}

// Central DATA-frame handling shared by every receiver. Validates the header,
// derives the payload length and stream offset from the sequence number, checks
// the deterministic payload pattern, and accounts rxPayload/rxFrames. Returns
// the payload length, or -1 when the datagram is not a DATA frame (so the caller
// handles its own control frames).
int32_t accountDataFrame(const char *buffer, int32_t n, int32_t cap, Shared &sh)
{
    if (n < static_cast<int32_t>(FRAME_HEADER)
        || static_cast<uint8_t>(buffer[0]) != FRAME_DATA) {
        return -1;
    }
    const int32_t payload = n - static_cast<int32_t>(FRAME_HEADER);
    const uint64_t seq = readU32(&buffer[1]);
    const uint64_t offset = seq * static_cast<uint64_t>(cap);
    for (int32_t i = 0; i < payload; ++i) {
        if (static_cast<uint8_t>(buffer[FRAME_HEADER + static_cast<size_t>(i)])
            != patternByte(offset + static_cast<uint64_t>(i))) {
            sh.integrityOk.store(false);
            break;
        }
    }
    sh.rxPayload.fetch_add(static_cast<uint64_t>(payload));
    sh.rxFrames.fetch_add(1);
    return payload;
}

// Bulk receiver; verifies and counts payload. Control frames are ignored.
// The buffer holds exactly one frame (cap + FRAME_HEADER); a block send arrives
// as one large KCP message and recv() hands it out frame by frame.
void throughputRx(const shared_ptr<KcpStream> &s, const Options &o, Shared &sh)
{
    (void) o;
    const int32_t cap = payloadCap(*s);
    string buffer(static_cast<size_t>(cap) + FRAME_HEADER, '\0');
    while (!sh.stop.load()) {
        const int32_t n = s->recv(&buffer[0], static_cast<int32_t>(buffer.size()));
        if (n <= 0) {
            break;
        }
        (void) accountDataFrame(buffer.data(), n, cap, sh);
    }
}

// Server side of probe mode: count bulk, echo probes.
void probeServerRx(const shared_ptr<KcpStream> &s, const Options &o, Shared &sh)
{
    (void) o;
    const int32_t cap = payloadCap(*s);
    string buffer(static_cast<size_t>(cap) + FRAME_HEADER, '\0');
    while (!sh.stop.load()) {
        const int32_t n = s->recv(&buffer[0], static_cast<int32_t>(buffer.size()));
        if (n <= 0) {
            break;
        }
        if (accountDataFrame(buffer.data(), n, cap, sh) >= 0) {
            continue;
        }
        if (static_cast<uint8_t>(buffer[0]) == FRAME_PROBE
            && n >= static_cast<int32_t>(FRAME_PROBE_SIZE)) {
            sendProbeAck(*s, buffer.data());
        }
    }
}

// Client side of probe mode: bulk TX with periodic probes, in blocks so the
// batch send path is exercised (probes are sent as their own messages).
void probeClientTx(const shared_ptr<KcpStream> &s, const Options &o, Shared &sh)
{
    const int32_t cap = payloadCap(*s);
    const size_t blockBytes = 256 * 1024;
    string block;
    uint32_t seq = 0;
    uint32_t probeSeq = 0;
    uint64_t offset = 0;
    uint64_t lastProbeMs = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
    while (!sh.stop.load()) {
        const uint64_t now = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch());
        if (now - lastProbeMs >= o.probeIntervalMs) {
            string probe;
            probe.push_back(static_cast<char>(FRAME_PROBE));
            const uint32_t s32 = probeSeq++;
            probe.push_back(static_cast<char>((s32 >> 24) & 0xff));
            probe.push_back(static_cast<char>((s32 >> 16) & 0xff));
            probe.push_back(static_cast<char>((s32 >> 8) & 0xff));
            probe.push_back(static_cast<char>(s32 & 0xff));
            appendU64(probe, static_cast<uint64_t>(now) * 1000ull);
            s->send(probe.data(), static_cast<int32_t>(probe.size()));
            sh.probesSent.fetch_add(1);
            lastProbeMs = now;
        }
        block.clear();
        while (block.size() < blockBytes) {
            block += buildDataFrame(seq, cap, offset);
            offset += static_cast<uint64_t>(cap);
            ++seq;
        }
        const int32_t n = s->sendall(block.data(), static_cast<int32_t>(block.size()));
        if (n <= 0) {
            break;
        }
        const uint64_t frames = static_cast<uint64_t>(n) / static_cast<uint64_t>(cap + FRAME_HEADER);
        sh.txPayload.fetch_add(frames * static_cast<uint64_t>(cap));
    }
}

void probeClientRx(const shared_ptr<KcpStream> &s, const Options &, Shared &sh)
{
    const int32_t cap = payloadCap(*s);
    string buffer(static_cast<size_t>(cap) + FRAME_HEADER, '\0');
    while (!sh.stop.load()) {
        const int32_t n = s->recv(&buffer[0], static_cast<int32_t>(buffer.size()));
        if (n <= 0) {
            break;
        }
        if (accountDataFrame(buffer.data(), n, cap, sh) >= 0) {
            continue;
        }
        if (static_cast<uint8_t>(buffer[0]) == FRAME_PROBE_ACK
            && n >= static_cast<int32_t>(FRAME_PROBE_SIZE)) {
            const uint64_t t0Us = readU64(&buffer[5]);
            const uint64_t nowU = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch()) * 1000ull;
            if (sh.warmOver.load() && nowU >= t0Us) {
                lock_guard<mutex> lock(sh.rttMutex);
                sh.probeRttUs.push_back(static_cast<double>(nowU - t0Us));
            }
            sh.probesAcked.fetch_add(1);
        }
    }
}

// Fault mode: send a fixed budget with verification, then an END frame.
void faultTx(const shared_ptr<KcpStream> &s, const Options &o, Shared &sh)
{
    const int32_t cap = payloadCap(*s);
    const uint64_t totalFrames = (o.bytes + static_cast<uint64_t>(cap) - 1) / static_cast<uint64_t>(cap);
    uint64_t hash = FNV_OFFSET;
    for (uint64_t seq = 0; seq < totalFrames && !sh.stop.load(); ++seq) {
        const uint64_t offset = seq * static_cast<uint64_t>(cap);
        const string frame = buildDataFrame(static_cast<uint32_t>(seq), cap, offset);
        const int32_t n = s->send(frame.data(), static_cast<int32_t>(frame.size()));
        if (n <= 0) {
            break;
        }
        hash = fnvUpdate(hash, reinterpret_cast<const uint8_t *>(frame.data()) + FRAME_HEADER,
                         static_cast<size_t>(cap));
        sh.txPayload.fetch_add(static_cast<uint64_t>(cap));
        sh.txFrames.fetch_add(1);
    }
    sh.txHash = hash;
    string end;
    end.push_back(static_cast<char>(FRAME_END));
    appendU64(end, totalFrames * static_cast<uint64_t>(cap));
    appendU64(end, hash);
    s->send(end.data(), static_cast<int32_t>(end.size()));
}

void faultRx(const shared_ptr<KcpStream> &s, const Options &, Shared &sh)
{
    const int32_t cap = payloadCap(*s);
    string buffer(static_cast<size_t>(cap) + FRAME_HEADER, '\0');
    while (!sh.stop.load()) {
        const int32_t n = s->recv(&buffer[0], static_cast<int32_t>(buffer.size()));
        if (n <= 0) {
            break;
        }
        const int32_t payload = accountDataFrame(buffer.data(), n, cap, sh);
        if (payload >= 0) {
            sh.rxHash = fnvUpdate(sh.rxHash,
                                  reinterpret_cast<const uint8_t *>(buffer.data()) + FRAME_HEADER,
                                  static_cast<size_t>(payload));
            continue;
        }
        if (static_cast<uint8_t>(buffer[0]) == FRAME_END
            && n >= static_cast<int32_t>(FRAME_END_SIZE)) {
            sh.endTotalLen.store(readU64(&buffer[1]));
            sh.endHash.store(readU64(&buffer[9]));
            sh.sawEnd.store(true);
            sh.done.store(true);
            sh.doneEvent.set();
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Results and percentiles
// ---------------------------------------------------------------------------

double percentile(vector<double> v, double p)
{
    if (v.empty()) {
        return 0.0;
    }
    sort(v.begin(), v.end());
    const double idx = (p / 100.0) * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

struct Summary
{
    bool ok = true;
    string error;
    double goodputMbps = 0.0;
    double goodputMiBps = 0.0;
    uint64_t deliveredBytes = 0;
    double seconds = 0.0;
    double cpuSec = 0.0;
    uint64_t rtoResends = 0;
    uint64_t fastResends = 0;
    double lossRate = 0.0;
    double probeP50Us = 0.0;
    double probeP90Us = 0.0;
    double probeP99Us = 0.0;
    double probeMaxUs = 0.0;
    uint64_t probesSent = 0;
    uint64_t probesAcked = 0;
    uint64_t packetsDropped = 0;
    bool integrityOk = true;
    uint64_t iterationsRun = 0;
    uint64_t allocations = 0;
};

string summaryJson(const Options &o, const Summary &s)
{
    ostringstream out;
    out << '{';
    bool first = true;
    auto str = [&](const char *k, const string &v) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << '"' << k << "\":\"" << v << '"';
    };
    auto num = [&](const char *k, double v) {
        if (!first) {
            out << ',';
        }
        first = false;
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", v);
        out << '"' << k << "\":" << buf;
    };
    auto u64 = [&](const char *k, uint64_t v) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << '"' << k << "\":" << v;
    };
    auto boolean = [&](const char *k, bool v) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << '"' << k << "\":" << (v ? "true" : "false");
    };

    str("type", "summary");
    str("mode", modeName(o.mode));
    str("link", linkName(o.link));
    str("direction", directionName(o.direction));
    u64("rtt_ms", o.rttMs);
    num("loss", o.loss);
    num("reorder", o.reorder);
    num("dup", o.dup);
    num("corrupt", o.corrupt);
    u64("rate", o.rate);
    u64("seed", o.seed);
    u64("mtu", o.mtu);
    u64("version", static_cast<uint64_t>(o.version));
    boolean("tuner", o.tuner && o.budgetSegs == 0);
    u64("budget_segs", o.budgetSegs);
    u64("conns", o.conns);
    u64("warmup_ms", static_cast<uint64_t>(o.warmupSecs * 1000));
    str("result", s.ok ? "ok" : "fail");
    if (!s.ok) {
        str("error", s.error);
    }
    num("goodput_mbps", s.goodputMbps);
    num("goodput_mibps", s.goodputMiBps);
    u64("delivered_bytes", s.deliveredBytes);
    num("seconds", s.seconds);
    num("cpu_sec", s.cpuSec);
    u64("rto_resends", s.rtoResends);
    u64("fast_resends", s.fastResends);
    num("loss_rate", s.lossRate);
    num("probe_p50_us", s.probeP50Us);
    num("probe_p90_us", s.probeP90Us);
    num("probe_p99_us", s.probeP99Us);
    num("probe_max_us", s.probeMaxUs);
    u64("probes_sent", s.probesSent);
    u64("probes_acked", s.probesAcked);
    u64("packets_dropped", s.packetsDropped);
    boolean("integrity_ok", s.integrityOk);
    u64("iterations", s.iterationsRun);
    u64("ikcp_allocs", s.allocations);
    out << '}';
    return out.str();
}

void printSummary(const Options &o, const Summary &s)
{
    printf("mode=%s link=%s direction=%s rtt=%ums loss=%.4f reorder=%.4f mtU=%u v%u tuner=%s conns=%u result=%s\n",
           modeName(o.mode), linkName(o.link), directionName(o.direction), o.rttMs, o.loss,
           o.reorder, o.mtu, static_cast<unsigned>(o.version), o.tuner ? "on" : "off",
           o.conns, s.ok ? "ok" : "FAIL");
    if (!s.ok) {
        printf("  error: %s\n", s.error.c_str());
    }
    if (o.mode == Mode::Latency || o.mode == Mode::Probe) {
        printf("  probe RTT us: p50=%.0f p90=%.0f p99=%.0f max=%.0f (acked %llu/%llu)\n",
               s.probeP50Us, s.probeP90Us, s.probeP99Us, s.probeMaxUs,
               static_cast<unsigned long long>(s.probesAcked),
               static_cast<unsigned long long>(s.probesSent));
    }
    if (o.mode == Mode::Throughput || o.mode == Mode::Probe || o.mode == Mode::Fault) {
        printf("  goodput: %.3f MiB/s (%.3f Mbit/s), delivered %.3f MiB in %.3fs, cpu %.2fs\n",
               s.goodputMiBps, s.goodputMbps,
               static_cast<double>(s.deliveredBytes) / (1024.0 * 1024.0), s.seconds, s.cpuSec);
    }
    if (o.profileAlloc) {
        const double perMb = s.deliveredBytes > 0
                ? static_cast<double>(s.allocations) / (static_cast<double>(s.deliveredBytes) / (1024.0 * 1024.0))
                : 0.0;
        printf("  OS segment allocations: %llu (%.1f per delivered MiB)\n",
               static_cast<unsigned long long>(s.allocations), perMb);
    }
    if (o.mode == Mode::Fault) {
        printf("  integrity: %s, iterations=%llu\n", s.integrityOk ? "ok" : "FAIL",
               static_cast<unsigned long long>(s.iterationsRun));
    }
}

uint64_t totalRtoResends(const vector<EndpointSet> &eps)
{
    // Sum both ends: for direction=down the sender is b, so reading only a would
    // report zero retransmissions. For bidi both directions are genuinely counted.
    uint64_t total = 0;
    for (size_t i = 0; i < eps.size(); ++i) {
        total += eps[i].a->stats().rtoResends;
        total += eps[i].b->stats().rtoResends;
    }
    return total;
}

uint64_t totalFastResends(const vector<EndpointSet> &eps)
{
    uint64_t total = 0;
    for (size_t i = 0; i < eps.size(); ++i) {
        total += eps[i].a->stats().fastResends;
        total += eps[i].b->stats().fastResends;
    }
    return total;
}

double maxLossRate(const vector<EndpointSet> &eps)
{
    double m = 0;
    for (size_t i = 0; i < eps.size(); ++i) {
        m = max(m, eps[i].a->stats().lossRate);
        m = max(m, eps[i].b->stats().lossRate);
    }
    return m;
}

uint64_t totalSimDropped(const vector<EndpointSet> &eps)
{
    uint64_t total = 0;
    for (size_t i = 0; i < eps.size(); ++i) {
        if (eps[i].simA) total += eps[i].simA->packetsDropped();
        if (eps[i].simB) total += eps[i].simB->packetsDropped();
    }
    return total;
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

Summary runThroughputMode(const Options &o, const vector<EndpointSet> &eps, Shared &sh, bench::StatsWriter *writer)
{
    Summary s;
    CoroutineGroup workers;
    workers.spawn([&] { statsLoop(eps, o, sh, writer); });
    for (size_t i = 0; i < eps.size(); ++i) {
        const shared_ptr<KcpStream> &tx = (o.direction == Direction::Down) ? eps[i].b : eps[i].a;
        const shared_ptr<KcpStream> &rx = (o.direction == Direction::Down) ? eps[i].a : eps[i].b;
        workers.spawn([tx, &o, &sh] { throughputTx(tx, o, sh); });
        workers.spawn([rx, &o, &sh] { throughputRx(rx, o, sh); });
        if (o.direction == Direction::Bidi) {
            workers.spawn([&eps, i, &o, &sh] { throughputTx(eps[i].b, o, sh); });
            workers.spawn([&eps, i, &o, &sh] { throughputRx(eps[i].a, o, sh); });
        }
    }

    Coroutine::msleep(static_cast<uint32_t>(o.warmupSecs * 1000));
    sh.warmMarkRx = sh.rxPayload.load();
    sh.warmOver.store(true);
    Coroutine::msleep(static_cast<uint32_t>(o.seconds * 1000));
    shutdownWorkers(workers, eps, sh);

    const uint64_t delivered = sh.rxPayload.load() - sh.warmMarkRx;
    s.deliveredBytes = delivered;
    s.seconds = o.seconds;
    s.goodputMiBps = static_cast<double>(delivered) / (1024.0 * 1024.0) / o.seconds;
    s.goodputMbps = s.goodputMiBps * 8.0;
    s.cpuSec = bench::processCpuSeconds();
    s.rtoResends = totalRtoResends(eps);
    s.fastResends = totalFastResends(eps);
    s.lossRate = maxLossRate(eps);
    s.packetsDropped = totalSimDropped(eps);
    s.integrityOk = sh.integrityOk.load();
    return s;
}

Summary runLatencyMode(const Options &o, const vector<EndpointSet> &eps, Shared &sh, bench::StatsWriter *writer)
{
    Summary s;
    const shared_ptr<KcpStream> &client = eps[0].a;
    const shared_ptr<KcpStream> &server = eps[0].b;
    const int32_t cap = payloadCap(*client);
    const uint32_t frameBytes = min(o.frameBytes, static_cast<uint32_t>(cap + FRAME_HEADER));
    string buffer(static_cast<size_t>(cap) + FRAME_HEADER + 64, '\0');

    CoroutineGroup workers;
    workers.spawn([&] { statsLoop(eps, o, sh, writer); });
    workers.spawn([server] {
        // The echo loop owns its buffer: sharing one with the client loop would
        // interleave the two coroutines' partial reads.
        const int32_t cap = payloadCap(*server);
        string buffer(static_cast<size_t>(cap) + FRAME_HEADER + 64, '\0');
        while (true) {
            const int32_t n = server->recv(&buffer[0], static_cast<int32_t>(buffer.size()));
            if (n <= 0) break;
            if (n < static_cast<int32_t>(FRAME_HEADER)) continue;
            if (static_cast<uint8_t>(buffer[0]) == FRAME_PROBE
                && n >= static_cast<int32_t>(FRAME_PROBE_SIZE)) {
                sendProbeAck(*server, buffer.data());
            }
        }
    });

    vector<double> rtts;
    const uint32_t warmCount = max<uint32_t>(1u, o.count / 10u);
    for (uint32_t i = 0; i < o.count && !sh.stop.load(); ++i) {
        string probe;
        probe.push_back(static_cast<char>(FRAME_PROBE));
        appendU32(probe, i);
        const uint64_t t0Us = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch()) * 1000ull;
        appendU64(probe, t0Us);
        while (static_cast<int32_t>(probe.size()) < static_cast<int32_t>(frameBytes)) {
            probe.push_back(static_cast<char>(patternByte(static_cast<uint64_t>(i) + probe.size())));
        }
        if (client->send(probe.data(), static_cast<int32_t>(probe.size())) <= 0) {
            break;
        }
        // Wait for the matching echo.
        while (!sh.stop.load()) {
            const int32_t n = client->recv(&buffer[0], static_cast<int32_t>(buffer.size()));
            if (n <= 0) {
                break;
            }
            if (n >= static_cast<int32_t>(FRAME_PROBE_SIZE) && static_cast<uint8_t>(buffer[0]) == FRAME_PROBE_ACK) {
                const uint64_t nowU = static_cast<uint64_t>(utils::DateTime::currentMSecsSinceEpoch()) * 1000ull;
                if (i >= warmCount) {
                    rtts.push_back(static_cast<double>(nowU - t0Us));
                }
                break;
            }
        }
    }
    shutdownWorkers(workers, eps, sh);

    s.probeP50Us = percentile(rtts, 50);
    s.probeP90Us = percentile(rtts, 90);
    s.probeP99Us = percentile(rtts, 99);
    s.probeMaxUs = rtts.empty() ? 0.0 : *max_element(rtts.begin(), rtts.end());
    s.cpuSec = bench::processCpuSeconds();
    s.integrityOk = sh.integrityOk.load();
    if (rtts.empty()) {
        s.ok = false;
        s.error = "no latency samples (all round trips failed).";
    }
    return s;
}

Summary runProbeMode(const Options &o, const vector<EndpointSet> &eps, Shared &sh, bench::StatsWriter *writer)
{
    Summary s;
    CoroutineGroup workers;
    workers.spawn([&] { statsLoop(eps, o, sh, writer); });
    for (size_t i = 0; i < eps.size(); ++i) {
        workers.spawn([&eps, i, &o, &sh] { probeClientTx(eps[i].a, o, sh); });
        workers.spawn([&eps, i, &o, &sh] { probeClientRx(eps[i].a, o, sh); });
        workers.spawn([&eps, i, &o, &sh] { probeServerRx(eps[i].b, o, sh); });
    }

    Coroutine::msleep(static_cast<uint32_t>(o.warmupSecs * 1000));
    sh.warmMarkRx = sh.rxPayload.load();
    sh.warmOver.store(true);
    Coroutine::msleep(static_cast<uint32_t>(o.seconds * 1000));
    shutdownWorkers(workers, eps, sh);

    vector<double> rtts;
    {
        lock_guard<mutex> lock(sh.rttMutex);
        rtts = sh.probeRttUs;
    }
    s.probeP50Us = percentile(rtts, 50);
    s.probeP90Us = percentile(rtts, 90);
    s.probeP99Us = percentile(rtts, 99);
    s.probeMaxUs = rtts.empty() ? 0.0 : *max_element(rtts.begin(), rtts.end());
    s.probesSent = sh.probesSent.load();
    s.probesAcked = sh.probesAcked.load();
    const uint64_t delivered = sh.rxPayload.load() - sh.warmMarkRx;
    s.deliveredBytes = delivered;
    s.seconds = o.seconds;
    s.goodputMiBps = static_cast<double>(delivered) / (1024.0 * 1024.0) / o.seconds;
    s.goodputMbps = s.goodputMiBps * 8.0;
    s.cpuSec = bench::processCpuSeconds();
    s.rtoResends = totalRtoResends(eps);
    s.fastResends = totalFastResends(eps);
    s.lossRate = maxLossRate(eps);
    s.packetsDropped = totalSimDropped(eps);
    s.integrityOk = sh.integrityOk.load();
    if (rtts.empty()) {
        s.ok = false;
        s.error = "no probe samples.";
    }
    return s;
}

Summary runFaultMode(const Options &o, Shared &sh, bench::StatsWriter *writer)
{
    Summary s;
    mt19937 paramRng(o.seed * 7919u + 17u);
    uniform_real_distribution<double> unit(0.0, 1.0);
    uniform_int_distribution<uint32_t> rttPick(0, 120);
    uniform_int_distribution<uint32_t> extraPick(1, 20);

    for (uint32_t iter = 0; iter < o.iterations; ++iter) {
        Options opt = o;
        const uint32_t iterSeed = o.seed + iter;
        opt.rttMs = o.rttMs != 0 ? o.rttMs : rttPick(paramRng);
        opt.loss = o.loss > 0.0 ? o.loss : unit(paramRng) * 0.03;
        opt.reorder = o.reorder > 0.0 ? o.reorder : unit(paramRng) * 0.01;
        opt.dup = o.dup > 0.0 ? o.dup : unit(paramRng) * 0.02;
        opt.reorderExtraMs = extraPick(paramRng);

        EndpointSet ep;
        string error;
        if (!makeEndpoints(opt, iterSeed, &ep, &error)) {
            s.ok = false;
            s.error = "iteration " + to_string(iter) + ": " + error;
            return s;
        }
        Shared ish;
        CoroutineGroup workers;
        workers.spawn([&ep, &opt, &ish] { faultRx(ep.b, opt, ish); });
        workers.spawn([&ep, &opt, &ish] { faultTx(ep.a, opt, ish); });

        // Wait for the receiver to see END, bounded by the watchdog.
        const uint32_t waitMs = static_cast<uint32_t>(max(1000.0, opt.seconds * 1000.0));
        ish.doneEvent.tryWait(waitMs);
        if (!ish.done.load()) {
            ish.stop.store(true);
            ep.a->abort();
            ep.b->abort();
            workers.joinall();
            s.ok = false;
            s.error = "iteration " + to_string(iter) + " (seed " + to_string(iterSeed)
                    + "): timeout waiting for END frame.";
            return s;
        }
        ish.stop.store(true);
        ep.a->abort();
        ep.b->abort();
        workers.joinall();

        const bool hashOk = (ish.txHash == ish.rxHash);
        const bool lenOk = (ish.rxPayload.load() == ish.endTotalLen.load());
        const bool patOk = ish.integrityOk.load();
        const bool frameOk = (ish.txFrames.load() == ish.rxFrames.load());
        if (!hashOk || !lenOk || !patOk || !frameOk) {
            s.ok = false;
            ostringstream msg;
            msg << "iteration " << iter << " (seed " << iterSeed << " rtt "
                << opt.rttMs << "ms loss " << opt.loss << " reorder " << opt.reorder
                << " dup " << opt.dup << "): integrity FAIL"
                << " hash=" << (hashOk ? "ok" : "bad")
                << " len=" << (lenOk ? "ok" : "bad")
                << " pattern=" << (patOk ? "ok" : "bad")
                << " frames=" << (frameOk ? "ok" : "bad")
                << " (tx " << ish.txFrames.load() << " rx " << ish.rxFrames.load() << ")";
            s.error = msg.str();
            return s;
        }
        s.iterationsRun = iter + 1;
        if (writer) {
            bench::Sample smp = bench::sampleFromStream(ep.a.get(), ish.txPayload.load(), ish.rxPayload.load());
            writer->writeSample("fault", smp);
        }
    }
    s.cpuSec = bench::processCpuSeconds();
    s.integrityOk = true;
    return s;
}

// ---------------------------------------------------------------------------
// Top-level runner
// ---------------------------------------------------------------------------

Summary runAll(const Options &o)
{
    Summary s;
    if (o.link == LinkKind::Real && (o.rttMs > 0 || o.loss > 0.0 || o.rate > 0)) {
        fprintf(stderr, "warning: --rtt/--loss/--rate are ignored with --link real.\n");
    }

    // Whole-run watchdog. Timeout raises TimeoutException into this coroutine.
    Timeout watchdog(static_cast<float>(o.timeoutSecs));

    const uint64_t allocStart = kcpSegmentOsAllocationCount();
    bench::StatsWriter writer(o.jsonPath);

    if (o.mode == Mode::Fault) {
        Shared sh;
        s = runFaultMode(o, sh, &writer);
        s.allocations = kcpSegmentOsAllocationCount() - allocStart;
        if (writer.isOpen()) {
            writer.writeSummary(summaryJson(o, s));
        }
        return s;
    }

    vector<EndpointSet> eps;
    for (uint32_t i = 0; i < o.conns; ++i) {
        EndpointSet ep;
        string error;
        if (!makeEndpoints(o, o.seed + i, &ep, &error)) {
            s.ok = false;
            s.error = error;
            if (writer.isOpen()) {
                writer.writeSummary(summaryJson(o, s));
            }
            return s;
        }
        eps.push_back(ep);
    }

    Shared sh;
    Summary result;
    if (o.mode == Mode::Throughput) {
        result = runThroughputMode(o, eps, sh, o.jsonPath.empty() ? nullptr : &writer);
    } else if (o.mode == Mode::Latency) {
        result = runLatencyMode(o, eps, sh, o.jsonPath.empty() ? nullptr : &writer);
    } else {
        result = runProbeMode(o, eps, sh, o.jsonPath.empty() ? nullptr : &writer);
    }

    result.allocations = kcpSegmentOsAllocationCount() - allocStart;
    if (writer.isOpen()) {
        writer.writeSummary(summaryJson(o, result));
    }
    return result;
}

}  // namespace

int main(int argc, char **argv)
{
    Options o;
    string error;
    if (!parseArgs(argc, argv, &o, &error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    shared_ptr<Summary> result = make_shared<Summary>();
    CoroutineGroup group;
    shared_ptr<Coroutine> runner = group.spawn([&o, result] {
        try {
            *result = runAll(o);
        } catch (const TimeoutException &) {
            result->ok = false;
            result->error = "watchdog timeout.";
        } catch (const CoroutineExitException &) {
            result->ok = false;
            result->error = "coroutine exit.";
        } catch (const std::exception &e) {
            result->ok = false;
            result->error = e.what();
        } catch (...) {
            result->ok = false;
            result->error = "unknown exception.";
        }
    });
    runner->join();

    printSummary(o, *result);
    return result->ok ? 0 : 1;
}
