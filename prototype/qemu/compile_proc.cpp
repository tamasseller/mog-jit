/* The mock translator (docs/jit-armv6m-dispatch-handoff.html §09) —
 * "compiling" procedure `idx` is a memcpy from its already-ABI-compliant
 * flash blob (test/qemu-run-abi.ts), not real code generation. The space
 * accounting, eviction (global LRU minimum, nothing pinned — §04/§08),
 * and compaction (memmove + fix up just the moved slots' own code_ptr,
 * no other patching — §11's position-independence) are the real thing,
 * though — all of it now `Runtime`'s own encapsulated behavior
 * (runtime_internal.h), not hand-rolled index arithmetic scattered
 * through this function the way it used to be.
 *
 * Its own file, split out of runtime_host.cpp, purely so qemu/Makefile
 * can enforce this function's own stack frame at build time
 * (-Wstack-usage=48 -Werror=stack-usage=48) instead of leaning on a
 * hand-measured, hand-maintained comment: `runtime_host.cpp`'s
 * `requiredStackBytes` bakes in a `-fstack-usage`-measured 48 bytes for
 * this function (`MOCK_TRANSLATOR_ENTRY_WORST_CASE_BYTES`'s own
 * comment), and now the build itself fails the instant that number
 * drifts, rather than the comment silently going stale. Sharing a file
 * with runtime_host.cpp's own *_on_stack/_split variants would have
 * defeated the point — their VLAs are genuinely, deliberately unbounded
 * at compile time, and `-Wstack-usage=`/`-Werror=stack-usage=` warns (and
 * with `-Werror=`, fails) on *any* function it can't statically bound,
 * regardless of the threshold. `Runtime`'s own methods (all defined
 * inline, in the shared header) fold straight back into this function at
 * -Os — confirmed via the same `-fstack-usage` measurement, not assumed —
 * so splitting the logic into methods doesn't multiply the number of
 * frames the build has to account for.
 *
 * Built with -ffixed-r8/r9/r10/r11 (qemu/Makefile) — those four registers
 * carry live JIT state (dispatch table base, runtime pointer, helper
 * vector base, LRU tick) across this file's own call boundary from
 * qemu/runtime.S, so removing them from the compiler's register
 * allocator entirely is a stronger, toolchain-enforced guarantee than
 * leaning on ordinary callee-saved convention.
 */

#include <stdint.h>
#include <string.h>
#include "runtime_internal.h"

/** Jumps straight to the same landing address a normal return would reach
 *  off the dispatch table's sentinel slot — but directly, bypassing
 *  returnHelper entirely, with LANDING_TRAP instead of whatever tag
 *  `enter_dispatch` (qemu/runtime.S) bootstrapped with (its own landing
 *  point is what actually disambiguates the two via r2). Never crosses
 *  the asm boundary by name (reached only from `compileProc`, below, in
 *  the same translation unit), so no `extern "C"` needed — nothing ever
 *  looks it up by its (mangled, and it's `static` besides) symbol. */
static void __attribute__((noreturn)) bailOut(Runtime *rt, uint32_t trapCode)
{
    register uint32_t r0_ asm("r0") = trapCode;
    register uint32_t r2_ asm("r2") = LANDING_TRAP;
    register uint32_t r3_ asm("r3") = rt->sentinelLandingAddress();
    register uint32_t r1_ asm("r1") = rt->savedSp;
    /* Restore sp before jumping away — see savedSp's own header. */
    asm volatile("mov sp, r1\n\tbx r3" : : "r"(r0_), "r"(r1_), "r"(r2_), "r"(r3_));
    __builtin_unreachable();
}

/** The mock translator itself. Reached only from qemu/runtime.S via a
 *  plain `bl compileProc` — `extern "C"` so that symbol stays exactly
 *  `compileProc`, not whatever the Itanium ABI would mangle this
 *  signature into. */
extern "C" void compileProc(uint32_t idx, Runtime *rt)
{
    const FlashProc *proc = &rt->flashProcs[idx];
    uint32_t need = proc->size;

    /* r11 (LRU_TICK_REG, src/registers.ts) — read, never written here:
     * -ffixed-r11 (qemu/Makefile) means nothing in this file ever touches
     * it, so it still holds whatever the prologue stub last bumped it to.
     * Handed to Runtime's own findEvictionVictim as "now" — that class
     * has no opinion on where the value comes from, only on what to do
     * with it once given one. */
    register uint32_t now asm("r11");

    while(!rt->hasRoomFor(need))
    {
        int victim = rt->findEvictionVictim(now);
        if(victim < 0) bailOut(rt, RESOURCE_ERROR_CODE); /* table empty, still doesn't fit */
        rt->evict((uint32_t)victim);
    }

    uint32_t dest = rt->allocate(need);
    memcpy((void *)(uintptr_t)dest, proc->bytes, need);
    rt->markCompiled(idx, dest);
}
