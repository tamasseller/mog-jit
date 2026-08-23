/* enterProgram's family — a real dispatch table and info block, call/return
 * records living on the ordinary operand stack rather than a separate
 * control stack, and eviction+compaction (compile_proc_real.cpp, through
 * Runtime's own encapsulated methods in runtime_internal.h) driven by a
 * deliberately small arenaSize.
 */

#include <stdint.h>
#include <stddef.h>
#include "runtime_internal.h"

#define ARENA_CAPACITY 512

/* This ABI's own fixed costs, for the stack-usage accounting the
 * *OnStack/*Split variants below need — every one of these is a property
 * of this implementation, measured once, not something that varies per
 * program. Manual sync points: nothing statically ties these numbers back
 * to runtime.S's own instruction sequences the way
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
 * (translateProc's stackFloor parameter). Four measured terms:
 * translatorTrampoline's own push{r0,r1,r2} plus REALIGN_ENTER's worst-case
 * reservation (24B total); compileProc's own 104-byte frame (`-fstack-usage`
 * at `-Os`, includes its own calleeArgCounts[8] local — bailOut and every
 * Runtime method it calls are fully inlined into it); translateProc's own
 * 176-byte frame (same technique, includes Ctx as a local; translateBody's
 * own recursive frames are a separate call, not folded in here); and
 * whichever of memcpy/memmove compileProc calls to land the translated
 * output in the arena — 20 bytes, measured via objdump on the linked
 * newlib-nano routine.
 *
 * Not yet enforced at build time via a per-file `-Wstack-usage=`/
 * `-Werror=stack-usage=` pin — wiring that into this Makefile's own
 * ultimate-makefile-based object rules is a reasonable follow-up, not done
 * here. */
#define TRANSLATOR_ENTRY_WORST_CASE_BYTES (24 + 104 + 176 + 20)

static uint8_t arenaStorage[ARENA_CAPACITY];

extern "C" {
extern void translatorTrampoline(void); /* runtime.S */
extern const uint16_t callHelper[];              /* runtime.S */
extern const uint16_t returnHelperFromLr[];      /* runtime.S */
extern const uint16_t returnHelperFromStack[];   /* runtime.S */
extern const uint16_t returnHelperTail[];        /* runtime.S */
extern const uint16_t clzHelper[];               /* runtime.S */
extern const uint16_t revbitsHelper[];           /* runtime.S */
extern const uint16_t brTableJumpHelper[];       /* runtime.S */
extern uint64_t enterDispatch(uint32_t argIn, Runtime *runtime); /* runtime.S */
}

/* A plain fixed flash symbol, not per-Runtime state — every program
 * execution points every uncompiled slot at the same address. No `| 1u`
 * needed: .thumb_func (runtime.S) already bakes the Thumb bit into
 * translatorTrampoline's own symbol value. `extern` on the definition
 * itself, not just the declaration in runtime_internal.h: a const global
 * defaults to internal linkage in C++, unlike C. */
extern const uint32_t trampolineAddr = (uint32_t)(uintptr_t)translatorTrampoline;

/* r10 (helper vector base) — fixed for the whole program's lifetime, so
 * link-time const rather than something enterProgram fills in on every
 * call. No `| 1u` needed: .thumb_func (runtime.S) already bakes the Thumb
 * bit into each of these seven symbols' own value. Index 3
 * (returnHelperTail) is reached directly only by the rare non-leaf-with-
 * deep-args case, which does its own record fetch/reclaim inline and skips
 * both fetch variants. Indices 4-6 (clzHelper/revbitsHelper/
 * brTableJumpHelper) are the reserved software-helper slots — see
 * runtime.S's own header above those three symbols. */
extern const uint32_t helperVec[7] = {
    (uint32_t)(uintptr_t)callHelper,
    (uint32_t)(uintptr_t)returnHelperFromLr,
    (uint32_t)(uintptr_t)returnHelperFromStack,
    (uint32_t)(uintptr_t)returnHelperTail,
    (uint32_t)(uintptr_t)clzHelper,
    (uint32_t)(uintptr_t)revbitsHelper,
    (uint32_t)(uintptr_t)brTableJumpHelper,
};

/* Layout-agnostic core: every region (runtime itself, and the code arena it
 * points arenaBase/arenaEnd at) is handed in by address, and nothing here
 * cares whether either came from a C-stack VLA, a static global, or some
 * platform-specific memory the caller already owns. runtime must already
 * point at storage big enough for Runtime::storageBytesFor(procCount)
 * bytes. */
static ProgramResult enterProgramCore(
    uint32_t argIn,
    Runtime *runtime,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    const FlashProc *procs, uint32_t procCount,
    uint32_t stackLimit, uint32_t arenaOverlapsStack)
{
    runtime->init(codeArenaBase, codeArenaSize, procs, procCount, stackLimit, arenaOverlapsStack);

    uint64_t packed = enterDispatch(argIn, runtime);
    return ProgramResult{ (uint32_t)packed, (uint32_t)(packed >> 32) };
}

/* How many bytes of C stack the whole excursion needs, worst case, below
 * wherever sp sits right now: Runtime itself, the operand stack's own
 * worst-case depth, the worst-case live call/return record depth, this
 * implementation's own fixed overhead, and the caller's own interrupt-frame
 * allowance.
 *
 * operandStackBytes and maxCallDepth are deliberately not derived from
 * procs/procCount here — there's no way to introspect an arbitrary
 * machine-code blob's own worst-case stack behavior at runtime. Both are
 * static, whole-program properties a real translator's own front end has
 * to compute before ever calling in here (for bytecode compiled via
 * @ppl/machine, maxCallDepth comes from validateProgram's own call-graph
 * DFS, and operandStackBytes is totalDepth * 4 — the whole tight TOS-depth
 * bound, not a window-credited fraction of it, since the window's actual
 * absorption depends on call-boundary argument shuffling that abstract
 * depth alone doesn't capture). */
static uint32_t requiredStackBytes(
    uint32_t procCount, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t interruptReserve)
{
    return Runtime::storageBytesFor(procCount)
         + operandStackBytes
         + maxCallDepth * CALL_RECORD_BYTES
         + ENTER_DISPATCH_FIXED_BYTES
         + TRANSLATOR_ENTRY_WORST_CASE_BYTES
         + interruptReserve;
}

static uint32_t currentSp()
{
    register uint32_t sp asm("sp");
    return sp;
}

/* True iff needed more bytes can be reserved below the current sp without
 * reaching or passing stackLimit — read directly out of the hardware
 * register, not threaded through as a parameter, since the whole point is
 * checking the actual current pointer before committing to any VLA that
 * would otherwise silently smash whatever memory sits below it. */
static bool stackHasRoom(uint32_t needed, uint32_t stackLimit)
{
    uint32_t sp = currentSp();
    if(sp < needed)
    {
        return false; /* would wrap computing sp - needed */
    }
    return (sp - needed) >= stackLimit;
}

/* Plain enterProgram takes no stackLimit at all — it never checks anything
 * stack-related except the translator's own LOOP/BR_TABLE recursion, which
 * gets this fixed, conservative margin below sp at entry instead of a
 * caller-supplied bound. Callers that need a tighter, caller-verified bound
 * should use enterProgramOnStack/enterProgramSplit instead. */
#define GENEROUS_TRANSLATOR_STACK_MARGIN 4096

extern "C" ProgramResult enterProgram(
    uint32_t argIn, uint32_t arenaSize,
    const FlashProc *procs, uint32_t procCount)
{
    /* One flexible-array-member object, over-allocated to fit procCount+1
     * dispatch entries (index 0 = sentinel) — sized and aligned by hand
     * since a plain `Runtime runtime;` local would only reserve the fixed
     * header. */
    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(procCount)];
    return enterProgramCore(argIn, reinterpret_cast<Runtime *>(runtimeStorage),
        (uint32_t)(uintptr_t)arenaStorage, arenaSize, procs, procCount,
        currentSp() - GENEROUS_TRANSLATOR_STACK_MARGIN, /*arenaOverlapsStack=*/0);
}

/* Variant: the current C stack is the whole work area — Runtime, its
 * dispatch table, the operand stack, and the compiled-code arena all come
 * out of it, but not as two blocks stacked one after the other. The arena
 * anchors at stackLimit itself and grows up from there; Runtime and
 * everything enterDispatch touches grow down from wherever sp already is.
 * The two regions converge on each other from opposite ends of the same
 * checked range instead of sitting on top of one another with nothing
 * between them — this leaves room for a real translator to temporarily
 * encroach into whatever part of the arena's own reservation is still free.
 *
 * The checked total is unchanged either way — codeArenaSize still counts
 * against stackLimit below; only where, within that already-reserved
 * range, the arena sits is different. On failure, reports RESOURCE_ERROR
 * directly — enterDispatch/Runtime were never set up, so there's nothing
 * else to unwind. */
extern "C" ProgramResult enterProgramOnStack(
    uint32_t argIn,
    const FlashProc *procs, uint32_t procCount,
    uint32_t codeArenaSize, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t stackLimit, uint32_t interruptReserve)
{
    uint32_t needed = requiredStackBytes(procCount, operandStackBytes, maxCallDepth, interruptReserve)
                     + codeArenaSize;
    if(!stackHasRoom(needed, stackLimit))
    {
        return ProgramResult{ RESOURCE_ERROR_CODE, 1 };
    }

    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(procCount)];
    return enterProgramCore(argIn, reinterpret_cast<Runtime *>(runtimeStorage),
        stackLimit, codeArenaSize, procs, procCount,
        stackLimit, /*arenaOverlapsStack=*/1);
}

/* Variant: the compiled-code arena lives in caller-supplied memory — a
 * distinct SRAM bank, CCM, whatever a given target's own bus layout wants
 * — while Runtime, its dispatch table, and the operand stack still live on
 * the current C stack. codeArenaSize deliberately isn't part of the stack
 * check below — that memory isn't on this stack at all, so it's the
 * caller's own responsibility to have sized it correctly. */
extern "C" ProgramResult enterProgramSplit(
    uint32_t argIn,
    const FlashProc *procs, uint32_t procCount,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t stackLimit, uint32_t interruptReserve)
{
    uint32_t needed = requiredStackBytes(procCount, operandStackBytes, maxCallDepth, interruptReserve);
    if(!stackHasRoom(needed, stackLimit))
    {
        return ProgramResult{ RESOURCE_ERROR_CODE, 1 };
    }

    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(procCount)];
    return enterProgramCore(argIn, reinterpret_cast<Runtime *>(runtimeStorage),
        codeArenaBase, codeArenaSize, procs, procCount,
        stackLimit, /*arenaOverlapsStack=*/0);
}
