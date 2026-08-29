#ifndef JIT_ARMV6M_RUNTIME_ENTRY_ARGS_H_
#define JIT_ARMV6M_RUNTIME_ENTRY_ARGS_H_

#include <stdint.h>
#include <stddef.h>

#include "runtime_host.h" /* ENTRY_ARGS_*_OFFSET */
#include "window.h"       /* jitc::physReg, and WINDOW_BASE/WINDOW_SIZE via registers.h */

struct EntryArgs
{
    const uint32_t *spilled;
    uint32_t spilledCount;
    uint32_t window[jitc::WINDOW_SIZE];
    uint32_t acc;
};

#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(offsetof(EntryArgs, spilled) == ENTRY_ARGS_SPILLED_OFFSET,
    "runtime.S's entry-argument block reads ea->spilled at ENTRY_ARGS_SPILLED_OFFSET");
static_assert(offsetof(EntryArgs, spilledCount) == ENTRY_ARGS_SPILLED_COUNT_OFFSET,
    "runtime.S's entry-argument block reads ea->spilledCount at ENTRY_ARGS_SPILLED_COUNT_OFFSET");
static_assert(offsetof(EntryArgs, window) == ENTRY_ARGS_WINDOW_OFFSET,
    "runtime.S's entry-argument block reads ea->window at ENTRY_ARGS_WINDOW_OFFSET");
static_assert(offsetof(EntryArgs, acc) == ENTRY_ARGS_ACC_OFFSET,
    "runtime.S's entry-argument block reads ea->acc at ENTRY_ARGS_ACC_OFFSET");
#endif

static_assert(jitc::WINDOW_SIZE == 4 && jitc::WINDOW_BASE == 4,
    "runtime.S's entry-argument block unrolls the window fill as ldr r4-r7");

inline void buildEntryArgs(EntryArgs *ea, const uint32_t *args, uint32_t declared)
{
    *ea = EntryArgs{}; /* zeroes window[], including physReg(declared-1)'s own slot */

    if(declared == 0)
    {
        return; /* nothing to place, and `args` may legitimately be null */
    }

    ea->acc = args[declared - 1];

    const uint32_t windowFloor = declared > jitc::WINDOW_SIZE ? declared - jitc::WINDOW_SIZE : 0;

    /* k + 1 < declared, not k < declared: slot declared-1 arrives via acc. */
    for(uint32_t k = windowFloor; k + 1 < declared; k++)
    {
        ea->window[jitc::physReg(k) - jitc::WINDOW_BASE] = args[k];
    }

    ea->spilled = args;
    ea->spilledCount = windowFloor;
}

#endif /* JIT_ARMV6M_RUNTIME_ENTRY_ARGS_H_ */
