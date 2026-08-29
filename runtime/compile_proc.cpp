/* jit-armv6m/runtime — layer 3a, the landing: compileProc, reached from
 * translatorTrampoline (runtime.S) whenever dispatch lands on an
 * uncompiled slot. Genuine bytecode-to-Thumb translation
 * (compiler/src/translate_proc.h), reading every procedure's own body
 * pointer/argCount/needsLRSave straight out of the whole-program directory
 * Runtime::init() already built (runtime_internal.h's ProcSlot) — never a
 * fixture-supplied stand-in. Reuses runtime_internal.h's Runtime/ProcSlot
 * and runtime_host.h's ProgramResult unmodified.
 *
 * Assembler (compiler/src/assembler.h) is the only seam between this and
 * Runtime: it owns arena growth, eviction, compaction and dispatch-table
 * registration internally, and exits directly through runtimeBail with
 * RESOURCE_EXHAUSTED_ARENA if evicting everything resident still leaves
 * too little room.
 */
#include <stdint.h>
#include "runtime_internal.h"
#include "translate_proc.h"
#include "assembler.h"

extern "C" void compileProc(uint32_t idx, Runtime *runtime)
{
    register uint32_t lruTick asm("r11");

    jitc::translateProc(idx, *runtime, lruTick);
}
