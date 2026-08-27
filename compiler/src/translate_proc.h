// The top-level per-procedure driver — decodes one procedure's wire bytes
// and emits Thumb code for it via abi_strategy.h's real-ABI sequences.
// Everything below reaches the outside world through Assembler itself,
// never through Runtime directly — this is the "core compiler logic" layer
// (docs/design.md's 3c), kept free of the environment's own oddities so
// it stays testable against a plain detached Assembler with no Runtime
// in play at all, exactly as every host unit test and QEMU pre-
// measurement call already does.
#ifndef JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
#define JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_

#include <cstdint>
#include "proc.h"
#include "assembler.h"

namespace jitc
{

// The per-procedure forward pass. procIdx is this procedure's own
// dispatch-table index (abiEmitCall's own packRecord argument). A CALL's
// own callee argCount is read directly off
// r.slot(instr.calleeIndex).argCount() — no separate array; r is the one
// seam into the whole-program directory (runtime_internal.h's ProcSlot).
//
// a is the only seam into anything Runtime-owned (arena growth, the live
// stack-nesting floor, final registration) — an attached Assembler
// (compiler/src/assembler.h) carries all of that; a detached one (every
// host test, the QEMU pre-measurement calls) has none of it, and gets
// pure translation-correctness checking instead. Either way this
// function finalizes a itself as its last step (flushing any still-open
// pool chunk, and — for an attached Assembler — committing the arena
// allocation and registering the result with Runtime) and returns the
// final halfword count; a caller never needs a separate a.finalize()
// call of its own. A translator-detected failure (arena exhaustion
// beyond what Assembler::reserve() could free, or the live stack-nesting
// guard tripping) calls a.fail(): on a detached Assembler this returns
// normally with overflowed() now true; on an attached one it never
// returns at all, unwinding straight to RESOURCE_ERROR.
//
// savesLR is the whole-program directory's own precomputed answer
// (runtime/runtime_internal.h's ProcSlot, via proc_scan.h's scanProcBody)
// to "does this body ever reach CALL/BR_TABLE(N>2)/CLZ/REVBITS" — every
// caller already has this answer up front (a Runtime's own ProcSlot, or a
// test/pre-measurement call's own hand-derived literal), so translateProc
// never needs to rescan the body itself.
uint32_t translateProc(
    const Proc &proc,
    uint32_t procIdx,
    Assembler &a,
    const Runtime& r);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
