// Coroutine stack allocation benchmark.
//
// Scenario 1 directly measures stack acquire/release (the mmap/munmap pair the
// stack pool eliminates) by constructing/destroying Coroutine objects without
// touching the event loop.
//
// Scenario 2 exercises the real spawn+join path: a driver coroutine runs inside
// the event loop and waits on an Event for each child, so the loop stays active
// across iterations. The fixed per-iteration event-loop scheduling cost
// dominates here, so keep the count modest.
//
// Run manually with:
//
//   ./qtng_test_coroutine_bench "[benchmark]"
//
// Compare against a build without the stack pool by watching the mmap/munmap
// syscall counts:
//
//   strace -c ./qtng_test_coroutine_bench "[benchmark]"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef __unix__
#  include <sys/resource.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include "qtng/eventloop.h"
#include "qtng/coroutine.h"
#include "qtng/locks.h"

using namespace std;
using namespace qtng;

namespace {

long peakRssKb()
{
#ifdef __unix__
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss;  // KB on Linux
    }
#endif
    return -1;
}

}  // namespace

TEST_CASE("coroutine stack acquire/release", "[benchmark][!benchmark]")
{
    const char *env = getenv("QTNG_BENCH_ITERATIONS");
    const int kIterations = env ? atoi(env) : 100000;

    const chrono::steady_clock::time_point start = chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        Coroutine *c = new Coroutine();
        delete c;
    }
    const long long elapsedMs =
            chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count();

    fprintf(stderr, "coroutine bench: %d stack acquire/release in %lld ms, peak RSS %ld KB\n", kIterations,
            elapsedMs, peakRssKb());
}

TEST_CASE("coroutine spawn+join stack reuse", "[benchmark][!benchmark]")
{
    const char *env = getenv("QTNG_BENCH_ITERATIONS");
    const int kIterations = env ? atoi(env) : 2000;

    Coroutine *driver = Coroutine::spawn([&] {
        for (int i = 0; i < kIterations; ++i) {
            shared_ptr<Event> done = make_shared<Event>();
            Coroutine *c = Coroutine::spawn([done] { done->set(); });
            // Blocking wait (msecs != 0) yields to the event loop instead of
            // returning immediately, so the loop schedules the child between
            // iterations.
            done->tryWait(UINT_MAX);
            delete c;
        }
    });

    const chrono::steady_clock::time_point start = chrono::steady_clock::now();
    REQUIRE(driver->join());
    const long long elapsedMs =
            chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count();
    delete driver;

    fprintf(stderr, "coroutine bench: %d spawn+join in %lld ms, peak RSS %ld KB\n", kIterations, elapsedMs,
            peakRssKb());
}
