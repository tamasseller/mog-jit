#ifndef JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_
#define JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_

#include <stdint.h>
#include "runtime.h"
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

#define CALL_RECORD_BYTES 4

/* extThunkHelper: 4 pushed lr + 8 REALIGN_ENTER slack. An extension's own C
 * helper adds its declared helperStackBytes (compiler/src/ext.h) on top. */
#define EXT_THUNK_STACK_BYTES 12

/* enterDispatch's two prologue pushes, 10 words. Excludes the entry
 * procedure's out-of-window arguments: operandStackBytes already charges
 * every totalDepth slot, enforced by enter_program.cpp's
 * RESOURCE_PROGRAM_ENTRY_DEPTH check. */
#define ENTER_DISPATCH_FIXED_BYTES 40

/* enterProgramCore's frame — established after the up-front stack check, so
 * it comes out of the reservation. GCC-measured at -Os with test/qemu's
 * flags, not estimated. */
#define ENTER_PROGRAM_CORE_FRAME_BYTES 88

/* translatorTrampoline entry down to where per-level recursion begins
 * (processNonTerminators/processUntilTerminator, policed live instead by
 * translate_proc.cpp's checkStackFloor). Terms, -Os with test/qemu's flags:
 *
 *   28  translatorTrampoline push{r0,r1,r2,lr} (16) + REALIGN_ENTER (12);
 *       asm, not .su-measurable — verify against runtime.S by hand
 *   200 compileProc
 *   120 translateProc (translateBody inlines into it)
 *   96  deepest single emit() with the arena full: abiEmitPrologue (16) +
 *       emitPrologueStub (16) + Assembler::emit (16) + growForAttached (48)
 *
 * check_stack_usage.py fails on drift in either direction. */
#define TRANSLATOR_ENTRY_WORST_CASE_BYTES (28 + 200 + 120 + 96)

#endif /* JIT_ARMV6M_RUNTIME_DISPATCH_ABI_H_ */
