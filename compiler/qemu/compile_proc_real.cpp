/* The real compileProc — genuine bytecode-to-Thumb translation
 * (compiler/src/translate_proc.h). Reuses runtime_internal.h's
 * Runtime/DispatchEntry and runtime_host.h's FlashProc/ProgramResult
 * unmodified.
 *
 * The bytecode (jitc::Proc/jitc::Instr) is NOT threaded through
 * Runtime::flashProcs/FlashProc — both stay exactly as they are. Instead,
 * realProcs/realProcCount (fixtures.h) are separate, additive globals this
 * file reaches by plain name, the same way runtime_host.cpp reaches
 * helperVec/trampolineAddr. enterProgram's own `procs` parameter is
 * satisfied by a small all-zero dummy FlashProc array (main.cpp) — nothing
 * on this path ever dereferences `.bytes`/`.size`.
 */
#include <stdint.h>
#include <string.h>
#include "runtime_internal.h"
#include "fixtures.h"
#include "translate_proc.h"

/* Generous headroom for one procedure's own translated output — this
 * slice's whole corpus tops out well under this. Translating into a
 * scratch buffer first, then copying the discovered size into the arena,
 * is valid because the emitted ABI code is position-independent — nothing
 * this translator emits bakes in its own eventual address. File-scope, not
 * stack-local, to keep compileProc's own frame small and bounded regardless
 * of this constant. */
#define SCRATCH_CAPACITY_HALFWORDS 128
static uint16_t scratch[SCRATCH_CAPACITY_HALFWORDS];

static void __attribute__((noreturn)) bailOut(Runtime *runtime, uint32_t trapCode)
{
    register uint32_t trapCodeReg asm("r0") = trapCode;
    register uint32_t tagReg asm("r2") = LANDING_TRAP;
    register uint32_t landingReg asm("r3") = runtime->sentinelLandingAddress();
    register uint32_t savedSpReg asm("r1") = runtime->savedSp;
    asm volatile("mov sp, r1\n\tbx r3" : : "r"(trapCodeReg), "r"(savedSpReg), "r"(tagReg), "r"(landingReg));
    __builtin_unreachable();
}

extern "C" void compileProc(uint32_t idx, Runtime *runtime)
{
    const jitc::Proc &proc = realProcs[idx];

    uint32_t calleeArgCounts[8]; /* this slice's own small fixture corpus — see fixtures.cpp */
    for(uint32_t i = 0; i < runtime->procCount; i++)
    {
        calleeArgCounts[i] = realProcs[i].argCount;
    }

    /* Read fresh every call — arenaCursor (folded into liveStackFloor() for
     * enterProgramOnStack) moves between different procedures'
     * compilations, so this can't be cached anywhere once and reused. */
    jitc::TranslateResult result = jitc::translateProc(
        proc, idx, calleeArgCounts, runtime->procCount, scratch, SCRATCH_CAPACITY_HALFWORDS,
        runtime->liveStackFloor());
    if(result.overflowed)
    {
        bailOut(runtime, RESOURCE_ERROR_CODE);
    }

    uint32_t need = result.halfwordCount * 2;
    register uint32_t now asm("r11");

    while(!runtime->hasRoomFor(need))
    {
        int victim = runtime->findEvictionVictim(now);
        if(victim < 0)
        {
            bailOut(runtime, RESOURCE_ERROR_CODE);
        }
        runtime->evict((uint32_t)victim);
    }

    uint32_t dest = runtime->allocate(need);
    memcpy((void *)(uintptr_t)dest, scratch, need);
    runtime->markCompiled(idx, dest);
}
