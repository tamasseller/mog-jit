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
extern uint64_t enterDispatch(uint32_t argIn, Runtime *runtime); /* runtime.S */
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

/* compileProc's own callee-argCount lookup table: one uint32_t per
 * procedure (ProcSlot.argCount(), read once per compile and copied into a
 * dense array — abiEmitCall needs O(1) indexing by calleeIndex, and
 * ProcSlot's own 16-byte stride doesn't give it that for free), sized to
 * procCount via a VLA (the same GCC/C++ extension enter_program.cpp's own
 * runtimeStorage already relies on) rather than a fixed cap — so it has
 * to be budgeted explicitly here rather than folded into a fixed frame
 * size the way it briefly was. */
#define CALLEE_ARG_COUNTS_BYTES_PER_PROC 4

/* enterDispatch's own two prologue pushes: {r2,r4,r5,r6,r7,lr} +
 * {r4,r5,r6,r7} = 10 words. Reserved once, for the whole excursion's
 * duration. */
#define ENTER_DISPATCH_FIXED_BYTES 40

/* Fixed, one-time cost of getting from translatorTrampoline's own entry
 * down to translateBody's own first call. The recursion beyond that point
 * has no static whole-program worst case; that is policed live instead
 * (Assembler::stackFloor(), read fresh by translateBody's own guard).
 *
 * Re-measured for the Assembler-based compileProc (compiler/src/
 * assembler.{h,cpp} — the arena-owning seam moved out of a separate
 * ArenaRoom pointer into the Assembler itself, and the literal pool moved
 * from translate_proc.cpp's own Ctx into the Assembler too), via
 * `-fstack-usage` against every function actually on this path. Two
 * candidate chains both run sequentially before translateBody's first
 * call — prologue emission, then the last-argument-fold scan — so the
 * worse of the two, not their sum, sets this constant:
 *
 * translatorTrampoline's own push{r0,r1,r2} plus REALIGN_ENTER's
 * worst-case reservation (24, asm, unchanged from before); compileProc's
 * own static frame (224 — up from 96: it now holds the Assembler object
 * itself as a local rather than a separate RuntimeArenaRoom; the
 * calleeArgCounts VLA is still excluded, budgeted separately above);
 * translateProc's own frame (144, includes Ctx as a local; translateBody's
 * own recursive frames are a separate call, not folded in here); then the
 * deeper of — a.reserve(STUB_SIZE+2) (16) + Assembler::growForAttached
 * (48), called once before abiEmitPrologue even runs, versus
 * AccState::flush (32) + materializeShape (8) + Assembler::materializeImm32
 * (16) + Assembler::emitSynthesizeImm32Into (40) = 96, reached if the
 * last-argument-fold scan's own eager-flush path needs full synthesis —
 * the second chain is deeper. 24+224+144+96 = 488.
 *
 * Not yet enforced at build time via a per-file `-Wstack-usage=`/
 * `-Werror=stack-usage=` pin — wiring that into this Makefile's own
 * ultimate-makefile-based object rules is a reasonable follow-up, not done
 * here. */
#define TRANSLATOR_ENTRY_WORST_CASE_BYTES (24 + 224 + 144 + 96)

#endif /* JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_ */
