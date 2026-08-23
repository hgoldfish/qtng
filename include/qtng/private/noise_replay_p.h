#ifndef QTNG_NOISE_REPLAY_P_H
#define QTNG_NOISE_REPLAY_P_H

#include <array>
#include <cstdint>

namespace qtng {

// WireGuard receive-side replay window (RFC 6479): 8192-bit bitmap, 64 redundant
// bits, reject before the counter can wrap through the window.
constexpr int kCounterWordBits = 64;
constexpr int kCounterBitsTotal = 8192;
constexpr int kCounterWords = kCounterBitsTotal / kCounterWordBits;
constexpr uint64_t kCounterWindowSize = static_cast<uint64_t>(kCounterBitsTotal - kCounterWordBits);
constexpr uint64_t kRejectAfterMessages = ~uint64_t(0) - kCounterWindowSize - 1;

struct ReplayCounter
{
    uint64_t counter = 0;
    std::array<uint64_t, static_cast<size_t>(kCounterWords)> backtrack{};

    void reset()
    {
        counter = 0;
        backtrack.fill(0);
    }

    bool exhausted() const
    {
        return counter >= kRejectAfterMessages + 1;
    }

    bool validate(uint64_t theirCounter)
    {
        if (exhausted() || theirCounter >= kRejectAfterMessages) {
            return false;
        }
        // Packet nonce is 0-based; WireGuard stores a 1-based counter so 0 means unused.
        ++theirCounter;
        if (kCounterWindowSize + theirCounter < counter) {
            return false;
        }
        const uint64_t wordBits = static_cast<uint64_t>(kCounterWordBits);
        const uint64_t index = theirCounter / wordBits;
        if (theirCounter > counter) {
            const uint64_t indexCurrent = counter / wordBits;
            uint64_t top = index - indexCurrent;
            if (top > static_cast<uint64_t>(kCounterWords)) {
                top = static_cast<uint64_t>(kCounterWords);
            }
            for (uint64_t i = 1; i <= top; ++i) {
                backtrack[static_cast<size_t>((i + indexCurrent) & (kCounterWords - 1))] = 0;
            }
            counter = theirCounter;
        }
        const uint64_t word = index & (kCounterWords - 1);
        const uint64_t bit = uint64_t(1) << (theirCounter % wordBits);
        if (backtrack[static_cast<size_t>(word)] & bit) {
            return false;
        }
        backtrack[static_cast<size_t>(word)] |= bit;
        return true;
    }
};

}  // namespace qtng

#endif  // QTNG_NOISE_REPLAY_P_H
