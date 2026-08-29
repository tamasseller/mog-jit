/* jit-armv6m/runtime — layer 1: enterProgramOnStack/enterProgramSplit.
 * Neither owns arena storage; it is always the caller's C stack or a
 * region the caller names, so nothing here has a fixed capacity to
 * outgrow. The fixed-cost constants requiredStackBytes sums live in
 * dispatch_abi.h. */

#include <stdint.h>
#include <stddef.h>
#include <cassert>
#include "runtime_internal.h"
#include "ext.h"
#include "dispatch_abi.h"
#include "entry_args.h"

/* The jit-armv6m wire envelope (encodeJitProgram, isa-core.md §5.5/§11.4).
 * The two whole-program stats have to be known before a single instruction
 * can be compiled, and validateProgram computed them before serializing.
 * bodyOffset is procedure 0's arg_count LEB128. */
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

/* Layout-agnostic core: every region arrives by address. `runtime` must
 * already point at Runtime::storageBytesFor(hdr.procCount) bytes.
 *
 * The two entry-argument checks can only run after Runtime::init's walk —
 * both compare against the arg_count it just recorded in slot 0. */
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

    /* No clamping, no zero-filling: the frame reclaimed on the way out is
     * sized from the declared count, so a disagreement has no safe default.
     * Reported, not asserted — a caller mistake, not malformed bytes. */
    if(argCount != declared)
    {
        return ProgramResult{ RESOURCE_PROGRAM_ENTRY_ARG_COUNT, LANDING_RESOURCE_ERROR };
    }

    /* operandStackBytes covers the entry procedure's out-of-window
     * arguments only because validateProgram seeds each procedure's peak at
     * its argCount. That is a producer property and totalDepth is wire
     * data, so check it: arg_count is 11 bits, and unchecked a forged
     * envelope could drive sp ~8KB below the reservation.
     *
     * Bounds the pushed words, not argCount — the stronger `declared >
     * totalDepth` would reject a deliberately understated envelope that is
     * in no danger, which test/qemu's fixtures rely on. */
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

/* Sizes and aligns the flexible-array-member Runtime storage. Must be its
 * own frame: the storage is a VLA that has to survive inside it. Each
 * caller does its own header parse, stack-budget check and arena placement
 * first — the one thing that genuinely differs between them. */
static ProgramResult enterProgramWithHeader(
    const uint32_t *args, uint32_t argCount,
    const ExtHooks *extension,
    const uint8_t *programBytes, uint32_t programSize, const ProgramHeader &hdr,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t arenaOverlapsStack)
{
    /* slot(idx) returns slots[idx+1], so entering procedure 0 of an empty
     * program reads one slot past what storageBytesFor(0) allocates. */
    if(hdr.procCount == 0)
    {
        return ProgramResult{ RESOURCE_PROGRAM_NO_PROCS, LANDING_RESOURCE_ERROR };
    }

    /* Over-allocated for hdr.procCount+1 slots (index 0 = sentinel); a
     * plain `Runtime runtime;` would only reserve the fixed header. */
    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(hdr.procCount)];
    return enterProgramCore(args, argCount, extension, reinterpret_cast<Runtime *>(runtimeStorage),
        programBytes, programSize, hdr,
        codeArenaBase, codeArenaSize, stackLimit, arenaOverlapsStack);
}

/* Worst-case C stack the whole excursion needs below the current sp.
 *
 * operandStackBytes is totalDepth * 4 — the whole TOS-depth bound, not a
 * window-credited fraction, since the window's real absorption depends on
 * call-boundary shuffling abstract depth doesn't capture.
 *
 * The terms follow from *when* stackHasRoom reads sp: the public entry
 * point's own frame is already below the measured sp and spent, so this
 * covers only what is taken afterwards — the VLA, enterProgramCore's
 * frame, enterDispatch's pushes plus the entry arguments, and the
 * translator's worst case if a slot is cold. */
static uint32_t requiredStackBytes(
    uint32_t procCount, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t interruptReserve, const ExtHooks *extension)
{
    /* v1 rejects call-shaped ops, so no helper recurses back into
     * bytecode — one frame, added once, like interruptReserve. */
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

/* sp is read from the hardware register rather than threaded in: the point
 * is checking the actual pointer before committing to a VLA. */
static bool stackHasRoom(uint32_t needed, uint32_t stackLimit)
{
    uint32_t sp = currentSp();
    if(sp < needed)
    {
        return false; /* would wrap computing sp - needed */
    }
    return (sp - needed) >= stackLimit;
}

/* The C stack is the whole work area. The arena anchors at stackLimit and
 * grows up; Runtime and the operand stack grow down from sp, converging
 * from opposite ends of one checked range — which is what lets the
 * translator encroach into the arena's unused reservation. codeArenaSize
 * still counts against stackLimit; only its placement differs. */
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

/* The arena is caller-supplied memory; Runtime and the operand stack still
 * live on the C stack. codeArenaSize is not part of the stack check — that
 * memory isn't on this stack, so sizing it is the caller's job. */
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
