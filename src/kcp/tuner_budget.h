#ifndef QTNG_KCP_TUNER_BUDGET_H
#define QTNG_KCP_TUNER_BUDGET_H

// One Tuner period of the send-budget update. Kept header-only so tests can
// drive the loss/delay decision without a live ikcp session.
//
// lossBasedBudget true (KcpSocket default): loss above 5% shrinks, but the
// loss-only floor is initialBudgetSegs, not a collapsed BDP. Multipath loss
// used to pin the window at 8 segments once deliveryBps had already fallen.
// lossBasedBudget false (SLOW): loss does not shrink; only queuing delay does.
//
// minSendBudgetSegs is the floor used when nothing is wrong. 0 means
// max(initialBudgetSegs, 64). The floor yields only to loss or queuing delay.
// appLimited never shrinks: a quiet or source-limited period must not be
// read back as "the path can only do this many segments".

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
                                       std::uint64_t freezeGrowthUntil, std::uint32_t period,
                                       std::uint32_t minSendBudgetSegs = 0, bool appLimited = false)
{
    const std::uint32_t budgetFloor = minSendBudgetSegs == 0 ? std::max(initialBudgetSegs, 64u)
                                                             : std::max(minSendBudgetSegs, 8u);
    const bool lossReduce = lossBasedBudget && lossRate > 0.05;
    // App-limited samples are not evidence. A zero queue or a send rate far
    // under the window must not be allowed to cut it.
    const bool reduce = !appLimited && (delayReduce || lossReduce);

    if (appLimited) {
        const std::uint32_t goal = std::max(targetBudget, budgetFloor);
        if (sendBudgetSegs < goal) {
            if (coldStartTicks < 3) {
                sendBudgetSegs = std::min(memoryCapSegs,
                                          std::max(sendBudgetSegs * 2, std::max(initialBudgetSegs, budgetFloor)));
                if (sendBudgetSegs > goal) {
                    sendBudgetSegs = goal;
                }
                ++coldStartTicks;
            } else if (now >= freezeGrowthUntil || sendBudgetSegs < budgetFloor) {
                const std::uint32_t step = std::max(1u, (goal - sendBudgetSegs) / 8);
                sendBudgetSegs = std::min(goal, sendBudgetSegs + step);
            }
        }
        sendBudgetSegs = std::max(8u, std::min(sendBudgetSegs, memoryCapSegs));
        KcpBudgetStep out;
        out.sendBudgetSegs = sendBudgetSegs;
        out.coldStartTicks = coldStartTicks;
        out.freezeGrowthUntil = freezeGrowthUntil;
        return out;
    }

    if (coldStartTicks < 3 && !reduce) {
        sendBudgetSegs = std::min(memoryCapSegs,
                                  std::max(sendBudgetSegs * 2, std::max(initialBudgetSegs, budgetFloor)));
        ++coldStartTicks;
    } else if (reduce) {
        // Delay cut is stronger (×0.7). Loss-only uses ×0.85. Never stack both.
        const std::uint32_t scaled = delayReduce ? (sendBudgetSegs * 7) / 10
                                                 : (sendBudgetSegs * 85) / 100;
        // Loss or delay is the only evidence that may land below budgetFloor.
        // Loss-only still stops at the cold-start budget, not a collapsed BDP.
        const std::uint32_t evidenceFloor = delayReduce ? bdpSegs : std::max(initialBudgetSegs, bdpSegs);
        sendBudgetSegs = std::max(evidenceFloor, scaled);
        freezeGrowthUntil = now + 2ull * period;
    } else if (now >= freezeGrowthUntil) {
        const std::uint32_t goal = std::max(targetBudget, budgetFloor);
        if (sendBudgetSegs < goal) {
            const std::uint32_t step = std::max(1u, (goal - sendBudgetSegs) / 8);
            sendBudgetSegs = std::min(goal, sendBudgetSegs + step);
        } else if (sendBudgetSegs > goal) {
            sendBudgetSegs = goal;
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
