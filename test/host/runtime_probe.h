#ifndef JIT_ARMV6M_TEST_RUNTIME_PROBE_H_
#define JIT_ARMV6M_TEST_RUNTIME_PROBE_H_

#include "runtime.h"

/* Runtime's befriended seam: arena internals the JIT reaches only through
 * ensureSpace, but that the arena tests assert on directly. */
struct RuntimeProbe
{
    static uint32_t arenaCeiling(const Runtime &r) { return r.arenaCeiling(); }
    static uint32_t occupiedSizeOf(const Runtime &r, uint32_t idx) { return r.occupiedSizeOf(idx); }
    static int findEvictionVictim(const Runtime &r, uint32_t now) { return r.findEvictionVictim(now); }

    /* The constructor word-aligns the arena's end; these tests want halfword
     * granularity, which no real arena ever needs. */
    static void setArenaEnd(Runtime &r, uint32_t end) { r.arenaEnd = end; }
};

#endif // JIT_ARMV6M_TEST_RUNTIME_PROBE_H_
