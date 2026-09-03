#include "runtime.h"
#include "proc_scan.h"
#include "decode_instr.h"
#include "abi_strategy.h"

/* One cursor for the whole directory: the procedures sit end to end, so each
 * body's scan leaves it exactly where the next arg_count starts. */
uint32_t Runtime::loadProgram(BcReader &r)
{
    if(dispatch.getProcCount() > jitc::MAX_PROC_IDX + 1)
    {
        return RESOURCE_LIMIT_PROC_COUNT;
    }

    for(uint32_t i = 0; i < dispatch.getProcCount(); i++)
    {
        assert(!r.atEnd()); // GCOV_EXCL_LINE — malformed/truncated program, matching decode_instr.cpp's own convention

        uint32_t argCount = 0;
        jitc::decodeLeb128(r, argCount);

        const BcHandle body = r.here();

        jitc::BodyScanResult scan = jitc::scanProcBody(r, memory.getStackLimit());

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

        ProcSlot &s = dispatch.slot(i);
        s.setCodePtr(trampolineAddr);
        s.lastUsed = 0;
        s.bodyHandle = body;
        s.setStaticInfo(argCount, scan.bodyBytes, scan.needsLRSave);
    }

    return 0;
}

uint32_t Runtime::pushStackFloor(uint32_t margin)
{
    register uint32_t sp asm("sp");

    if(sp <= memory.stackFloorThreshold(margin))
    {
        runtimeBail(this, RESOURCE_EXHAUSTED_TRANSLATOR_STACK);
    }

    return memory.publishStackFloor(sp - margin);
}

void Runtime::popStackFloor(uint32_t outerFloor)
{
    memory.retractStackFloor(outerFloor);
}

void Runtime::evict(uint32_t idx, const uint16_t *end)
{
    const uint32_t victimAddr = dispatch.addressOf(idx);
    const uint32_t gapEnd = victimAddr + occupiedSizeOf(idx);

    memory.closeGap(victimAddr, gapEnd, (uint32_t)(uintptr_t)end);
    dispatch.retire(idx, gapEnd, gapEnd - victimAddr);
}

uint16_t* Runtime::ensureSpace(const uint16_t* end, uint32_t lruTick)
{
    const uint32_t ceiling = memory.ceiling();

    assert((uint32_t)end <= ceiling);

    if((uint32_t)end == ceiling)
    {
        int victim = dispatch.findEvictionVictim(lruTick);
        if(victim < 0)
        {
            runtimeBail(this, RESOURCE_EXHAUSTED_ARENA);
            return nullptr;
        }

        evict((uint32_t)victim, end);
    }

    return (uint16_t *)(uintptr_t)memory.getCursor();
}
