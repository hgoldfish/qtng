#include "bench_stats.h"

#include <cstdio>
#include <sstream>
#include <string>

#ifdef __unix__
#include <sys/resource.h>
#endif

#include "qtng/utils/datetime.h"

using namespace std;

namespace bench {

namespace {

void appendU64(ostringstream &out, const char *key, uint64_t value, bool &first)
{
    if (!first) {
        out << ',';
    }
    first = false;
    out << '"' << key << "\":" << value;
}

void appendDouble(ostringstream &out, const char *key, double value, bool &first)
{
    if (!first) {
        out << ',';
    }
    first = false;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f", value);
    out << '"' << key << "\":" << buf;
}

void appendString(ostringstream &out, const char *key, const string &value, bool &first)
{
    if (!first) {
        out << ',';
    }
    first = false;
    out << '"' << key << "\":\"" << value << '"';
}

void appendBool(ostringstream &out, const char *key, bool value, bool &first)
{
    if (!first) {
        out << ',';
    }
    first = false;
    out << '"' << key << "\":" << (value ? "true" : "false");
}

}  // namespace

double processCpuSeconds()
{
#ifdef __unix__
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    const double user = static_cast<double>(usage.ru_utime.tv_sec)
            + static_cast<double>(usage.ru_utime.tv_usec) / 1e6;
    const double sys = static_cast<double>(usage.ru_stime.tv_sec)
            + static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
    return user + sys;
#else
    return 0.0;
#endif
}

Sample sampleFromStream(const qtng::KcpStream *stream, uint64_t txPayload, uint64_t rxPayload)
{
    Sample s;
    s.tsMs = static_cast<uint64_t>(qtng::utils::DateTime::currentMSecsSinceEpoch());
    if (stream) {
        const qtng::KcpStreamStats st = stream->stats();
        s.sndWnd = st.sndWnd;
        s.sendBudgetSegs = st.sendBudgetSegs;
        s.memoryCapSegs = st.memoryCapSegs;
        s.rtoResends = st.rtoResends;
        s.fastResends = st.fastResends;
        s.fastresend = st.fastresend;
        s.waitsnd = st.waitsnd;
        s.sndUna = st.sndUna;
        s.sndNxt = st.sndNxt;
        s.sentSegs = st.sentSegs;
        s.rmtWnd = st.rmtWnd;
        s.rcvWnd = st.rcvWnd;
        s.rxSrtt = st.rxSrtt;
        s.srttMin = st.srttMin;
        s.mss = st.mss;
        s.lossRate = st.lossRate;
        s.deliveryBps = st.deliveryBps;
    }
    s.txPayload = txPayload;
    s.rxPayload = rxPayload;
    s.cpuSec = processCpuSeconds();
    return s;
}

StatsWriter::StatsWriter(const string &path)
    : m_out(path, ios::out | ios::app)
{
}

StatsWriter::~StatsWriter()
{
    if (m_out.is_open()) {
        m_out.flush();
        m_out.close();
    }
}

void StatsWriter::writeSample(const string &role, const Sample &s)
{
    if (!m_out.is_open()) {
        return;
    }
    ostringstream out;
    out << '{';
    bool first = true;
    appendString(out, "type", "sample", first);
    appendString(out, "role", role, first);
    appendU64(out, "ts_ms", s.tsMs, first);
    appendBool(out, "warm", s.warm, first);
    appendU64(out, "waitsnd", s.waitsnd, first);
    appendU64(out, "snd_wnd", s.sndWnd, first);
    appendU64(out, "send_budget_segs", s.sendBudgetSegs, first);
    appendU64(out, "memory_cap_segs", s.memoryCapSegs, first);
    appendU64(out, "rmt_wnd", s.rmtWnd, first);
    appendU64(out, "rcv_wnd", s.rcvWnd, first);
    appendU64(out, "rto_resends", s.rtoResends, first);
    appendU64(out, "fast_resends", s.fastResends, first);
    appendU64(out, "fastresend", s.fastresend, first);
    appendU64(out, "sent_segs", s.sentSegs, first);
    appendU64(out, "snd_una", s.sndUna, first);
    appendU64(out, "snd_nxt", s.sndNxt, first);
    appendU64(out, "rx_srtt", s.rxSrtt, first);
    appendU64(out, "srtt_min", s.srttMin, first);
    appendU64(out, "mss", s.mss, first);
    appendDouble(out, "loss_rate", s.lossRate, first);
    appendDouble(out, "delivery_bps", s.deliveryBps, first);
    appendU64(out, "tx_payload", s.txPayload, first);
    appendU64(out, "rx_payload", s.rxPayload, first);
    appendDouble(out, "cpu_sec", s.cpuSec, first);
    out << '}';

    lock_guard<mutex> lock(m_mutex);
    m_out << out.str() << '\n';
    m_out.flush();
}

void StatsWriter::writeSummary(const string &json)
{
    if (!m_out.is_open()) {
        return;
    }
    lock_guard<mutex> lock(m_mutex);
    m_out << json << '\n';
    m_out.flush();
}

}  // namespace bench
