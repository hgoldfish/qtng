#ifndef QTNG_KCP_TUNER_BUDGET_H
#define QTNG_KCP_TUNER_BUDGET_H

// One Tuner period of the send-budget update, BBRv1-flavoured. Kept header-only
// so tests can drive the decision without a live ikcp session.
//
// Capacity is decided in runTuner (btlBw * srttMin * gain); this step only moves
// the tracked budget toward that target:
//
//   * Startup grows multiplicatively (BBR's slow start); every other state walks
//     toward the target. Outside Startup the target is a ceiling.
//   * appLimited never shrinks: a quiet or source-limited period is not evidence
//     about the path and must not be read back as its capacity.
//
// Loss is deliberately not part of this step or of the ceiling below. A lossy
// path must not be read as one that cannot carry data: the budget's own target
// is gain * bdpSegs and bdpSegs comes from the achieved rate, which the window
// itself sets, so anything that shrinks the window on loss also shrinks the
// estimate that sizes it.

#include <algorithm>
#include <cstdint>

namespace qtng {

inline std::uint32_t stepKcpSendBudgetBbr(std::uint32_t sendBudgetSegs, std::uint32_t targetSegs,
                                          std::uint32_t memoryCapSegs, bool appLimited, bool startupGrow,
                                          std::uint32_t minSegs = 8)
{
    const std::uint32_t cap = std::max(minSegs, memoryCapSegs);
    const std::uint32_t goal = std::min(cap, std::max(minSegs, targetSegs));

    if (appLimited) {
        // Not evidence about the path: never shrink, only clamp.
        return std::min(std::max(sendBudgetSegs, minSegs), cap);
    }

    if (sendBudgetSegs < goal) {
        const std::uint32_t step = startupGrow ? sendBudgetSegs : std::max(1u, (goal - sendBudgetSegs) / 8);
        sendBudgetSegs = std::min(goal, sendBudgetSegs + std::max(1u, step));
    } else if (sendBudgetSegs > goal) {
        // Startup ramps freely: its target is the current estimate and clamping
        // down would undo the ramp. Everywhere else the target is a ceiling.
        if (!startupGrow) {
            sendBudgetSegs = goal;
        }
    }

    if (sendBudgetSegs > cap) {
        sendBudgetSegs = cap;
    }
    if (sendBudgetSegs < minSegs) {
        sendBudgetSegs = minSegs;
    }
    return sendBudgetSegs;
}

// Multiplicative step of ProbeBW's up-probe. 5/4 converges to a path's working
// point in a handful of periods without the overshoot a doubling would cause:
// there is no pacer, so the whole window leaves in one burst and every extra
// segment is extra queueing on a shallow buffer.
static constexpr std::uint32_t kProbeGrowNum = 5;
static constexpr std::uint32_t kProbeGrowDen = 4;

// Back-off applied when the queue starts building. Gentler than the growth step
// on purpose: it only has to undo one probe, and a hard cut would hand the
// window back to whatever the rate estimate implies.
static constexpr std::uint32_t kProbeTrimNum = 9;
static constexpr std::uint32_t kProbeTrimDen = 10;

// ProbeBW's window ceiling. Kept independent of the BDP on purpose: the send
// budget's target is gain * bdpSegs and bdpSegs is derived from the achieved
// rate, which the window itself sets, so on a lossy path the target becomes a
// function of the window it is sizing -- a low eta (delivered / window-full)
// drags the window down until it sits at the floor, and the lower window keeps
// eta low. This ceiling has no such feedback: it moves only on direct evidence
// about the queue, and loss never touches it.
//
// grow: the pipe is full and no queue is standing. trim: queuing delay.
// Neither: hold. Seeded from floorSegs the first time (ceiling == 0).
// Never below floorSegs -- so a legitimately grown BDP is not starved -- and
// never above memoryCapSegs.
inline std::uint32_t stepKcpProbeCeiling(std::uint32_t ceiling, std::uint32_t floorSegs,
                                         std::uint32_t memoryCapSegs, bool grow, bool trim)
{
    const std::uint32_t cap = std::max(floorSegs, memoryCapSegs);

    if (ceiling == 0) {
        return std::min(floorSegs, cap);
    }

    if (trim) {
        ceiling = (ceiling * kProbeTrimNum) / kProbeTrimDen;
    } else if (grow) {
        const std::uint32_t grown = std::max(ceiling + 1, (ceiling * kProbeGrowNum) / kProbeGrowDen);
        ceiling = std::min(cap, grown);
    }

    return std::min(cap, std::max(ceiling, floorSegs));
}

}  // namespace qtng

#endif
