#ifndef JIT_ARMV6M_RUNTIME_ENTRY_ARGS_H_
#define JIT_ARMV6M_RUNTIME_ENTRY_ARGS_H_

#include <stdint.h>
#include <stddef.h>

#include "runtime_host.h" /* ENTRY_ARGS_*_OFFSET */
#include "window.h"       /* jitc::physReg, and WINDOW_BASE/WINDOW_SIZE via registers.h */

struct EntryArgs
{
    // !!! ORDER IS FIXED: asm depends on it. !!!
    const uint32_t *spilledStart;
    const uint32_t *spilledEnd;
    uint32_t acc;
    uint32_t window[jitc::WINDOW_SIZE];
};

#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(offsetof(EntryArgs, spilledStart) == 0);
static_assert(sizeof(EntryArgs::spilledStart) == 4);
static_assert(offsetof(EntryArgs, spilledEnd) == 4);
static_assert(sizeof(EntryArgs::spilledEnd) == 4);
static_assert(offsetof(EntryArgs, acc) == 8);
static_assert(sizeof(EntryArgs::acc) == 4);
static_assert(offsetof(EntryArgs, window) == 12);
static_assert(sizeof(EntryArgs::window) == 16);
#endif


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

    ea->spilledStart = args;
    ea->spilledEnd = args + windowFloor;
}

#endif /* JIT_ARMV6M_RUNTIME_ENTRY_ARGS_H_ */
