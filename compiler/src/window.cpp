#include "window.h"
#include "emitter.h"
#include "accstate.h"
#include "shape.h"
#include "armv6.h"

#include <algorithm>

namespace jitc
{

using R = ArmV6M::LoReg;

bool inWindow(uint32_t tos, uint32_t k)
{
    return tos - k <= WINDOW_SIZE;
}

uint32_t physReg(uint32_t k)
{
    return WINDOW_BASE + (WINDOW_SIZE - 1 - (k % WINDOW_SIZE));
}

static uint32_t spilledCount(uint32_t tos)
{
    return tos > WINDOW_SIZE ? tos - WINDOW_SIZE : 0;
}

static ArmV6M::LoRegs oneReg(uint32_t r)
{
    ArmV6M::LoRegs regs{0};
    regs.add(R((uint16_t)r));
    return regs;
}

// The (unordered) register set holding k = bottom .. bottom+count-1 — a
// valid PUSH/POP mask regardless of wrap.
static ArmV6M::LoRegs regsFor(uint32_t bottom, uint32_t count)
{
    ArmV6M::LoRegs regs{0};
    for(uint32_t i = 0; i < count; i++)
    {
        regs.add(R((uint16_t)physReg(bottom + i)));
    }
    return regs;
}

struct RegRun
{
    uint32_t regs[WINDOW_SIZE];
    uint32_t count;
};

struct RegRuns
{
    RegRun runs[2];
    uint32_t runCount;
};

static ArmV6M::LoRegs toLoRegs(const RegRun &run)
{
    ArmV6M::LoRegs regs{0};
    for(uint32_t i = 0; i < run.count; i++)
    {
        regs.add(R((uint16_t)run.regs[i]));
    }
    return regs;
}

// k = bottom .. bottom+count-1 (count <= WINDOW_SIZE), split at the point
// (if any) where physReg wraps from r4 back to r7 — at most two
// contiguous, k-ascending-but-register-descending runs.
static RegRuns windowRuns(uint32_t bottom, uint32_t count)
{
    RegRuns result{};
    if(count == 0)
    {
        result.runCount = 0;
        return result;
    }
    uint32_t phase = bottom % WINDOW_SIZE;
    uint32_t preWrapLen = std::min(count, WINDOW_SIZE - phase);
    for(uint32_t i = 0; i < preWrapLen; i++)
    {
        result.runs[0].regs[i] = physReg(bottom + i);
    }
    result.runs[0].count = preWrapLen;
    uint32_t postWrapLen = count - preWrapLen;
    if(postWrapLen == 0)
    {
        result.runCount = 1;
        return result;
    }
    for(uint32_t i = 0; i < postWrapLen; i++)
    {
        result.runs[1].regs[i] = physReg(bottom + preWrapLen + i);
    }
    result.runs[1].count = postWrapLen;
    result.runCount = 2;
    return result;
}

// Push k = bottom .. bottom+count-1 such that the largest k ends up closest
// to the resulting sp: push the pre-wrap run first, the post-wrap run
// second.
static void pushLargestKClosest(Emitter &e, uint32_t bottom, uint32_t count)
{
    RegRuns rr = windowRuns(bottom, count);
    for(uint32_t i = 0; i < rr.runCount; i++)
    {
        e.emit(ArmV6M::push(toLoRegs(rr.runs[i])));
    }
}

// Pop k = bottom .. bottom+count-1 — genuinely historical spilled data — via
// at most two batched POPs, runs consumed in reverse (larger-k,
// closer-to-sp run first).
static void popRuns(Emitter &e, uint32_t bottom, uint32_t count)
{
    RegRuns rr = windowRuns(bottom, count);
    for(uint32_t i = rr.runCount; i > 0; i--)
    {
        e.emit(ArmV6M::pop(toLoRegs(rr.runs[i - 1])));
    }
}

// Pure spillOffset math, no adjustment — Window::spillOffset (below) is
// what every real caller uses; this stays a free function only because the
// adjustment itself needs it as a building block.
static uint32_t rawSpillOffset(uint32_t tos, uint32_t k)
{
    return 4 * (spilledCount(tos) - 1 - k);
}

uint32_t Window::spillOffset(uint32_t k) const
{
    uint32_t raw = rawSpillOffset(tos, k);
    return (savesLR && k < initialSpilledCount) ? raw + 4 : raw;
}

void Window::discardWindow(Emitter &e) const
{
    uint32_t spilled = spilledCount(tos) - (savesLR ? initialSpilledCount : 0);
    if(spilled > 0)
    {
        e.emit(ArmV6M::incrSp(ArmV6M::Uoff<2, 7>((uint16_t)(4 * spilled))));
    }
}

void Window::pushValue(Emitter &e, AccState &accState)
{
    bool pushEvicts = tos >= WINDOW_SIZE;
    uint32_t evictedByPush = tos - WINDOW_SIZE;
    if(pushEvicts)
    {
        e.emit(ArmV6M::push(oneReg(physReg(evictedByPush))));
    }
    accState.flush(e, physReg(tos));
    tos += 1;
}

void Window::finishPop(Emitter &e)
{
    bool popUncovers = (tos - 1) >= WINDOW_SIZE;
    uint32_t uncoveredByPop = tos - 1 - WINDOW_SIZE;
    if(popUncovers)
    {
        e.emit(ArmV6M::pop(oneReg(physReg(uncoveredByPop))));
    }
    tos -= 1;
}

void spillForCall(Emitter &e, Window &window, uint32_t stackArgs)
{
    uint32_t w = std::min(window.tos, WINDOW_SIZE);
    uint32_t m = std::min(stackArgs, w);
    uint32_t bottom = window.tos - w;
    uint32_t base = window.tos - m;

    if(base > bottom)
    {
        e.emit(ArmV6M::push(regsFor(bottom, base - bottom)));
    }
    pushLargestKClosest(e, base, m);
}

void fillCalleeArgs(Emitter &e, uint32_t stackArgs)
{
    uint32_t m = std::min(stackArgs, WINDOW_SIZE - 1);
    if(m == 0)
    {
        return;
    }
    popRuns(e, stackArgs - m, m);
}

void restoreWindow(Emitter &e, Window &window, uint32_t targetTos)
{
    uint32_t spilledNow = spilledCount(window.tos);
    uint32_t spilledTarget = spilledCount(targetTos);
    uint32_t reloadTop = std::min(spilledNow, targetTos);

    if(spilledNow > reloadTop)
    {
        e.emit(ArmV6M::incrSp(ArmV6M::Uoff<2, 7>((uint16_t)(4 * (spilledNow - reloadTop)))));
    }
    popRuns(e, spilledTarget, reloadTop - spilledTarget);

    window.tos = targetTos;
}

void reloadAfterCall(Emitter &e, Window &window, uint32_t targetTos)
{
    uint32_t w = std::min(window.tos, WINDOW_SIZE);
    uint32_t bottom = window.tos - w;
    uint32_t count = std::min(targetTos, WINDOW_SIZE);
    uint32_t deeperFloor = targetTos - count;

    if(targetTos > bottom)
    {
        e.emit(ArmV6M::pop(regsFor(bottom, targetTos - bottom)));
    }

    uint32_t historicalTop = std::min(bottom, targetTos);
    popRuns(e, deeperFloor, historicalTop - deeperFloor);

    window.tos = targetTos;
}

} // namespace jitc
