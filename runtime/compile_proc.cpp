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
 * (translateProc's own Assembler::finalize() call), and exits directly
 * (Assembler::fail() -> dispatch_abi.cpp's runtimeBail) with
 * RESOURCE_EXHAUSTED_ARENA if even evicting everything resident couldn't
 * free enough room — this file
 * no longer needs its own scratch buffer, ArenaRoom implementor, or
 * post-hoc overflow check.
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
