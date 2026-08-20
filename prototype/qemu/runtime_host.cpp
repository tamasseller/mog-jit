/* enter_program's family (docs/jit-armv6m-dispatch-handoff.html §09) — a
 * real dispatch table and info block, call/return records living on the
 * ordinary operand stack rather than a separate control stack, and
 * eviction+compaction (compile_proc.cpp, through Runtime's own
 * encapsulated methods — runtime_internal.h) driven by a deliberately
 * small `arenaSize`. The mock translator itself, and the `Runtime`/
 * `DispatchEntry` layout it shares with this file, live in
 * compile_proc.cpp/runtime_internal.h now — split out purely so
 * compile_proc.cpp's own stack usage can be enforced at build time
 * (qemu/Makefile) independently of the `enter_program_on_stack`/
 * `enter_program_split` variants' own, genuinely unbounded-at-compile-time
 * VLAs.
 */

#include <stdint.h>
#include <stddef.h>
#include "runtime_internal.h"

#define ARENA_CAPACITY 512

/* This ABI's own fixed costs, for the stack-usage accounting the *_on_stack
 * / *_split variants below need — every one of these is a property of
 * THIS implementation, measured once, not something that varies per
 * program (that's operandStackBytes/maxCallDepth's job, see
 * requiredStackBytes). Manual sync points: nothing statically ties these
 * numbers back to qemu/runtime.S's own instruction sequences the way
 * RUNTIME_DISPATCH_TABLE_OFFSET's static_assert does — re-measure if
 * enter_dispatch's prologue, translator_trampoline, or REALIGN_ENTER ever
 * change shape. */

/* callHelper/returnHelper's own pushed/popped record (qemu/runtime.S) —
 * one word today, per CALL/RETURN pair live on the operand stack. */
#define CALL_RECORD_BYTES 4

/* enter_dispatch's own two prologue PUSHes (qemu/runtime.S): {r2,r4,r5,r6,
 * r7,lr} + {r4,r5,r6,r7} = 10 words. Reserved once, for the whole
 * excursion's duration — never popped until the final return — so this
 * coexists with literally everything else accounted for below. */
#define ENTER_DISPATCH_FIXED_BYTES 40

/* Worst-case *transient* depth beneath one translator entry, all of it
 * momentary (present only while actually compiling a procedure):
 * translator_trampoline's own push{r0,r1,r2} (12B) plus REALIGN_ENTER's
 * worst-case reservation (24B total there, qemu/runtime.S — see its own
 * comment for the two-case derivation), then compileProc's own 48-byte
 * frame (measured via `-fstack-usage`, enforced at build time —
 * compile_proc.cpp's own Makefile rule; bailOut and every Runtime method
 * it calls are fully inlined into it at -Os, confirmed via the same
 * measurement — no separate frame of its own) plus whichever of
 * memcpy/memmove it calls — 20 bytes each, measured via objdump on the
 * linked newlib-nano routines: both are leaf (no further calls, no
 * `sub sp` beyond their own fixed `push {r4-r7,lr}`), so it's a max,
 * never both at once, never nested.
 *
 * MOCK_ on purpose: this is only a fixed constant because compileProc
 * *is* the mock translator (test/qemu-run-abi.ts's own header) — a plain,
 * unconditionally-terminating memcpy from an already-compiled blob, no
 * recursion, no unbounded nesting. docs/jit-armv6m-dispatch-handoff.html
 * §09's own "translator's own exception" section already establishes why
 * a *real* translator can't be reserved for this way: BR_TABLE/LOOP
 * nesting has no static, program-wide worst case the way the operand
 * stack does, so a real translator's own stack usage has to be checked
 * *live*, against however much headroom remains, not folded into this
 * fixed sum. Swapping in a real translator means replacing this term
 * (not just re-measuring it) with that live check. */
#define MOCK_TRANSLATOR_ENTRY_WORST_CASE_BYTES (24 + 48 + 20)

static uint8_t g_arenaStorage[ARENA_CAPACITY];

extern "C" {
extern void translator_trampoline(void); /* qemu/runtime.S */
extern const uint16_t callHelper[];       /* qemu/runtime.S */
extern const uint16_t returnHelper[];     /* qemu/runtime.S */
extern uint64_t enter_dispatch(uint32_t argIn, Runtime *runtime); /* qemu/runtime.S */
}

/* A plain fixed flash symbol, not per-Runtime state — every program
 * execution points every uncompiled slot at the same address, so there's
 * nothing to gain by carrying a copy of it inside `Runtime`. No `| 1u`
 * needed, same reasoning as `g_helperVec` just below: `.thumb_func`
 * (qemu/runtime.S) already bakes the Thumb bit into translator_trampoline's
 * own symbol value. `extern` on the definition itself (not just the
 * declaration in runtime_internal.h): a `const` global defaults to
 * *internal* linkage in C++, unlike C, so without repeating it here
 * compile_proc.cpp couldn't see this at all. Not `extern "C"` — plain
 * data symbols are never name-mangled in the first place, only
 * functions/overloads are, confirmed empirically before relying on it. */
extern const uint32_t g_trampolineAddr = (uint32_t)(uintptr_t)translator_trampoline;

/* r10 (helper vector base) — fixed for the whole program's lifetime, so
 * link-time const rather than something enter_program fills in on every
 * call. No `| 1u`/`+ 1u` needed: `.thumb_func` (qemu/runtime.S) already
 * bakes the Thumb bit into callHelper/returnHelper's own symbol value
 * (confirmed via readelf — their st_value is already odd), and the plain
 * R_ARM_ABS32 relocation this cast produces resolves against that value
 * directly. */
extern const uint32_t g_helperVec[2] = {
    (uint32_t)(uintptr_t)callHelper,
    (uint32_t)(uintptr_t)returnHelper,
};

/** Layout-agnostic core: every region (`runtime` itself, and the code
 *  arena it points `arenaBase`/`arenaEnd` at) is handed in by address, and
 *  nothing here cares whether either came from a C-stack VLA, a static
 *  global, or some platform-specific memory the caller already owns.
 *  `runtime` must already point at storage big enough for
 *  `Runtime::storageBytesFor(procCount)` bytes — every caller below is
 *  the one deciding *where* that storage lives; this function only fills
 *  it in (via `Runtime::init`) and runs the excursion. */
static ProgramResult enterProgramCore(
    uint32_t argIn,
    Runtime *runtime,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    const FlashProc *procs, uint32_t procCount)
{
    runtime->init(codeArenaBase, codeArenaSize, procs, procCount);

    /* enter_dispatch (qemu/runtime.S) does the actual excursion — an
     * ordinary AAPCS call, so nothing here needs a clobber list. Its result
     * comes back as a uint64_t register pair (r0:r1), not a 2-word struct:
     * that's specifically about enter_dispatch being hand-written assembly
     * — returning a plain register pair means the asm never has to compute
     * or write through a hidden-pointer argument. `ProgramResult` itself
     * (8 bytes, over AAPCS32's 4-byte register-return threshold) still goes
     * through a hidden pointer once it gets here, but the compiler
     * synthesizes that automatically for an ordinary by-value struct
     * return — there's no hand-written asm on this side of the boundary
     * for it to burden. */
    uint64_t packed = enter_dispatch(argIn, runtime);
    return ProgramResult{ (uint32_t)packed, (uint32_t)(packed >> 32) };
}

/** How many bytes of C stack the whole excursion needs, worst case, below
 *  wherever `sp` sits right now: `Runtime` itself (fixed header plus one
 *  dispatch entry per procedure, plus the sentinel — `Runtime::
 *  storageBytesFor`), the operand stack's own worst-case depth, the
 *  worst-case live call/return record depth, this implementation's own
 *  fixed overhead (`ENTER_DISPATCH_FIXED_BYTES` +
 *  `MOCK_TRANSLATOR_ENTRY_WORST_CASE_BYTES`, both defined above — the
 *  latter's own comment explains why it's only a fixed constant for
 *  *this* mock translator), and the caller's own interrupt-frame
 *  allowance.
 *
 *  `operandStackBytes` and `maxCallDepth` are deliberately *not* derived
 *  from `procs`/`procCount` in here — there's no way to introspect an
 *  arbitrary machine-code blob's own worst-case stack behavior at
 *  runtime, any more than `arenaSize` (the code-space analogue) has ever
 *  been derived that way. Both are static, whole-program properties a
 *  real translator's own front end has to compute before ever calling in
 *  here. For a program compiled from real bytecode via `@ppl/machine`,
 *  the robust source for `maxCallDepth` is `validateProgram`'s
 *  already-implemented, already-tested tight bound
 *  (packages/machine/src/validate.ts) — the worst-case call-chain length
 *  falls out of the same DFS that computes `totalDepth` (`totalDepthOf`'s
 *  own recursion depth).
 *
 *  `operandStackBytes` is `totalDepth * 4` — the *whole* tight TOS-depth
 *  bound, not `totalDepth` minus some credit for the 4-register window.
 *  An earlier version of this comment claimed
 *  `max(0, totalDepth - 4) * 4`, reasoning that the window always
 *  absorbs the top 4 live slots for free. Wrong in general, on two
 *  counts: a worst-case path can end in a leaf that's pure acc-in/acc-out
 *  (argCount 1, no PUSH at all — see src/window.ts's own "spillForCall"),
 *  crediting *zero* window absorption at its own peak; and a `CALL` site
 *  spills whatever's resident but *not* one of the outgoing arguments
 *  regardless of whether the window had room for it (the callee needs a
 *  canonical register layout for its own incoming args, not "whatever's
 *  currently full") — so depth alone doesn't determine how much the
 *  window actually absorbs at any given moment. `totalDepth * 4` is the
 *  safe bound derivable from what `validateProgram` already computes
 *  today; a tighter one would need a new analysis that tracks *actual*
 *  spilled bytes through the real call-boundary shuffling `window.ts`
 *  performs, not just abstract TOS depth. */
static uint32_t requiredStackBytes(
    uint32_t procCount, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t interruptReserve)
{
    return Runtime::storageBytesFor(procCount)
         + operandStackBytes
         + maxCallDepth * CALL_RECORD_BYTES
         + ENTER_DISPATCH_FIXED_BYTES
         + MOCK_TRANSLATOR_ENTRY_WORST_CASE_BYTES
         + interruptReserve;
}

/** True iff `needed` more bytes can be reserved below the current `sp`
 *  without reaching or passing `stackLimit` — read directly out of the
 *  hardware register, not threaded through as a parameter, since the
 *  whole point is checking the *actual* current pointer before
 *  committing to any VLA that would otherwise just silently smash
 *  whatever memory sits below it (no MPU-backed guard page on Cortex-M0
 *  to catch that after the fact). */
static bool stackHasRoom(uint32_t needed, uint32_t stackLimit)
{
    register uint32_t sp_ asm("sp");
    if(sp_ < needed) return false; /* would wrap computing sp_ - needed */
    return (sp_ - needed) >= stackLimit;
}

extern "C" ProgramResult enter_program(
    uint32_t argIn, uint32_t arenaSize,
    const FlashProc *procs, uint32_t procCount)
{
    /* One flexible-array-member object, over-allocated to fit procCount+1
     * dispatch entries (index 0 = sentinel) — sized and aligned by hand
     * since a plain `Runtime runtime;` local would only reserve the fixed
     * header (a FAM contributes 0 to sizeof), not the trailing slots. */
    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(procCount)];
    return enterProgramCore(argIn, reinterpret_cast<Runtime *>(runtimeStorage),
        (uint32_t)(uintptr_t)g_arenaStorage, arenaSize, procs, procCount);
}

/** Variant: the current C stack *is* the whole work area — `Runtime`, its
 *  dispatch table, the operand stack, *and* the compiled-code arena all
 *  come out of it — but not as two blocks stacked one after the other.
 *  The arena anchors at `stackLimit` itself and grows *up* from there
 *  (bump-allocating toward higher addresses, same as ever); `Runtime`
 *  and everything `enter_dispatch` touches grow down from wherever `sp`
 *  already is, same as `enter_program_split` below. The two regions
 *  converge on each other from opposite ends of the same checked range
 *  instead of sitting on top of one another with nothing between them —
 *  deliberately: sandwiching the arena between `Runtime` and the
 *  operand-stack region (an earlier version of this function did exactly
 *  that, via its own `codeArena[codeArenaSize]` VLA) leaves no room for
 *  a future *real* translator to ever ask "how much of the arena's own
 *  reservation is actually still free right now?" and temporarily
 *  encroach into it the way docs/jit-armv6m-dispatch-handoff.html §09's
 *  "translator's own exception" already describes — there'd be a whole
 *  `Runtime`-sized block in the way. With the arena anchored at
 *  `stackLimit`, that gap is exactly where the two regions actually
 *  meet, the same convergence §09's own diagram always assumed.
 *
 *  The checked total is unchanged either way — `codeArenaSize` still
 *  counts against `stackLimit` below, exactly as before; only *where*,
 *  within that already-reserved range, the arena sits is different.
 *  Checked against `stackLimit`/`interruptReserve` before any of that
 *  memory is touched; on failure, reports RESOURCE_ERROR directly —
 *  `enter_dispatch`/`Runtime` were never set up, so there's nothing else
 *  to unwind. */
extern "C" ProgramResult enter_program_on_stack(
    uint32_t argIn,
    const FlashProc *procs, uint32_t procCount,
    uint32_t codeArenaSize, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t stackLimit, uint32_t interruptReserve)
{
    uint32_t needed = requiredStackBytes(procCount, operandStackBytes, maxCallDepth, interruptReserve)
                     + codeArenaSize;
    if(!stackHasRoom(needed, stackLimit))
        return ProgramResult{ RESOURCE_ERROR_CODE, 1 };

    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(procCount)];
    return enterProgramCore(argIn, reinterpret_cast<Runtime *>(runtimeStorage),
        stackLimit, codeArenaSize, procs, procCount);
}

/** Variant: the compiled-code arena lives in caller-supplied memory — a
 *  distinct SRAM bank, CCM, whatever a given target's own bus layout
 *  wants — while `Runtime`, its dispatch table, and the operand stack
 *  still live on the current C stack, since the translator/helpers/
 *  extensions calling into this are just ordinary C using that same
 *  stack regardless of where the arena itself sits. `codeArenaSize`
 *  deliberately isn't part of the stack check below — that memory isn't
 *  on this stack at all, so it's the caller's own, separate
 *  responsibility to have sized it correctly. */
extern "C" ProgramResult enter_program_split(
    uint32_t argIn,
    const FlashProc *procs, uint32_t procCount,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t stackLimit, uint32_t interruptReserve)
{
    uint32_t needed = requiredStackBytes(procCount, operandStackBytes, maxCallDepth, interruptReserve);
    if(!stackHasRoom(needed, stackLimit))
        return ProgramResult{ RESOURCE_ERROR_CODE, 1 };

    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(procCount)];
    return enterProgramCore(argIn, reinterpret_cast<Runtime *>(runtimeStorage),
        codeArenaBase, codeArenaSize, procs, procCount);
}
