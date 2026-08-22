/* The real compileProc — replaces the mock's memcpy-of-a-precompiled-blob
 * with genuine bytecode-to-Thumb translation (jit-armv6m/compiler/src/
 * translate_proc.h). Reuses runtime_internal.h's Runtime/DispatchEntry and
 * runtime_host.h's FlashProc/ProgramResult totally unmodified — this file
 * is purely additive, exactly like jit-armv6m/prototype/qemu/
 * compile_proc.cpp's own mock path, which stays untouched and keeps
 * validating dispatch/eviction/compaction independent of compiler
 * correctness.
 *
 * The new bytecode (jitc::Proc/jitc::Instr) is deliberately NOT threaded
 * through Runtime::flashProcs/FlashProc — both stay exactly as they are.
 * Instead, g_realProcs/g_realProcCount (fixtures.h) are new, separate,
 * additive globals this file reaches by plain name, mirroring how
 * runtime_host.cpp already reaches g_helperVec/g_trampolineAddr the same
 * way rather than through Runtime. enter_program's own `procs` parameter
 * is satisfied by a small all-zero dummy FlashProc array (main.cpp) —
 * nothing on this path ever dereferences `.bytes`/`.size`.
 */
#include <stdint.h>
#include <string.h>
#include "runtime_internal.h"
#include "fixtures.h"
#include "translate_proc.h"

/* Generous headroom for one procedure's own translated output — this
 * slice's whole corpus tops out well under this (measured while building
 * the fixtures; see jit-armv6m/compiler/test/qemu/main.cpp's own
 * commentary). Translating into a scratch buffer first, then copying the
 * discovered size into the arena, is valid because the emitted ABI code
 * is position-independent (programAbi.ts's own §11 property) — nothing
 * this translator emits bakes in its own eventual address. File-scope,
 * not stack-local, to keep compileProc's own frame small and bounded
 * regardless of this constant. */
#define SCRATCH_CAPACITY_HALFWORDS 128
static uint16_t g_scratch[SCRATCH_CAPACITY_HALFWORDS];

static void __attribute__((noreturn)) bailOut(Runtime *rt, uint32_t trapCode)
{
    register uint32_t r0_ asm("r0") = trapCode;
    register uint32_t r2_ asm("r2") = LANDING_TRAP;
    register uint32_t r3_ asm("r3") = rt->sentinelLandingAddress();
    register uint32_t r1_ asm("r1") = rt->savedSp;
    asm volatile("mov sp, r1\n\tbx r3" : : "r"(r0_), "r"(r1_), "r"(r2_), "r"(r3_));
    __builtin_unreachable();
}

extern "C" void compileProc(uint32_t idx, Runtime *rt)
{
    const jitc::Proc &proc = g_realProcs[idx];

    uint32_t calleeArgCounts[8]; /* this slice's own small fixture corpus — see fixtures.cpp */
    for(uint32_t i = 0; i < rt->procCount; i++)
        calleeArgCounts[i] = g_realProcs[i].argCount;

    jitc::TranslateResult tr = jitc::translateProc(
        proc, idx, calleeArgCounts, rt->procCount, g_scratch, SCRATCH_CAPACITY_HALFWORDS);
    if(tr.overflowed) bailOut(rt, RESOURCE_ERROR_CODE);

    uint32_t need = tr.halfwordCount * 2;
    register uint32_t now asm("r11");

    while(!rt->hasRoomFor(need))
    {
        int victim = rt->findEvictionVictim(now);
        if(victim < 0) bailOut(rt, RESOURCE_ERROR_CODE);
        rt->evict((uint32_t)victim);
    }

    uint32_t dest = rt->allocate(need);
    memcpy((void *)(uintptr_t)dest, g_scratch, need);
    rt->markCompiled(idx, dest);
}
