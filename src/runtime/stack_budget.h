#ifndef JIT_ARMV6M_RUNTIME_STACK_BUDGET_H_
#define JIT_ARMV6M_RUNTIME_STACK_BUDGET_H_

/* Every byte of stack the JIT can consume, in one place.
 *
 * There is no MPU and no MSPLIM on a Cortex-M0, so overflow is silent
 * corruption. Two mechanisms keep it bounded, and each number below belongs to
 * exactly one of them:
 *
 *  - Reserved up front. enterProgram sums these once and refuses the program
 *    if the measured sp cannot cover them. They bound the regions no runtime
 *    check ever sees.
 *
 *  - Checked per level. A recursion that cannot be bounded statically carries
 *    a guard at the single function every one of its cycles passes through;
 *    the guard demands the margin below is still free and bails if it is not.
 *
 * The C++ numbers are measured from GCC's own call graph by
 * tools/stack-margin.ts, not traced by hand, and test/qemu's
 * stack-usage-check re-derives and gates them on every build. The assembly
 * numbers are the only hand-maintained ones, since no .ci file describes
 * runtime.S. */

/* --- reserved up front --------------------------------------------------- */

/* enterDispatch's own frame, from runtime.S. */
#define ENTER_DISPATCH_FIXED_BYTES 36

/* enterProgramCore's frame, excluding the Runtime it places (that is sized
 * separately from procCount). */
#define ENTER_PROGRAM_CORE_FRAME_BYTES 88

/* translatorTrampoline's push {r0, r1, r2, lr} plus REALIGN_ENTER's worst
 * case, from runtime.S. */
#define TRANSLATOR_ENTRY_ASM_BYTES 24

/* translateProc down to the first stack guard — or to a leaf, on the paths
 * that never reach one. Nothing checks this region at run time, so it has to
 * be reserved in full. The body scan has the same kind of unguarded prefix and
 * runs from the same frame one phase earlier, so it is gated against this
 * number too rather than reserved separately. */
#define TRANSLATOR_ENTRY_CPP_BYTES 472

#define TRANSLATOR_ENTRY_WORST_CASE_BYTES (TRANSLATOR_ENTRY_ASM_BYTES + TRANSLATOR_ENTRY_CPP_BYTES)

/* What extThunkHelper spends before it reaches an extension's C helper. */
#define EXT_THUNK_STACK_BYTES 12

/* --- checked per level --------------------------------------------------- */

/* One level of the translator's recursion: from the guard in
 * GUARDED_processUntilTerminator down to the next guard, or to a leaf.
 * Includes that function's own frame, which is already spent when its check
 * runs — the slack that buys a rule with no prologue-placement assumption. */
#define TRANSLATE_LEVEL_STACK_MARGIN 416

/* The same, for the pre-pass that walks a procedure body before translation.
 * GUARDED_scanBody recurses on block nesting and checks its own floor per level. */
#define SCAN_STACK_MARGIN 160

#endif /* JIT_ARMV6M_RUNTIME_STACK_BUDGET_H_ */
