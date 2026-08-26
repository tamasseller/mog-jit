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
// dispatch-table index (abiEmitCall's own packRecord argument).
// calleeArgCounts[i] is procedure i's own argCount — this function only
// ever reads calleeArgCounts[instr.calleeIndex].
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
// savesLROverride, if non-null, is the whole-program directory's own
// precomputed answer (runtime/runtime_internal.h's ProcSlot) to "does this
// body ever reach CALL/BR_TABLE(N>2)/CLZ/REVBITS" — needsLRSave(proc)
// answers the same question by scanning the body fresh, exactly right for
// a one-off host-test/pre-measurement call, but wasteful to redo on every
// recompile once a directory already has the answer. Null (every existing
// caller, unchanged) falls back to that scan.
uint32_t translateProc(
    const Proc &proc,
    uint32_t procIdx,
    const uint32_t *calleeArgCounts, uint32_t calleeCount,
    Assembler &a,
    const bool *savesLROverride = nullptr);

/** Whether proc's own body ever reaches an op needing lr protected before
 *  anything can clobber it: a nested CALL, blocks.h's own openBrTableJump
 *  (BR_TABLE N > 2 only), or unaryops.h's CLZ/REVBITS — all reached via
 *  BLX through the helper vector, which clobbers real hardware lr exactly
 *  like a local BL would. Exported for callers with no directory
 *  (runtime/runtime_internal.h's ProcSlot) to source the answer from
 *  instead — translateProc itself falls back to this whenever
 *  savesLROverride is null. */
bool needsLRSave(const Proc &proc);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
