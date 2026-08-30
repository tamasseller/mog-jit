#include "runtime.h"
#include "proc_scan.h"
#include "decode_instr.h"
#include "abi_strategy.h"

uint32_t Runtime::loadProgram(const uint8_t *programBytes, uint32_t programSize, uint32_t bodyOffset)
{
    if(procCount > jitc::MAX_PROC_IDX + 1)
    {
        return RESOURCE_LIMIT_PROC_COUNT;
    }

    uint32_t pos = bodyOffset;
    for(uint32_t i = 0; i < procCount; i++)
    {
        assert(pos < programSize); // GCOV_EXCL_LINE — malformed/truncated program, matching decode_instr.cpp's own convention
        
        uint32_t argCount = jitc::decodeLeb128(programBytes, pos, pos);
        uint32_t bodyStart = pos;

        jitc::BodyScanResult scan = jitc::scanProcBody(programBytes, programSize, bodyStart, stackLimit);

        if(!scan.ok)
        {
            return scan.failCode; 
        }
        if(argCount > ProcSlot::MAX_ARG_COUNT)
        {
            return RESOURCE_LIMIT_ARG_COUNT;
        }
        if(scan.bodyBytes > ProcSlot::MAX_BODY_BYTES)
        {
            return RESOURCE_LIMIT_BODY_BYTES;
        }

        ProcSlot &s = slot(i);
        s.setCodePtr(trampolineAddr);
        s.lastUsed = 0;
        s.bodyPtr = (uint32_t)(uintptr_t)(programBytes + bodyStart);
        s.setStaticInfo(argCount, scan.bodyBytes, scan.needsLRSave);

        pos = bodyStart + scan.bodyBytes;
    }

    return 0;
}

uint32_t Runtime::pushStackFloor(uint32_t margin)
{
    register uint32_t sp asm("sp");

    /* Where the arena has actually reached, not where it was allowed to: the
     * two sides check each other, and whichever got there first wins. */
    const uint32_t floor = (arenaOverlapsStack && stackLimit < arenaCursor) ? arenaCursor : stackLimit;

    if(sp <= floor + margin + interruptReserve)
    {
        runtimeBail(this, RESOURCE_EXHAUSTED_TRANSLATOR_STACK);
    }

    const uint32_t outerFloor = liveStackFloor;
    liveStackFloor = sp - margin;
    return outerFloor;
}

void Runtime::popStackFloor(uint32_t outerFloor)
{
    liveStackFloor = outerFloor;
}

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

void Runtime::evict(uint32_t idx, const uint16_t *end)
{
    uint32_t victimAddr = slot(idx).codePtr & ~1u;
    uint32_t victimSize = occupiedSizeOf(idx);
    uint32_t gapEnd = victimAddr + victimSize;
    uint32_t tailLen = (uint32_t)end - gapEnd;

    uint16_t *dst = (uint16_t *)(uintptr_t)victimAddr;
    const uint16_t *src = (const uint16_t *)(uintptr_t)gapEnd;

    for(uint32_t i = 0; i < tailLen / 2; i++) 
    {
        dst[i] = src[i];
    }

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
    const uint32_t ceiling = arenaCeiling();

    assert((uint32_t)end <= ceiling);

    if((uint32_t)end == ceiling)
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