/* The real compileProc — genuine bytecode-to-Thumb translation
 * (compiler/src/translate_proc.h), reading every procedure's own body
 * pointer/argCount/needsLRSave straight out of the whole-program directory
 * Runtime::init() already built (runtime_internal.h's ProcSlot) — never a
 * fixture-supplied stand-in. Reuses runtime_internal.h's Runtime/ProcSlot
 * and runtime_host.h's ProgramResult unmodified.
 *
 * Assembler (compiler/src/assembler.h) is now the only seam between this
 * and Runtime: an attached Assembler owns arena growth/eviction/
 * compaction and final dispatch-table registration internally
 * (translateProc's own Assembler::finalize() call), and exits directly to
 * RESOURCE_ERROR (Assembler::fail() -> runtimeBail, below) if even
 * evicting everything resident couldn't free enough room — this file no
 * longer needs its own scratch buffer, ArenaRoom implementor, or post-hoc
 * overflow check.
 */
#include <stdint.h>
#include "runtime_internal.h"
#include "translate_proc.h"
#include "assembler.h"

extern "C" void compileProc(uint32_t idx, Runtime *runtime)
{
    const ProcSlot &procSlot = runtime->slot(idx);
    jitc::Proc proc{procSlot.argCount(), (const uint8_t *)(uintptr_t)procSlot.bodyPtr, procSlot.bodyBytes()};
    bool savesLR = procSlot.needsLRSave();

    /* abiEmitCall needs O(1) indexing by calleeIndex; ProcSlot's own
     * 16-byte stride doesn't give it that for free, so this dense copy —
     * a VLA, explicitly budgeted into requiredStackBytes
     * (runtime_host.cpp's CALLEE_ARG_COUNTS_BYTES_PER_PROC) rather than a
     * fixed cap, since a real program's own procCount isn't bounded to
     * any fixture-sized constant. */
    uint32_t calleeArgCounts[runtime->procCount];
    for(uint32_t i = 0; i < runtime->procCount; i++)
    {
        calleeArgCounts[i] = runtime->slot(i).argCount();
    }

    register uint32_t lruTick asm("r11");
    jitc::Assembler assembler(runtime, idx, lruTick);

    /* translateProc finalizes assembler itself as its last step —
     * flushing any still-open pool chunk and, since assembler is
     * attached, committing the arena allocation and calling
     * runtime->markCompiled(idx, ...) — so there is nothing left for this
     * function to do afterward. Failure (arena exhaustion beyond what
     * Assembler::reserve() could free by evicting, or the live
     * stack-nesting guard tripping) never returns here at all: it
     * unwinds straight through runtimeBail below. */
    jitc::translateProc(proc, idx, calleeArgCounts, runtime->procCount, assembler, &savesLR);
}

/* Assembler::fail()'s own direct exit on an attached Assembler — restores
 * the caller's own saved sp and transfers to the sentinel landing, tagged
 * LANDING_TRAP, unwinding the entire excursion including the trampoline's
 * own pushed frame. Never returns. This is the old bailOut, moved
 * verbatim and renamed to the extern "C" name runtime_internal.h
 * declares — reached from inside Assembler now (compiler/src/
 * assembler.cpp's fail()) instead of from this file's own post-hoc
 * overflow check, which no longer exists. */
extern "C" void runtimeBail(Runtime *runtime, uint32_t trapCode)
{
    register uint32_t trapCodeReg asm("r0") = trapCode;
    register uint32_t tagReg asm("r2") = LANDING_TRAP;
    register uint32_t landingReg asm("r3") = runtime->sentinelLandingAddress();
    register uint32_t savedSpReg asm("r1") = runtime->savedSp;
    asm volatile("mov sp, r1\n\tbx r3" : : "r"(trapCodeReg), "r"(savedSpReg), "r"(tagReg), "r"(landingReg));
    __builtin_unreachable();
}
