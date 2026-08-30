#ifndef JIT_ARMV6M_RUNTIME_CODE_ARENA_H_
#define JIT_ARMV6M_RUNTIME_CODE_ARENA_H_

#include <stdint.h>
#include <cassert>

/* Where compiled code is put, and how far down the stack may come to meet it.
 * On a shared-region configuration those are two ends of the same ground, so
 * both live here; stack_budget.h states the one-way traffic rule they enforce
 * on each other. Knows nothing about procedures — see dispatch_table.h. */
class CodeArena
{
    friend struct RuntimeProbe;

    uint32_t end;
    uint32_t cursor;
    uint32_t stackLimit;
    uint32_t overlapsStack;   /* 0/1, not bool, so this struct's layout is trivial */
    uint32_t interruptReserve;
    uint32_t liveStackFloor;

    /* Whichever of the two the stack meets first: the statically validated
     * limit, or where the arena has actually reached. */
    inline uint32_t hardFloor() const
    {
        return (overlapsStack && stackLimit < cursor) ? cursor : stackLimit;
    }

public:
    inline CodeArena(uint32_t base, uint32_t size, uint32_t stackLimit, uint32_t overlapsStack, uint32_t interruptReserve):
        end((base + size) & ~3u), cursor((base + 3u) & ~3u), stackLimit(stackLimit),
        overlapsStack(overlapsStack), interruptReserve(interruptReserve), liveStackFloor(UINT32_MAX)
    {
        assert(cursor <= end); // GCOV_EXCL_LINE
    }

    inline uint32_t getCursor() const
    {
        return cursor;
    }

    /* The statically validated floor, which is the only one that exists before
     * the first guard runs. */
    inline uint32_t getStackLimit() const
    {
        return stackLimit;
    }

    /* Never past what was validated up front, and never into the stack for as
     * long as a guard holds a floor below that line. */
    inline uint32_t ceiling() const
    {
        if(!overlapsStack)
        {
            return end;
        }

        const uint32_t live = interruptReserve < liveStackFloor ? liveStackFloor - interruptReserve : 0;
        return live < end ? live : end;
    }

    inline uint32_t commit(uint32_t newEnd)
    {
        assert(cursor <= newEnd && newEnd <= ceiling());

        cursor = (newEnd + 3u) & ~3u;

        return cursor;
    }

    /* Slides [gapEnd, occupiedEnd) down onto gapStart and hands the difference
     * back to the cursor. */
    inline void closeGap(uint32_t gapStart, uint32_t gapEnd, uint32_t occupiedEnd)
    {
        uint16_t *dst = (uint16_t *)(uintptr_t)gapStart;
        const uint16_t *src = (const uint16_t *)(uintptr_t)gapEnd;

        for(uint32_t i = 0; i < (occupiedEnd - gapEnd) / 2; i++)
        {
            dst[i] = src[i];
        }

        cursor -= gapEnd - gapStart;
    }

    /* The lowest sp from which a level reserving margin may still proceed. */
    inline uint32_t stackFloorThreshold(uint32_t margin) const
    {
        return hardFloor() + margin + interruptReserve;
    }

    inline uint32_t publishStackFloor(uint32_t floor)
    {
        const uint32_t outerFloor = liveStackFloor;
        liveStackFloor = floor;
        return outerFloor;
    }

    inline void retractStackFloor(uint32_t outerFloor)
    {
        liveStackFloor = outerFloor;
    }
};

#endif /* JIT_ARMV6M_RUNTIME_CODE_ARENA_H_ */
