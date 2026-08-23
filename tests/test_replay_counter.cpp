#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <set>
#include <vector>

#include "qtng/private/noise_replay_p.h"

using namespace std;
using namespace qtng;

namespace {

uint64_t oldestAcceptableZeroBased(uint64_t oneBasedCounter)
{
    if (oneBasedCounter == 0) {
        return 0;
    }
    const uint64_t oldestOneBased = oneBasedCounter > kCounterWindowSize ? oneBasedCounter - kCounterWindowSize
                                                                         : 1;
    return oldestOneBased - 1;
}

struct NaiveReplayCounter
{
    uint64_t counter = 0;
    set<uint64_t> seen;

    void reset()
    {
        counter = 0;
        seen.clear();
    }

    bool validate(uint64_t theirCounter)
    {
        if (counter >= kRejectAfterMessages + 1 || theirCounter >= kRejectAfterMessages) {
            return false;
        }
        ++theirCounter;
        if (kCounterWindowSize + theirCounter < counter) {
            return false;
        }
        if (seen.count(theirCounter)) {
            return false;
        }
        if (theirCounter > counter) {
            counter = theirCounter;
            for (auto it = seen.begin(); it != seen.end();) {
                if (*it + kCounterWindowSize < counter) {
                    it = seen.erase(it);
                } else {
                    ++it;
                }
            }
        }
        seen.insert(theirCounter);
        return true;
    }
};

}  // namespace

TEST_CASE("ReplayCounter accepts sequential nonces", "[replay_counter]")
{
    ReplayCounter rc;
    for (uint64_t i = 0; i < 10'000; ++i) {
        REQUIRE(rc.validate(i));
        REQUIRE_FALSE(rc.validate(i));
    }
    REQUIRE(rc.counter == 10'000);
}

TEST_CASE("ReplayCounter rejects duplicate and stale packets", "[replay_counter]")
{
    ReplayCounter rc;
    REQUIRE(rc.validate(0));
    REQUIRE_FALSE(rc.validate(0));
    REQUIRE(rc.validate(1));
    REQUIRE(rc.validate(2));
    REQUIRE_FALSE(rc.validate(1));
}

TEST_CASE("ReplayCounter accepts out-of-order packets inside the window", "[replay_counter]")
{
    ReplayCounter rc;
    REQUIRE(rc.validate(100));
    REQUIRE(rc.validate(50));
    REQUIRE(rc.validate(99));
    REQUIRE_FALSE(rc.validate(50));
    REQUIRE(rc.validate(101));
}

TEST_CASE("ReplayCounter window boundary", "[replay_counter]")
{
    ReplayCounter rc;
    const uint64_t high = 20'000;
    REQUIRE(rc.validate(high));

    const uint64_t tooOld = high - kCounterWindowSize - 1;
    REQUIRE_FALSE(rc.validate(tooOld));

    const uint64_t oldestOk = high - kCounterWindowSize;
    REQUIRE(rc.validate(oldestOk));
    REQUIRE_FALSE(rc.validate(oldestOk));
}

TEST_CASE("ReplayCounter reset clears state", "[replay_counter]")
{
    ReplayCounter rc;
    REQUIRE(rc.validate(42));
    REQUIRE_FALSE(rc.validate(42));
    rc.reset();
    REQUIRE(rc.counter == 0);
    REQUIRE(rc.validate(42));
    REQUIRE_FALSE(rc.validate(42));
}

TEST_CASE("ReplayCounter word boundary nonces", "[replay_counter]")
{
    ReplayCounter rc;
    const vector<uint64_t> probes = {0, 1, 62, 63, 64, 65, 126, 127, 128, 8191, 8192, 16'383, 16'384};
    for (const uint64_t n : probes) {
        REQUIRE(rc.validate(n));
        REQUIRE_FALSE(rc.validate(n));
    }
}

TEST_CASE("ReplayCounter large forward jump clears stale history", "[replay_counter]")
{
    ReplayCounter rc;
    REQUIRE(rc.validate(0));
    const uint64_t jump = 50'000;
    REQUIRE(rc.validate(jump));

    const uint64_t oldest = oldestAcceptableZeroBased(jump + 1);
    REQUIRE_FALSE(rc.validate(oldest - 1));
    REQUIRE(rc.validate(oldest));
    REQUIRE(rc.validate(jump - 1));
    REQUIRE_FALSE(rc.validate(oldest));
}

TEST_CASE("ReplayCounter full window sequential sweep", "[replay_counter]")
{
    ReplayCounter rc;
    const uint64_t start = 100'000;
    for (uint64_t offset = 0; offset <= kCounterWindowSize; ++offset) {
        const uint64_t nonce = start + offset;
        REQUIRE(rc.validate(nonce));
        REQUIRE_FALSE(rc.validate(nonce));
    }
    REQUIRE_FALSE(rc.validate(start));
    REQUIRE(rc.validate(start + kCounterWindowSize + 1));
}

TEST_CASE("ReplayCounter rejects near uint64 wrap", "[replay_counter]")
{
    ReplayCounter rc;
    rc.counter = kRejectAfterMessages + 1;
    REQUIRE(rc.exhausted());
    REQUIRE_FALSE(rc.validate(0));

    rc.reset();
    REQUIRE_FALSE(rc.validate(kRejectAfterMessages));
    REQUIRE(rc.validate(kRejectAfterMessages - 1));
}

TEST_CASE("ReplayCounter matches naive reference on random traffic", "[replay_counter][.slow]")
{
    mt19937_64 rng(0x64798192beefcafeULL);
    uniform_int_distribution<int> actionDist(0, 99);
    uniform_int_distribution<uint64_t> nonceDist(0, 200'000);

    ReplayCounter fast;
    NaiveReplayCounter naive;

    for (int step = 0; step < 500'000; ++step) {
        const int action = actionDist(rng);
        if (action < 5) {
            fast.reset();
            naive.reset();
            continue;
        }

        uint64_t nonce = 0;
        if (action < 60) {
            nonce = nonceDist(rng);
        } else if (action < 85) {
            nonce = max(fast.counter, naive.counter) + static_cast<uint64_t>(nonceDist(rng) % 500);
            if (nonce > 0) {
                --nonce;
            }
        } else {
            const uint64_t maxSeen = max(fast.counter, naive.counter);
            if (maxSeen > 0) {
                nonce = maxSeen - 1 - static_cast<uint64_t>(nonceDist(rng) % 500);
            }
        }

        const bool fastResult = fast.validate(nonce);
        const bool naiveResult = naive.validate(nonce);
        REQUIRE(fastResult == naiveResult);
    }
}

TEST_CASE("ReplayCounter matches naive reference on shuffled window", "[replay_counter]")
{
    ReplayCounter fast;
    NaiveReplayCounter naive;

    const uint64_t base = 1'000'000;
    vector<uint64_t> window(kCounterWindowSize + 1);
    iota(window.begin(), window.end(), base);

    mt19937_64 rng(0x7769726567756172ULL);
    shuffle(window.begin(), window.end(), rng);

    for (const uint64_t nonce : window) {
        REQUIRE(fast.validate(nonce));
        REQUIRE(naive.validate(nonce));
    }

    for (const uint64_t nonce : window) {
        REQUIRE_FALSE(fast.validate(nonce));
        REQUIRE_FALSE(naive.validate(nonce));
    }
}

TEST_CASE("ReplayCounter matches naive reference across bitmap wrap", "[replay_counter]")
{
    ReplayCounter fast;
    NaiveReplayCounter naive;

    const uint64_t start = static_cast<uint64_t>(kCounterWords) * static_cast<uint64_t>(kCounterWordBits) - 64;
    vector<uint64_t> nonces(kCounterWindowSize + 256);
    iota(nonces.begin(), nonces.end(), start);

    mt19937_64 rng(0x7769726567756172ULL);
    shuffle(nonces.begin(), nonces.end(), rng);

    for (const uint64_t nonce : nonces) {
        REQUIRE(fast.validate(nonce) == naive.validate(nonce));
    }

    shuffle(nonces.begin(), nonces.end(), rng);
    for (const uint64_t nonce : nonces) {
        REQUIRE_FALSE(fast.validate(nonce));
        REQUIRE_FALSE(naive.validate(nonce));
    }
}

TEST_CASE("ReplayCounter stress sequential advance", "[replay_counter][.slow]")
{
    ReplayCounter rc;
    for (uint64_t i = 0; i < 1'000'000; ++i) {
        REQUIRE(rc.validate(i));
    }
    REQUIRE(rc.counter == 1'000'000);
}

TEST_CASE("ReplayCounter stress alternating reorder", "[replay_counter][.slow]")
{
    ReplayCounter rc;
    for (uint64_t i = 0; i < 200'000; ++i) {
        const uint64_t even = i * 2;
        const uint64_t odd = even + 1;
        REQUIRE(rc.validate(odd));
        REQUIRE(rc.validate(even));
        REQUIRE_FALSE(rc.validate(even));
    }
}
