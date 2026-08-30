#ifndef JIT_ARMV6M_RUNTIME_DISPATCH_TABLE_H_
#define JIT_ARMV6M_RUNTIME_DISPATCH_TABLE_H_

#include <stdint.h>
#include <stddef.h>

#include "runtime_host.h"

extern const uint32_t trampolineAddr;

struct ProcSlot
{
    uint32_t codePtr;    
    uint32_t lastUsed;   
    uint32_t bodyPtr;
    uint32_t staticInfo; 

    static constexpr uint32_t MAX_ARG_COUNT = (1u << 11) - 1;
    static constexpr uint32_t MAX_BODY_BYTES = (1u << 20) - 1;

    inline void setStaticInfo(uint32_t argCount, uint32_t bodyBytes, bool needsLRSave)
    {
        staticInfo = (needsLRSave ? 0x80000000u : 0u) | (argCount << 20) | bodyBytes;
    }

    inline uint32_t argCount() const
    {
        return (staticInfo >> 20) & MAX_ARG_COUNT;
    }

    inline uint32_t bodyBytes() const
    {
        return staticInfo & MAX_BODY_BYTES;
    }

    inline bool needsLRSave() const
    {
        return (staticInfo & 0x80000000u) != 0;
    }

    inline void setCodePtr(uint32_t addr)
    {
        codePtr = addr | 1u;
    }

    inline void slideCodePtr(uint32_t delta)
    {
        codePtr = (codePtr - delta) | 1u;
    }
};

static_assert(sizeof(ProcSlot) == 16, "power-of-two: idx*16 must stay a shift, not a multiply — runtime.S's own hardcoded stride");
static_assert(sizeof(ProcSlot) == DISPATCH_SENTINEL_OFFSET,
    "runtime.S's own DISPATCH_SENTINEL_OFFSET must match sizeof(ProcSlot)");

/* One entry per procedure, plus the sentinel below index zero that runtime.S
 * parks the landing address and the entry sp in. Says where each procedure's
 * code is and when it was last reached; says nothing about where that code
 * fits — see code_arena.h for the other half. */
class DispatchTable
{
    friend struct RuntimeProbe;

    uint32_t procCount;

    ProcSlot slots[];

public:
    inline explicit DispatchTable(uint32_t procCount): procCount(procCount)
    {
        slots[0].lastUsed = 0;
        slots[0].bodyPtr = 0;
        slots[0].staticInfo = 0;
    }

    inline uint32_t getProcCount() const
    {
        return procCount;
    }

    inline ProcSlot &slot(uint32_t idx)
    {
        return slots[idx + 1];
    }

    inline const ProcSlot &slot(uint32_t idx) const
    {
        return slots[idx + 1];
    }

    inline uint32_t sentinelLandingAddress() const
    {
        return slots[0].codePtr;
    }

    inline uint32_t savedSp() const
    {
        return slots[0].bodyPtr;
    }

    inline bool isResident(uint32_t idx) const
    {
        return slot(idx).codePtr != trampolineAddr;
    }

    inline uint32_t addressOf(uint32_t idx) const
    {
        return slot(idx).codePtr & ~1u;
    }

    inline void markCompiled(uint32_t idx, uint32_t dest, uint32_t lruTick)
    {
        ProcSlot &entry = slot(idx);
        entry.setCodePtr(dest);
        entry.lastUsed = lruTick;
    }

    /* Not inlined: ensureSpace runs per emitted halfword and this only on a
     * full arena — folded in, its frame would ride the whole translation. */
    __attribute__((noinline)) int findEvictionVictim(uint32_t now) const
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

    /* The lowest resident body starting strictly above addr, or ceiling if
     * there is none — the end of what idx occupies. */
    inline uint32_t nextResidentAbove(uint32_t addr, uint32_t ceiling) const
    {
        uint32_t gapEnd = ceiling;
        for(uint32_t i = 0; i < procCount; i++)
        {
            if(!isResident(i))
            {
                continue;
            }
            uint32_t a = addressOf(i);
            if(a > addr && a < gapEnd)
            {
                gapEnd = a;
            }
        }
        return gapEnd;
    }

    /* Sends idx back to the trampoline and follows everything the arena's own
     * compaction just moved down by delta. */
    inline void retire(uint32_t idx, uint32_t gapEnd, uint32_t delta)
    {
        ProcSlot &victimEntry = slot(idx);
        victimEntry.setCodePtr(trampolineAddr);
        victimEntry.lastUsed = 0;

        for(uint32_t i = 0; i < procCount; i++)
        {
            ProcSlot &entry = slot(i);
            if(entry.codePtr != trampolineAddr && (entry.codePtr & ~1u) >= gapEnd)
            {
                entry.slideCodePtr(delta);
            }
        }
    }
};

#endif /* JIT_ARMV6M_RUNTIME_DISPATCH_TABLE_H_ */
