/* jit-armv6m/runtime — layer 3a, the landing: compileProc, reached from
 * translatorTrampoline (runtime.S) whenever dispatch lands on an
 * uncompiled slot. Genuine bytecode-to-Thumb translation
 * (compiler/src/translate_proc.h), reading every procedure's own body
 * pointer/argCount/needsLRSave straight out of the whole-program directory
 * Runtime::init() already built (runtime_internal.h's ProcSlot) — never a
 * fixture-supplied stand-in. Reuses runtime_internal.h's Runtime/ProcSlot
 * and runtime_host.h's ProgramResult unmodified.
 *
 * Assembler (compiler/src/assembler.h, layer 3b) is now the only seam
 * between this and Runtime: an attached Assembler owns arena growth/
 * eviction/compaction and final dispatch-table registration internally
 * (translateProc's own Assembler::finalize() call), and exits directly to
 * RESOURCE_ERROR (Assembler::fail() -> dispatch_abi.cpp's runtimeBail) if
 * even evicting everything resident couldn't free enough room — this file
 * no longer needs its own scratch buffer, ArenaRoom implementor, or
 * post-hoc overflow check.
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
     * (dispatch_abi.h's CALLEE_ARG_COUNTS_BYTES_PER_PROC) rather than a
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
     * unwinds straight through dispatch_abi.cpp's runtimeBail. */
    jitc::translateProc(proc, idx, calleeArgCounts, runtime->procCount, assembler, &savesLR);
}
