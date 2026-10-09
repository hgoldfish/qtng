#ifndef KCP_BENCH_STATS_H
#define KCP_BENCH_STATS_H

// Periodic KcpStreamStats sampling plus CPU accounting, emitted as JSON Lines so
// run_matrix.py (or any script) can aggregate without a JSON dependency here.

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

#include "qtng/kcp.h"

namespace bench {

struct Sample
{
    std::uint64_t tsMs = 0;
    bool warm = false;
    std::uint32_t sndWnd = 0;
    std::uint32_t sendBudgetSegs = 0;
    std::uint32_t memoryCapSegs = 0;
    std::uint32_t rtoResends = 0;
    std::uint32_t fastResends = 0;
    std::uint32_t fastresend = 0;
    std::uint32_t waitsnd = 0;
    std::uint32_t sndUna = 0;
    std::uint32_t sndNxt = 0;
    std::uint32_t sentSegs = 0;
    std::uint32_t rmtWnd = 0;
    std::uint32_t rcvWnd = 0;
    std::uint32_t rxSrtt = 0;
    std::uint32_t srttMin = 0;
    std::uint32_t mss = 0;
    double lossRate = 0;
    double deliveryBps = 0;
    std::uint64_t txPayload = 0;
    std::uint64_t rxPayload = 0;
    double cpuSec = 0;
};

class StatsWriter
{
public:
    explicit StatsWriter(const std::string &path);
    ~StatsWriter();

    // One JSON object per call, tagged with the stream role ("a" / "b").
    void writeSample(const std::string &role, const Sample &s);
    // The final object carrying the run summary.
    void writeSummary(const std::string &json);
    bool isOpen() const { return m_out.is_open(); }

private:
    std::ofstream m_out;
    std::mutex m_mutex;
};

// Process CPU time (user + system) in seconds, via getrusage.
double processCpuSeconds();

// Fill the KcpStream-derived fields of a sample.
Sample sampleFromStream(const qtng::KcpStream *stream, std::uint64_t txPayload, std::uint64_t rxPayload);

}  // namespace bench

#endif  // KCP_BENCH_STATS_H
