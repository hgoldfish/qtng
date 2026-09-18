#ifndef QTNG_KCP_TUNER_BUDGET_H
#define QTNG_KCP_TUNER_BUDGET_H

// One Tuner period of the send-budget update. Kept header-only so tests can
// drive the loss/delay decision without a live ikcp session.
//
// lossBasedBudget true (KcpSocket default): loss above 5% shrinks, but the
// loss-only floor is initialBudgetSegs, not a collapsed BDP. Multipath loss
// used to pin the window at 8 segments once deliveryBps had already fallen.
// lossBasedBudget false (SLOW): loss does not shrink; only queuing delay does.

#include <algorithm>
#include <cstdint>

namespace qtng {

struct KcpBudgetStep {
    std::uint32_t sendBudgetSegs = 0;
    std::uint32_t coldStartTicks = 0;
    std::uint64_t freezeGrowthUntil = 0;
};

inline KcpBudgetStep stepKcpSendBudget(std::uint32_t sendBudgetSegs, std::uint32_t bdpSegs,
                                       std::uint32_t memoryCapSegs, std::uint32_t initialBudgetSegs,
                                       std::uint32_t targetBudget, double lossRate, bool delayReduce,
                                       bool lossBasedBudget, std::uint32_t coldStartTicks, std::uint64_t now,
                                       std::uint64_t freezeGrowthUntil, std::uint32_t period)
{
    const bool lossReduce = lossBasedBudget && lossRate > 0.05;
    const bool reduce = delayReduce || lossReduce;
    if (coldStartTicks < 3 && !reduce) {
        sendBudgetSegs = std::min(memoryCapSegs, std::max(sendBudgetSegs * 2, initialBudgetSegs));
        ++coldStartTicks;
    } else if (reduce) {
        // Delay cut is stronger (×0.7). Loss-only uses ×0.85. Never stack both.
        const std::uint32_t scaled = delayReduce ? (sendBudgetSegs * 7) / 10
                                                 : (sendBudgetSegs * 85) / 100;
        // Loss-only must not follow a collapsed BDP down to 8 segments.
        const std::uint32_t floor = delayReduce ? bdpSegs : std::max(initialBudgetSegs, bdpSegs);
        sendBudgetSegs = std::max(floor, scaled);
        freezeGrowthUntil = now + 2ull * period;
    } else if (now >= freezeGrowthUntil) {
        if (sendBudgetSegs < targetBudget) {
            const std::uint32_t step = std::max(1u, (targetBudget - sendBudgetSegs) / 8);
            sendBudgetSegs = std::min(targetBudget, sendBudgetSegs + step);
        } else if (sendBudgetSegs > targetBudget) {
            sendBudgetSegs = targetBudget;
        }
    }
    sendBudgetSegs = std::max(8u, std::min(sendBudgetSegs, memoryCapSegs));
    KcpBudgetStep out;
    out.sendBudgetSegs = sendBudgetSegs;
    out.coldStartTicks = coldStartTicks;
    out.freezeGrowthUntil = freezeGrowthUntil;
    return out;
}

}  // namespace qtng

#endif
