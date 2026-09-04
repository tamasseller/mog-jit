#include "window.h"
#include "assembler.h"
#include "accstate.h"
#include "shape.h"
#include "armv6.h"
#include "runtime.h"

#include <algorithm>

namespace jitc
{

using R = ArmV6M::LoReg;

bool inWindow(uint32_t tos, uint32_t k)
{
    return tos - k <= WINDOW_SIZE && k < tos;
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

static RegRuns windowRuns(uint32_t bottom, uint32_t count)
{
    RegRuns result;
    result.runCount = 0;
    result.runs[0].count = 0;
    result.runs[1].count = 0;

    if(count == 0)
    {
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

static void pushLargestKClosest(Assembler &e, uint32_t bottom, uint32_t count)
{
    RegRuns rr = windowRuns(bottom, count);
    for(uint32_t i = 0; i < rr.runCount; i++)
    {
        e.emit(ArmV6M::push(toLoRegs(rr.runs[i])));
    }
}

static uint32_t popRuns(Assembler &e, uint32_t bottom, uint32_t count)
{
    uint32_t reloaded = 0;
    RegRuns rr = windowRuns(bottom, count);
    for(uint32_t i = rr.runCount; i > 0; i--)
    {
        const auto regs = toLoRegs(rr.runs[i - 1]);
        for(uint32_t j = 0; j < rr.runs[i - 1].count; j++)
        {
            reloaded |= Effect::bit(rr.runs[i - 1].regs[j]);
        }
        e.emit(ArmV6M::pop(regs));
    }
    return reloaded;
}

static uint32_t rawSpillOffset(uint32_t tos, uint32_t k)
{
    return 4 * (spilledCount(tos) - 1 - k);
}

ArmV6M::Uoff<2, 8> spillImm(Assembler &a, uint32_t byteOffset)
{
    if(!ArmV6M::Uoff<2, 8>::isInRange(byteOffset))
    {
        runtimeBail(&a.runtime, RESOURCE_LIMIT_SPILL_OFFSET);
    }
    return ArmV6M::Uoff<2, 8>((uint16_t)byteOffset);
}

uint32_t Window::spillOffset(uint32_t k) const
{
    uint32_t raw = rawSpillOffset(tos, k);
    return (savesLR && k < initialSpilledCount) ? raw + 4 : raw;
}

Effect Window::discard(Assembler &e) const
{
    uint32_t spilled = spilledCount(tos) - (savesLR ? initialSpilledCount : 0);
    if(spilled > 0)
    {
        uint32_t bytes = 4 * spilled;
        if(!ArmV6M::Uoff<2, 7>::isInRange(bytes))
        {
            runtimeBail(&e.runtime, RESOURCE_LIMIT_WINDOW_RECLAIM);
        }
        e.emit(ArmV6M::incrSp(ArmV6M::Uoff<2, 7>((uint16_t)bytes)));
    }

    return Effect::none(); // only sp moved
}

Effect Window::pushValue(Assembler &e, AccState &accState)
{
    bool pushEvicts = tos >= WINDOW_SIZE;
    uint32_t evictedByPush = tos - WINDOW_SIZE;
    if(pushEvicts)
    {
        e.emit(ArmV6M::push(oneReg(physReg(evictedByPush))));
    }
    accState.flush(e, physReg(tos)); // records its own effect
    tos += 1;
    return Effect::none();
}

Effect Window::pushFrom(Assembler &e, AccState &accState, uint32_t srcReg)
{
    if(tos >= WINDOW_SIZE)
    {
        e.emit(ArmV6M::push(oneReg(physReg(tos - WINDOW_SIZE))));
    }

    uint32_t dst = physReg(tos);
    accState.resolveIfLiveIn(e, dst);

    tos += 1;

    if(dst == srcReg)
    {
        return Effect::none();
    }

    e.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dst), ArmV6M::AnyReg((uint16_t)srcReg)));
    return Effect::into(dst, false);
}

Effect Window::finishPop(Assembler &e)
{
    bool popUncovers = (tos - 1) >= WINDOW_SIZE;
    uint32_t uncoveredByPop = tos - 1 - WINDOW_SIZE;
    uint32_t reloaded = 0;

    if(popUncovers)
    {
        reloaded = Effect::bit(physReg(uncoveredByPop));
        e.emit(ArmV6M::pop(oneReg(physReg(uncoveredByPop))));
    }

    tos -= 1;
    return Effect::writes(reloaded);
}

Effect Window::spillForCall(Assembler &e, uint32_t stackArgs)
{
    uint32_t w = std::min(this->tos, WINDOW_SIZE);
    uint32_t m = std::min(stackArgs, w);
    uint32_t bottom = this->tos - w;
    uint32_t base = this->tos - m;

    if(base > bottom)
    {
        e.emit(ArmV6M::push(regsFor(bottom, base - bottom)));
    }
    pushLargestKClosest(e, base, m);
    return Effect::none(); // pushes read registers, they do not write any
}

Effect Window::fillCalleeArgs(Assembler &e, uint32_t stackArgs)
{
    uint32_t m = std::min(stackArgs, WINDOW_SIZE - 1);
    if(m == 0)
    {
        return Effect::none();
    }

    return Effect::writes(popRuns(e, stackArgs - m, m));
}

Effect Window::restore(Assembler &e, uint32_t targetTos)
{
    uint32_t spilledNow = spilledCount(this->tos);
    uint32_t spilledTarget = spilledCount(targetTos);
    uint32_t reloadTop = std::min(spilledNow, targetTos);

    if(spilledNow > reloadTop)
    {
        uint32_t bytes = 4 * (spilledNow - reloadTop);
        if(!ArmV6M::Uoff<2, 7>::isInRange(bytes))
        {
            runtimeBail(&e.runtime, RESOURCE_LIMIT_WINDOW_RECLAIM);
        }
        e.emit(ArmV6M::incrSp(ArmV6M::Uoff<2, 7>((uint16_t)bytes)));
    }

    const uint32_t reloaded = popRuns(e, spilledTarget, reloadTop - spilledTarget);

    this->tos = targetTos;
    return Effect::writes(reloaded);
}

Effect Window::reloadAfterCall(Assembler &e, uint32_t targetTos)
{
    uint32_t w = std::min(this->tos, WINDOW_SIZE);
    uint32_t bottom = this->tos - w;
    uint32_t count = std::min(targetTos, WINDOW_SIZE);
    uint32_t deeperFloor = targetTos - count;

    uint32_t reloaded = 0;

    if(targetTos > bottom)
    {
        for(uint32_t k = bottom; k < targetTos; k++)
        {
            reloaded |= Effect::bit(physReg(k));
        }
        e.emit(ArmV6M::pop(regsFor(bottom, targetTos - bottom)));
    }

    uint32_t historicalTop = std::min(bottom, targetTos);
    reloaded |= popRuns(e, deeperFloor, historicalTop - deeperFloor);

    this->tos = targetTos;
    return Effect::writes(reloaded);
}

} // namespace jitc
