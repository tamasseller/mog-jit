/* jit-armv6m/runtime — layer 2, the dispatch/eviction ABI's own fixed
 * conventions: the extern "C" symbols runtime.S provides (the helper
 * vector's own routines, the translator trampoline, enterDispatch
 * itself), and the fixed per-call/per-procedure byte costs those symbols'
 * own instruction sequences imply. Shared between enter_program.cpp's
 * stack-budget arithmetic (layer 1) and dispatch_abi.cpp's own helperVec/
 * trampolineAddr/runtimeBail definitions and compile_proc.cpp (layer 3a),
 * which reads TRANSLATOR_ENTRY_WORST_CASE_BYTES' own derivation comment
 * whenever compileProc's call chain changes shape.
 */
#ifndef JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_
#define JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_

#include <stdint.h>
#include "runtime_internal.h"
#include "entry_args.h"

extern "C" {
extern void translatorTrampoline(void); /* runtime.S */
extern const uint16_t callHelper[];              /* runtime.S */
extern const uint16_t returnHelperFromLr[];      /* runtime.S */
extern const uint16_t returnHelperFromStack[];   /* runtime.S */
extern const uint16_t returnHelperTail[];        /* runtime.S */
extern const uint16_t clzHelper[];               /* runtime.S */
extern const uint16_t revbitsHelper[];           /* runtime.S */
extern const uint16_t brTableJumpHelper[];       /* runtime.S */
extern const uint16_t returnHelperFromStackReclaim[]; /* runtime.S */
extern const uint16_t trapHelper[];              /* runtime.S */
extern const uint16_t extThunkHelper[];          /* runtime.S */
extern uint64_t enterDispatch(const EntryArgs *entryArgs, Runtime *runtime); /* runtime.S */
}

/* This ABI's own fixed costs, for the stack-usage accounting
 * enter_program.cpp's requiredStackBytes needs — every one of these is a
 * property of this implementation, measured once, not something that
 * varies per program. Manual sync points: nothing statically ties these
 * numbers back to runtime.S's own instruction sequences the way
 * RUNTIME_DISPATCH_TABLE_OFFSET's static_assert does — re-measure if
 * enterDispatch's prologue, translatorTrampoline, or REALIGN_ENTER ever
 * change shape. */

/* The call/return record travels in lr, not on the operand stack — a leaf
 * callee (no CALL of its own) never spends a stack word on it. Only a
 * non-leaf callee's own prologue pushes it, once per activation, so this
 * constant is a per-call-depth upper bound (every frame assumed non-leaf),
 * not a tight count. */
#define CALL_RECORD_BYTES 4

/* What runtime.S's extThunkHelper costs an excursion that reaches it: 4 for
 * the pushed lr, 8 for REALIGN_ENTER's slack. An extension's own C helper
 * adds its own frame on top, which is why ExtHooks carries a declared
 * helperStackBytes (compiler/src/ext.h) that enterProgram* folds into the
 * up-front budget — a helper's worst case has to reach that check, or the
 * static reservation isn't a bound at all. */
#define EXT_THUNK_STACK_BYTES 12

/* enterDispatch's own two prologue pushes: {r2,r4,r5,r6,r7,lr} +
 * {r4,r5,r6,r7} = 10 words. Reserved once, for the whole excursion's
 * duration.
 *
 * Deliberately does NOT include the entry procedure's own out-of-window
 * arguments, which enterDispatch also pushes: those are operand-stack
 * words like any other frame slot, and operandStackBytes already charges
 * 4 bytes for every slot of totalDepth with no window credit, while
 * validateProgram seeds totalDepth at each procedure's own argCount. The
 * relationship that makes that true is enforced, not assumed — see
 * enter_program.cpp's RESOURCE_PROGRAM_ENTRY_DEPTH check. */
#define ENTER_DISPATCH_FIXED_BYTES 40

/* enterProgramCore's own frame — the one C frame that is established
 * *after* enterProgram*'s up-front stack check has already run, so unlike
 * either public entry point's frame (which sp already reflects when
 * currentSp() reads it) this one has to come out of the reservation. It
 * covers the staged EntryArgs descriptor (entry_args.h, 28 bytes) and the
 * inlined Runtime::init that builds the per-procedure directory.
 *
 * GCC-measured at -Os with test/qemu's own flags, not estimated, and
 * enforced on every build by test/qemu/check_stack_usage.py the same way
 * TRANSLATOR_ENTRY_WORST_CASE_BYTES's own chain is — this replaced a
 * hand-guessed 32-byte "staging" term that undercounted the real frame by
 * 56 bytes. The VLA enterProgramWithHeader allocates just above it is
 * accounted separately and exactly, by requiredStackBytes' own
 * storageBytesFor(procCount) term. */
#define ENTER_PROGRAM_CORE_FRAME_BYTES 88

/* Fixed, one-time cost of getting from translatorTrampoline's own entry
 * down to the point where the real per-level recursion begins
 * (processNonTerminators/processUntilTerminator, translate_proc.cpp).
 * That recursion has no static whole-program worst case; it is policed
 * live instead, at every level (translate_proc.cpp's checkStackFloor,
 * reading Runtime::liveStackFloor() fresh on each call — see
 * test/qemu/Makefile's stack-usage-check target for how its own per-level
 * byte cost is kept honest against what GCC actually measures).
 *
 * Re-derived from scratch via `arm-none-eabi-g++ -Os -fstack-usage`
 * against the exact flags test/qemu's own build uses (optimization/
 * inlining-dependent — re-derive from a real build, never estimate).
 * `translateBody`/`translateLoop`/`translateIfThen`/`translateIfThenElse`/
 * `translateSwitch` do not appear in `.su` output at all at this
 * optimization level — confirmed fully inlined into `translateProc`/
 * `processNonTerminators` respectively, not separate frames to budget
 * here. The one real, straight-line chain from `translatorTrampoline`'s
 * entry to the deepest point that can run before the per-level recursion
 * takes over is:
 *
 *   translatorTrampoline's push{r0,r1,r2,lr} (16, runtime.S:196) +
 *   REALIGN_ENTER's worst case (12: up to 4 for 8-byte realignment,
 *   always 8 more to save the pre-realignment sp, runtime.S:165-171) = 28
 *   (asm, not `.su`-measurable — re-verify by re-reading runtime.S
 *   directly if either macro/prologue ever changes shape)
 * + compileProc (200, runtime/compile_proc.cpp — down from a previously
 *   documented 224 now that the dead `calleeArgCounts` VLA is gone)
 * + translateProc (120, compiler/src/translate_proc.cpp — includes
 *   translateBody's own inlined body; previously documented as 144
 *   modeling translateBody as a separate frame, which is no longer how
 *   this compiles)
 * + the deepest a single `emit()` call inside `emitPrologueStub` can
 *   reach if the arena happens to be full at that exact moment:
 *   abiEmitPrologue (16) + emitPrologueStub (16) + Assembler::emit (16) +
 *   Assembler::growForAttached (48) = 96 (compiler/src/abi_strategy.cpp,
 *   compiler/src/assembler.cpp — Runtime::findEvictionVictim/evict, both
 *   defined in runtime_internal.h, are confirmed inlined into
 *   growForAttached, not separate frames)
 *
 * 28 + 200 + 120 + 96 = 444.
 *
 * Enforced at build time by test/qemu/Makefile's `stack-usage-check`
 * target: every named function above is checked against this same
 * expected value on every build, failing loudly (not just on increase —
 * also if one shrinks, which is exactly how this constant went stale
 * last time) if `-fstack-usage`'s real measurement ever drifts. */
#define TRANSLATOR_ENTRY_WORST_CASE_BYTES (28 + 200 + 120 + 96)

#endif /* JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_ */
