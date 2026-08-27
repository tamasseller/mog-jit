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

/* enterDispatch's own two prologue pushes: {r2,r4,r5,r6,r7,lr} +
 * {r4,r5,r6,r7} = 10 words. Reserved once, for the whole excursion's
 * duration. */
#define ENTER_DISPATCH_FIXED_BYTES 40

/* Fixed, one-time cost of getting from translatorTrampoline's own entry
 * down to translateBody's own first call. The recursion beyond that point
 * has no static whole-program worst case; that is policed live instead
 * (Runtime::liveStackFloor(), read fresh by translateBody's own guard).
 *
 * STALE, NEEDS RE-DERIVING: the 96 below (and so the 488 total) was
 * measured against an Assembler::materializeImm32 that no longer exists
 * in this shape — it called out to a separate emitSynthesizeImm32Into for
 * byte-by-byte synthesis; that's gone, folded into materializeImm32
 * itself (an imm8-direct / bitwise-NOT-of-imm8 / shifted-imm8 repertoire,
 * falling back to the pool only outside all three). Re-deriving isn't
 * just a search-and-replace: the "last-argument-fold scan" call site this
 * 96 was anchored on (translate_proc.cpp's accState.flush(a,
 * physReg(lastArgSlot)) right before translateBody's first call) turns
 * out to never actually reach materializeImm32 at all — accState is
 * freshly constructed (Kind::Clean) and every producer() call reaching
 * that point sets Shape::ofReg(ACC_REG), never a pending immediate, so
 * materializeShape's imm branch is dead code from there. Whatever the
 * true second-deepest one-time chain is (through Assembler::reserve at
 * the top of translateProc, or through CONST/IMM_ACC's own
 * materializeImm32 call inside translateBody — which may belong to the
 * *live*, per-recursion-level budget instead, not this fixed one-time
 * one) needs re-tracing before this number can be trusted again.
 *
 * Last known-good derivation (for compileProc as of the Assembler
 * restructuring, docs/assembler-restructuring.md), via `-fstack-usage`
 * against every function actually on this path — kept here as the
 * starting point for the re-derivation above, not as a currently
 * accurate one: two candidate chains both run sequentially before
 * translateBody's first call — prologue emission, then the
 * last-argument-fold scan — so the worse of the two, not their sum, sets
 * this constant: translatorTrampoline's own push{r0,r1,r2} plus
 * REALIGN_ENTER's worst-case reservation (24, asm, unchanged from
 * before); compileProc's own static frame (224 — up from 96: it now
 * holds the Assembler object itself as a local rather than a separate
 * RuntimeArenaRoom; the calleeArgCounts VLA this frame size once excluded
 * and budgeted separately has since been deleted entirely, dead code —
 * nothing to account for on that front any more); translateProc's own frame (144, includes Ctx as a
 * local; translateBody's own recursive frames are a separate call, not
 * folded in here); then the deeper of — a.reserve(STUB_SIZE+2) (16) +
 * Assembler::growForAttached (48), called once before abiEmitPrologue
 * even runs, versus AccState::flush (32) + materializeShape (8) +
 * Assembler::materializeImm32 (16) + Assembler::emitSynthesizeImm32Into
 * (40) = 96 (the reachability of which is exactly what's now in
 * question, above) — the second chain taken as deeper.
 * 24+224+144+96 = 488.
 *
 * Not yet enforced at build time via a per-file `-Wstack-usage=`/
 * `-Werror=stack-usage=` pin — wiring that into this Makefile's own
 * ultimate-makefile-based object rules is a reasonable follow-up, not done
 * here. */
#define TRANSLATOR_ENTRY_WORST_CASE_BYTES (24 + 224 + 144 + 96)

#endif /* JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_ */
