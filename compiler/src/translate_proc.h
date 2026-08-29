// The top-level per-procedure driver — decodes one procedure's wire bytes
// and emits Thumb code via abi_strategy.h's real-ABI sequences.
// Everything reaches the outside world through Runtime: slot(procIdx)
// supplies argCount/bodyPtr/bodyBytes/needsLRSave, and compilation always
// goes through an Assembler attached to r. A host test or QEMU
// pre-measurement gets the same path by handing in a throwaway Runtime
// with only the facts translateProc reads filled in.
#ifndef JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
#define JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_

#include <cstdint>
#include "proc.h"
#include "assembler.h"

namespace jitc
{

// The per-procedure forward pass. procIdx is both this procedure's own
// dispatch-table index (abiEmitCall's own packRecord argument) and the
// slot r.slot(procIdx) is read from for the Proc itself (argCount/body/
// bodyBytes) and needsLRSave. A CALL's own callee argCount is read the
// same way, off r.slot(instr.calleeIndex).argCount() — no separate array.
//
// r is also the only seam into anything Runtime-owned (arena growth via
// the Assembler this function builds over it, the live stack-nesting
// floor via r.liveStackFloor(), final registration). This function
// finalizes that Assembler itself as its last step (flushing any
// still-open pool chunk, committing the arena allocation, and
// registering the result with r) and returns the final halfword count.
// A translator-detected failure calls Assembler::fail() with the
// RESOURCE_* code naming which one (runtime_host.h): it never returns,
// unwinding straight to the landing.
//
// lruTick is the current r11 value (the live LRU tick), read once by the
// caller so this function never needs `register ... asm("r11")` itself.
uint32_t translateProc(
    uint32_t procIdx,
    Runtime& r,
    uint32_t lruTick
);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
