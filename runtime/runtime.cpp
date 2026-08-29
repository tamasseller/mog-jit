#include "runtime.h"

void Runtime::markCompiled(uint32_t idx, uint32_t dest, uint32_t lruTick)
{
    ProcSlot &entry = slot(idx);
    entry.setCodePtr(dest);
    entry.lastUsed = lruTick;
}

int Runtime::findEvictionVictim(uint32_t now) const
{
    int victim = -1;
    uint32_t oldestAge = 0;
    for(uint32_t i = 0; i < procCount; i++)
    {
        if(!isResident(i))
        {
            continue;
        }
        uint32_t age = now - slot(i).lastUsed;
        if(victim < 0 || age > oldestAge)
        {
            oldestAge = age;
            victim = (int)i;
        }
    }
    return victim;
}

uint32_t Runtime::occupiedSizeOf(uint32_t idx) const
{
    uint32_t addr = slot(idx).codePtr & ~1u;
    uint32_t gapEnd = arenaCursor;
    for(uint32_t i = 0; i < procCount; i++)
    {
        if(!isResident(i))
        {
            continue;
        }
        uint32_t a = slot(i).codePtr & ~1u;
        if(a > addr && a < gapEnd)
        {
            gapEnd = a;
        }
    }
    return gapEnd - addr;
}

void Runtime::evict(uint32_t idx, const uint16_t *end)
{
    uint32_t victimAddr = slot(idx).codePtr & ~1u;
    uint32_t victimSize = occupiedSizeOf(idx);
    uint32_t gapEnd = victimAddr + victimSize;
    uint32_t tailLen = (uint32_t)end - gapEnd;

    memmove((void *)(uintptr_t)victimAddr, (void *)(uintptr_t)gapEnd, tailLen);
    arenaCursor -= victimSize;

    ProcSlot &victimEntry = slot(idx);
    victimEntry.setCodePtr(trampolineAddr);
    victimEntry.lastUsed = 0;

    for(uint32_t i = 0; i < procCount; i++)
    {
        ProcSlot &entry = slot(i);
        if(entry.codePtr != trampolineAddr && (entry.codePtr & ~1u) >= gapEnd)
        {
            entry.slideCodePtr(victimSize);
        }
    }
}

uint16_t* Runtime::ensureSpace(const uint16_t* end, uint32_t lruTick)
{
    assert((uint32_t)end <= arenaEnd);

    if((uint32_t)end == arenaEnd)
    {
        int victim = findEvictionVictim(lruTick);
        if(victim < 0)
        {
            runtimeBail(this, RESOURCE_EXHAUSTED_ARENA);
            return nullptr;
        }


        evict((uint32_t)victim, end);
    }

    return (uint16_t *)(uintptr_t)arenaCursor;
}