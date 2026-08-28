/* jit-armv6m/runtime — layer 1, initialization: enterProgramOnStack/
 * enterProgramSplit. A real dispatch table and info block, call/return
 * records living on the ordinary operand stack rather than a separate
 * control stack, and eviction+compaction (compile_proc.cpp, through
 * Runtime's own encapsulated methods in runtime_internal.h) driven by a
 * deliberately small arenaSize. The dispatch/eviction ABI's own fixed
 * conventions (the helper vector, the fixed-cost constants
 * requiredStackBytes below sums) live in dispatch_abi.{h,cpp} — layer 2,
 * this file's own caller-facing counterpart. Neither variant here owns any
 * arena storage of its own — that's always either the caller's own C
 * stack (enterProgramOnStack) or a region the caller names explicitly
 * (enterProgramSplit) — so there is no fixed capacity baked into this
 * file to outgrow.
 */

#include <stdint.h>
#include <stddef.h>
#include <cassert>
#include "runtime_internal.h"
#include "ext.h"
#include "dispatch_abi.h"
#include "entry_args.h"

/* The jit-armv6m-specific wire envelope (packages/machine/src/bytecode.ts's
 * encodeJitProgram) every enterProgram* variant now takes in place of a
 * caller-supplied procCount/operandStackBytes/maxCallDepth — isa-core.md
 * §5.5/§11.4's own extension point, since a bare-metal JIT needs those two
 * whole-program stats before it can compile a single instruction (the
 * static stack reservation below), and validateProgram already computed
 * them once, ahead of ever serializing the program at all. bodyOffset is
 * where proc_count's own successor — procedure 0's own arg_count LEB128 —
 * begins; Runtime::init() re-walks from there to build every slot's static
 * half (runtime_internal.h's ProcSlot). */
struct ProgramHeader
{
    uint32_t maxCallDepth;
    uint32_t totalDepth;
    uint32_t procCount;
    uint32_t bodyOffset;
};

static ProgramHeader parseProgramHeader(const uint8_t *bytes, uint32_t size)
{
    uint32_t pos = 0;
    assert(pos < size); // GCOV_EXCL_LINE — malformed/truncated program, matching decode_instr.cpp's own convention
    uint32_t maxCallDepth = jitc::decodeLeb128(bytes, pos, pos);
    assert(pos < size); // GCOV_EXCL_LINE
    uint32_t totalDepth = jitc::decodeLeb128(bytes, pos, pos);
    assert(pos < size); // GCOV_EXCL_LINE
    uint32_t procCount = jitc::decodeLeb128(bytes, pos, pos);
    return ProgramHeader{maxCallDepth, totalDepth, procCount, pos};
}

/* Layout-agnostic core: every region (runtime itself, and the code arena it
 * points arenaEnd/arenaCursor at) is handed in by address, and nothing here
 * cares whether either came from a C-stack VLA, a static global, or some
 * platform-specific memory the caller already owns. runtime must already
 * point at storage big enough for Runtime::storageBytesFor(hdr.procCount)
 * bytes.
 *
 * Runtime::init() doing the whole-program directory walk can itself fail
 * four ways (a procedure's own scan overflowed the stack or ran off the
 * blob, or its arg_count/body length doesn't fit ProcSlot's packed
 * fields). It names which as a RESOURCE_* code, forwarded verbatim
 * without ever reaching enterDispatch — the same shape as the
 * stack-budget checks below, which report their own.
 *
 * The two entry-argument checks here can only run after that walk: both
 * compare against the entry procedure's own declared arg_count, which is
 * exactly what init() just recorded in slot 0. */
static ProgramResult enterProgramCore(
    const uint32_t *args, uint32_t argCount,
    const ExtHooks *extension,
    Runtime *runtime,
    const uint8_t *programBytes, uint32_t programSize, const ProgramHeader &hdr,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t arenaOverlapsStack)
{
    if(uint32_t code = runtime->init(programBytes, programSize, hdr.bodyOffset, hdr.procCount,
        codeArenaBase, codeArenaSize, stackLimit, arenaOverlapsStack, extension); code != 0)
    {
        return ProgramResult{ code, LANDING_RESOURCE_ERROR };
    }

    const uint32_t declared = runtime->slot(0).argCount();

    /* No clamping and no zero-filling: the entry procedure reads exactly
     * the slots it declared, and the frame it reclaims on the way out is
     * sized from that same number, so a count that disagrees with the
     * program is not a value this can pick a default for. Reported rather
     * than asserted because it is a caller mistake, not malformed wire
     * bytes. */
    if(argCount != declared)
    {
        return ProgramResult{ RESOURCE_PROGRAM_ENTRY_ARG_COUNT, LANDING_RESOURCE_ERROR };
    }

    /* enterDispatch is about to push the entry procedure's out-of-window
     * arguments — everything below its 4-register window. Those words are
     * covered by operandStackBytes (= totalDepth * 4, charged per abstract
     * TOS slot with no window credit) *only* because validateProgram seeds
     * every procedure's own local peak at its argCount, so a well-formed
     * envelope never reports a totalDepth below procedures[0].argCount.
     * That is a property of the producer, and totalDepth arrives as
     * trusted wire data, so check it rather than assume it: arg_count is
     * 11 bits (ProcSlot), which unchecked would let a forged envelope
     * drive sp ~8KB below a reservation sized from the forged number.
     *
     * Deliberately bounds the *pushed words*, not argCount itself. The
     * stronger `declared > totalDepth` reads better as a restatement of
     * the producer's contract, but it is stricter than the safety
     * requirement and would reject a deliberately understated envelope
     * that is nonetheless in no danger — test/qemu's own fixtures encode
     * max_call_depth/total_depth as 0 on purpose (fixtures.cpp's
     * finishProgram), and every one of them pushes nothing here. */
    const uint32_t entrySpilled = declared > jitc::WINDOW_SIZE ? declared - jitc::WINDOW_SIZE : 0;
    if(entrySpilled > hdr.totalDepth)
    {
        return ProgramResult{ RESOURCE_PROGRAM_ENTRY_DEPTH, LANDING_RESOURCE_ERROR };
    }

    EntryArgs entryArgs;
    buildEntryArgs(&entryArgs, args, declared);

    uint64_t packed = enterDispatch(&entryArgs, runtime);
    return ProgramResult{ (uint32_t)packed, (uint32_t)(packed >> 32) };
}

/* The one piece both enterProgram* variants below share verbatim: size and
 * align the one-flexible-array-member Runtime storage by hand (a VLA, so
 * this has to be its own stack frame the storage survives inside of —
 * inlining it into each of the two callers by hand, as before, is what
 * made them look identical apart from their own arena/stack-limit
 * arguments), then hand off to enterProgramCore. Each caller still parses
 * the header and does its own stack-budget check and arena-placement
 * decision first, since those are the one thing that genuinely differs
 * between the two. */
static ProgramResult enterProgramWithHeader(
    const uint32_t *args, uint32_t argCount,
    const ExtHooks *extension,
    const uint8_t *programBytes, uint32_t programSize, const ProgramHeader &hdr,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t arenaOverlapsStack)
{
    /* A program with no procedures at all has nothing enterDispatch could
     * ever run — Runtime::slot(idx) unconditionally returns slots[idx+1]
     * (index 0 reserved as sentinel), so entering at procedure 0 would
     * read one slot past what storageBytesFor(0) allocates. Rejected here,
     * before any of that storage is even sized. */
    if(hdr.procCount == 0)
    {
        return ProgramResult{ RESOURCE_PROGRAM_NO_PROCS, LANDING_RESOURCE_ERROR };
    }

    /* One flexible-array-member object, over-allocated to fit
     * hdr.procCount+1 slots (index 0 = sentinel) — sized and aligned by
     * hand since a plain `Runtime runtime;` local would only reserve the
     * fixed header. */
    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(hdr.procCount)];
    return enterProgramCore(args, argCount, extension, reinterpret_cast<Runtime *>(runtimeStorage),
        programBytes, programSize, hdr,
        codeArenaBase, codeArenaSize, stackLimit, arenaOverlapsStack);
}

/* How many bytes of C stack the whole excursion needs, worst case, below
 * wherever sp sits right now: Runtime itself, the operand stack's own
 * worst-case depth, the worst-case live call/return record depth, this
 * implementation's own fixed overhead (dispatch_abi.h), and the caller's
 * own interrupt-frame allowance.
 *
 * operandStackBytes and maxCallDepth come from the program's own wire
 * envelope (ProgramHeader, above) rather than a caller-supplied parameter
 * — there's no way to introspect an arbitrary machine-code blob's own
 * worst-case stack behavior at runtime, so these are static, whole-program
 * properties validateProgram computed once, before the program was ever
 * serialized (maxCallDepth from its own call-graph DFS, operandStackBytes
 * = totalDepth * 4 — the whole tight TOS-depth bound, not a
 * window-credited fraction of it, since the window's actual absorption
 * depends on call-boundary argument shuffling that abstract depth alone
 * doesn't capture).
 *
 * What is and isn't in here follows from *when* stackHasRoom reads sp.
 * currentSp() runs inside whichever public entry point called it, with
 * that function's own frame already established — so its frame is below
 * the measured sp and already spent, and parseProgramHeader's has come and
 * gone. Everything taken afterwards has to fit in what this returns: the
 * VLA enterProgramWithHeader allocates (storageBytesFor, the first term),
 * enterProgramCore's frame (ENTER_PROGRAM_CORE_FRAME_BYTES), enterDispatch's
 * two pushes plus the entry procedure's own out-of-window arguments
 * (ENTER_DISPATCH_FIXED_BYTES and operandStackBytes respectively), and the
 * translator's worst case if a slot turns out to be cold. */
static uint32_t requiredStackBytes(
    uint32_t procCount, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t interruptReserve, const ExtHooks *extension)
{
    /* An extension's helpers run at the deepest point of an excursion, and
     * v1 rejects call-shaped ops, so none of them recurses back into
     * bytecode — one frame, added once, exactly like interruptReserve. */
    uint32_t extHelperBytes = extension != nullptr ? extension->helperStackBytes : 0;

    return Runtime::storageBytesFor(procCount)
         + operandStackBytes
         + maxCallDepth * CALL_RECORD_BYTES
         + ENTER_DISPATCH_FIXED_BYTES
         + ENTER_PROGRAM_CORE_FRAME_BYTES
         + TRANSLATOR_ENTRY_WORST_CASE_BYTES
         + interruptReserve
         + extHelperBytes;
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
 * range, the arena sits is different. On failure, reports
 * RESOURCE_EXHAUSTED_STACK_BUDGET directly — enterDispatch/Runtime were
 * never set up, so there's nothing else to unwind. */
extern "C" ProgramResult enterProgramOnStack(
    const uint32_t *args, uint32_t argCount,
    const uint8_t *programBytes, uint32_t programSize,
    const ExtHooks *extension,
    uint32_t codeArenaSize, uint32_t stackLimit, uint32_t interruptReserve)
{
    ProgramHeader hdr = parseProgramHeader(programBytes, programSize);
    uint32_t operandStackBytes = hdr.totalDepth * 4;

    uint32_t needed = requiredStackBytes(hdr.procCount, operandStackBytes, hdr.maxCallDepth, interruptReserve, extension)
                     + codeArenaSize;
    if(!stackHasRoom(needed, stackLimit))
    {
        return ProgramResult{ RESOURCE_EXHAUSTED_STACK_BUDGET, LANDING_RESOURCE_ERROR };
    }

    return enterProgramWithHeader(args, argCount, extension, programBytes, programSize, hdr,
        stackLimit, codeArenaSize, stackLimit, /*arenaOverlapsStack=*/1);
}

/* Variant: the compiled-code arena lives in caller-supplied memory — a
 * distinct SRAM bank, CCM, whatever a given target's own bus layout wants
 * — while Runtime, its dispatch table, and the operand stack still live on
 * the current C stack. codeArenaSize deliberately isn't part of the stack
 * check below — that memory isn't on this stack at all, so it's the
 * caller's own responsibility to have sized it correctly. */
extern "C" ProgramResult enterProgramSplit(
    const uint32_t *args, uint32_t argCount,
    const uint8_t *programBytes, uint32_t programSize,
    const ExtHooks *extension,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t interruptReserve)
{
    ProgramHeader hdr = parseProgramHeader(programBytes, programSize);
    uint32_t operandStackBytes = hdr.totalDepth * 4;

    uint32_t needed = requiredStackBytes(hdr.procCount, operandStackBytes, hdr.maxCallDepth, interruptReserve, extension);
    if(!stackHasRoom(needed, stackLimit))
    {
        return ProgramResult{ RESOURCE_EXHAUSTED_STACK_BUDGET, LANDING_RESOURCE_ERROR };
    }

    return enterProgramWithHeader(args, argCount, extension, programBytes, programSize, hdr,
        codeArenaBase, codeArenaSize, stackLimit, /*arenaOverlapsStack=*/0);
}
