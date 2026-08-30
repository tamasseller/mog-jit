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

    /* Fixed for this arena's lifetime. */
    uint32_t end;
    uint32_t stackLimit;
    uint32_t overlapsStack;   /* 0/1, not bool, so this struct's layout is trivial */
    uint32_t interruptReserve;

    /* One excursion's worth — Excursion is what puts these back. */
    uint32_t cursor;
    uint32_t liveStackFloor;

    /* Whichever of the two the stack meets first: the statically validated
     * limit, or where the arena has actually reached. */
    inline uint32_t hardFloor() const
    {
        return (overlapsStack && stackLimit < cursor) ? cursor : stackLimit;
    }

    inline CodeArena(uint32_t base, uint32_t end, uint32_t stackLimit, uint32_t overlapsStack, uint32_t interruptReserve):
        end(end & ~3u), stackLimit(stackLimit), overlapsStack(overlapsStack),
        interruptReserve(interruptReserve), cursor((base + 3u) & ~3u), liveStackFloor(UINT32_MAX)
    {
        assert(cursor <= this->end); // GCOV_EXCL_LINE
    }

public:
    /* Memory of its own, that the stack never reaches. */
    static inline CodeArena region(uint32_t base, uint32_t size, uint32_t stackLimit, uint32_t interruptReserve = 0)
    {
        return CodeArena(base, base + size, stackLimit, /*overlapsStack=*/0, interruptReserve);
    }

    /* The bottom of the stack region itself, with no end of its own: what
     * stops it is the stack coming down to meet it. */
    static inline CodeArena sharedWithStack(uint32_t stackLimit, uint32_t interruptReserve = 0)
    {
        return CodeArena(stackLimit, UINT32_MAX, stackLimit, /*overlapsStack=*/1, interruptReserve);
    }

    /* One program's use of the arena, handing back on the way out exactly what
     * it found. codeLimit is the highest address compiled code may reach on
     * this run — published as the first stack floor, since that line is
     * exactly a promise about how deep the stack will come. */
    class Excursion
    {
        Excursion(const Excursion&) = delete;
        Excursion(Excursion&&) = delete;

        void operator =(const Excursion&) = delete;
        void operator =(Excursion&&) = delete;

        CodeArena& a;
        uint32_t outerCursor;

    public:
        inline Excursion(CodeArena& a, uint32_t codeLimit):
            a(a), outerCursor(a.cursor)
        {
            if(a.overlapsStack) 
            {
                a.liveStackFloor = codeLimit + a.interruptReserve;
            }
        }

        inline ~Excursion()
        {
            a.cursor = outerCursor;
            a.liveStackFloor = UINT32_MAX;
        }
    };

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

    /* Room kept below the stack for an exception frame, so it belongs to the
     * up-front reservation as much as to the arena's own ceiling. */
    inline uint32_t getInterruptReserve() const
    {
        return interruptReserve;
    }

    /* Never past the arena's own end, and never into the ground a live floor
     * has claimed for the stack. */
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

    /* Floors only ever descend while a guard is live: an inner level reaching
     * less deep than the excursion's own promise does not give ground back. */
    inline uint32_t publishStackFloor(uint32_t floor)
    {
        const uint32_t outerFloor = liveStackFloor;
        liveStackFloor = floor < outerFloor ? floor : outerFloor;
        return outerFloor;
    }

    inline void retractStackFloor(uint32_t outerFloor)
    {
        liveStackFloor = outerFloor;
    }
};

#endif /* JIT_ARMV6M_RUNTIME_CODE_ARENA_H_ */
