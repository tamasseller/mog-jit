#ifndef JIT_ARMV6M_RUNTIME_EXECUTOR_H_
#define JIT_ARMV6M_RUNTIME_EXECUTOR_H_

#include <stdint.h>

#include "code_arena.h"

/* How an excursion ended: `trapped` is one of dispatch_abi.h's LANDING_* tags,
 * and `value` is what that tag says it is. */
struct ProgramResult
{
    uint32_t value;
    uint32_t trapped;
};

/* Owns the arena, settled once for as many programs as the caller cares to
 * run. Everything program-specific arrives at run(), which places that
 * program's Runtime in its own frame and takes both that and the arena back
 * down again on the way out. */
class Executor
{
    CodeArena arena;

    inline explicit Executor(const CodeArena &arena): arena(arena) { }

public:
    /* The arena is a region of its own and the stack never reaches it. */
    static inline Executor split(uint32_t codeArenaBase, uint32_t codeArenaSize, uint32_t stackLimit, uint32_t interruptReserve)
    {
        return Executor(CodeArena::region(codeArenaBase, codeArenaSize, stackLimit, interruptReserve));
    }

    /* The arena sits at the bottom of the permitted stack region and the two
     * grow towards each other. It is not sized: it is everything between
     * stackLimit and the code limit run() computes, so a program gets whatever
     * its own static reservation leaves over. */
    static inline Executor onStack(uint32_t stackLimit, uint32_t interruptReserve)
    {
        return Executor(CodeArena::sharedWithStack(stackLimit, interruptReserve));
    }

    ProgramResult run(const uint8_t *programBytes, uint32_t programSize, uint32_t *args, uint32_t argCount);
};

#endif /* JIT_ARMV6M_RUNTIME_EXECUTOR_H_ */
