#ifndef JIT_ARMV6M_RUNTIME_EXECUTOR_H_
#define JIT_ARMV6M_RUNTIME_EXECUTOR_H_

#include <stdint.h>

#include "runtime_host.h"

/* Where programs get their memory from, settled once for as many of them as
 * the caller cares to run. Everything program-specific arrives at run(),
 * which places that program's Runtime in its own frame and takes it back down
 * again on the way out. */
class Executor
{
    uint32_t codeArenaBase;    /* both zero when arenaOverlapsStack: run() derives */
    uint32_t codeArenaSize;    /* the arena from what the budget check leaves over */
    uint32_t stackLimit;
    uint32_t arenaOverlapsStack;   /* 0/1, not bool, so this struct's layout is trivial */
    uint32_t interruptReserve;

    inline Executor(uint32_t codeArenaBase, uint32_t codeArenaSize, uint32_t stackLimit,
            uint32_t arenaOverlapsStack, uint32_t interruptReserve):
        codeArenaBase(codeArenaBase), codeArenaSize(codeArenaSize), stackLimit(stackLimit),
        arenaOverlapsStack(arenaOverlapsStack), interruptReserve(interruptReserve)
    { }

public:
    /* The arena is a region of its own and the stack never reaches it. */
    static inline Executor split(uint32_t codeArenaBase, uint32_t codeArenaSize, uint32_t stackLimit, uint32_t interruptReserve)
    {
        return Executor(codeArenaBase, codeArenaSize, stackLimit, /*arenaOverlapsStack=*/0, interruptReserve);
    }

    /* The arena sits at the bottom of the permitted stack region and the two
     * grow towards each other. It is not sized: it is everything between
     * stackLimit and the code limit run() computes, so a program gets whatever
     * its own static reservation leaves over. */
    static inline Executor onStack(uint32_t stackLimit, uint32_t interruptReserve)
    {
        return Executor(0, 0, stackLimit, /*arenaOverlapsStack=*/1, interruptReserve);
    }

    ProgramResult run(const uint8_t *programBytes, uint32_t programSize, uint32_t *args, uint32_t argCount) const;
};

#endif /* JIT_ARMV6M_RUNTIME_EXECUTOR_H_ */
