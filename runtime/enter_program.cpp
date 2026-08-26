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
#include "dispatch_abi.h"

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
 * (a procedure's own scan overflowed, or its arg_count/body length doesn't
 * fit ProcSlot's packed fields) — reported as RESOURCE_ERROR without ever
 * reaching enterDispatch, the same as the stack-budget checks below
 * already do for a different reason. */
static ProgramResult enterProgramCore(
    uint32_t argIn,
    Runtime *runtime,
    const uint8_t *programBytes, uint32_t programSize, const ProgramHeader &hdr,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t arenaOverlapsStack)
{
    if(!runtime->init(programBytes, programSize, hdr.bodyOffset, hdr.procCount,
        codeArenaBase, codeArenaSize, stackLimit, arenaOverlapsStack))
    {
        return ProgramResult{ RESOURCE_ERROR_CODE, 1 };
    }

    uint64_t packed = enterDispatch(argIn, runtime);
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
    uint32_t argIn,
    const uint8_t *programBytes, uint32_t programSize, const ProgramHeader &hdr,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t arenaOverlapsStack)
{
    /* One flexible-array-member object, over-allocated to fit
     * hdr.procCount+1 slots (index 0 = sentinel) — sized and aligned by
     * hand since a plain `Runtime runtime;` local would only reserve the
     * fixed header. */
    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(hdr.procCount)];
    return enterProgramCore(argIn, reinterpret_cast<Runtime *>(runtimeStorage),
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
 * doesn't capture). */
static uint32_t requiredStackBytes(
    uint32_t procCount, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t interruptReserve)
{
    return Runtime::storageBytesFor(procCount)
         + operandStackBytes
         + maxCallDepth * CALL_RECORD_BYTES
         + procCount * CALLEE_ARG_COUNTS_BYTES_PER_PROC
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
    const uint8_t *programBytes, uint32_t programSize,
    uint32_t codeArenaSize, uint32_t stackLimit, uint32_t interruptReserve)
{
    ProgramHeader hdr = parseProgramHeader(programBytes, programSize);
    uint32_t operandStackBytes = hdr.totalDepth * 4;

    uint32_t needed = requiredStackBytes(hdr.procCount, operandStackBytes, hdr.maxCallDepth, interruptReserve)
                     + codeArenaSize;
    if(!stackHasRoom(needed, stackLimit))
    {
        return ProgramResult{ RESOURCE_ERROR_CODE, 1 };
    }

    return enterProgramWithHeader(argIn, programBytes, programSize, hdr,
        stackLimit, codeArenaSize, stackLimit, /*arenaOverlapsStack=*/1);
}

/* Variant: the compiled-code arena lives in caller-supplied memory — a
 * distinct SRAM bank, CCM, whatever a given target's own bus layout wants
 * — while Runtime, its dispatch table, and the operand stack still live on
 * the current C stack. codeArenaSize deliberately isn't part of the stack
 * check below — that memory isn't on this stack at all, so it's the
 * caller's own responsibility to have sized it correctly. */
extern "C" ProgramResult enterProgramSplit(
    uint32_t argIn,
    const uint8_t *programBytes, uint32_t programSize,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t interruptReserve)
{
    ProgramHeader hdr = parseProgramHeader(programBytes, programSize);
    uint32_t operandStackBytes = hdr.totalDepth * 4;

    uint32_t needed = requiredStackBytes(hdr.procCount, operandStackBytes, hdr.maxCallDepth, interruptReserve);
    if(!stackHasRoom(needed, stackLimit))
    {
        return ProgramResult{ RESOURCE_ERROR_CODE, 1 };
    }

    return enterProgramWithHeader(argIn, programBytes, programSize, hdr,
        codeArenaBase, codeArenaSize, stackLimit, /*arenaOverlapsStack=*/0);
}
