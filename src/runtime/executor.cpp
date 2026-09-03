#include "executor.h"

#include "decode_instr.h"
#include <stdint.h>
#include <stddef.h>
#include <cassert>
#include "runtime.h"
#include "ext.h"
#include "dispatch_abi.h"
#include "entry_args.h"
#include "program_frame.h"

struct ProgramHeader
{
    uint32_t maxCallDepth;
    uint32_t totalDepth;
    uint32_t procCount;
};

/* Leaves `r` on the first procedure's arg_count, where loadProgram picks up. */
static ProgramHeader parseProgramHeader(BcReader &r)
{
    ProgramHeader hdr{};

    assert(!r.atEnd()); // GCOV_EXCL_LINE malformed/truncated program
    jitc::decodeLeb128(r, hdr.maxCallDepth);

    assert(!r.atEnd()); // GCOV_EXCL_LINE malformed/truncated program
    jitc::decodeLeb128(r, hdr.totalDepth);

    assert(!r.atEnd()); // GCOV_EXCL_LINE malformed/truncated program
    jitc::decodeLeb128(r, hdr.procCount);

    return hdr;
}

static uint32_t requiredStackBytes(
    uint32_t procCount, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t interruptReserve)
{
    /* 
     * The thunk's own frame is the runtime's, not the extension's: an
     * extension can only know what its C helper costs. 
     */
    const uint32_t declared = extHelperStackBytes();
    const uint32_t extHelperBytes = declared ? declared + EXT_THUNK_STACK_BYTES : 0;

    /* 
     * A translation and an extension helper are the two things that sit on top
     * of the deepest the compiled code itself reaches, and they never coexist 
     */
    const uint32_t deepestExcursion = extHelperBytes > TRANSLATOR_ENTRY_WORST_CASE_BYTES
        ? extHelperBytes : TRANSLATOR_ENTRY_WORST_CASE_BYTES;

    return Runtime::storageBytesFor(procCount)
         + operandStackBytes
         + maxCallDepth * CALL_RECORD_BYTES
         + ENTER_DISPATCH_FIXED_BYTES
         + EXECUTOR_RUN_FRAME_BYTES
         + deepestExcursion
         + interruptReserve;
}

static uint32_t currentSp()
{
    register uint32_t sp asm("sp");
    return sp;
}

/* The hard ceiling for compiled code: SP at entry less everything this
 * excursion has statically reserved. Zero if that does not even reach
 * stackLimit, which is the up-front rejection. */
static uint32_t codeLimitFor(uint32_t needed, uint32_t stackLimit)
{
    uint32_t sp = currentSp();
    
    if(sp < needed)
    {
        return 0; /* would wrap computing sp - needed */
    }

    return (sp - needed) >= stackLimit ? sp - needed : 0;
}

ProgramResult Executor::run(BcHandle program, uint32_t programSize, uint32_t *args, uint32_t argCount)
{
    BcReader wire;

    wire.open(program, programSize);
    if(!programFrameOk(wire, programSize))
    {
        return ProgramResult{ RESOURCE_PROGRAM_FRAME, LANDING_RESOURCE_ERROR };
    }

    const uint32_t payloadSize = programSize - PROGRAM_FRAME_BYTES;

    wire.open(program, payloadSize);
    ProgramHeader hdr = parseProgramHeader(wire);
    uint32_t operandStackBytes = hdr.totalDepth * 4;

    const uint32_t codeLimit = codeLimitFor(
        requiredStackBytes(hdr.procCount, operandStackBytes, hdr.maxCallDepth, arena.getInterruptReserve()),
        arena.getStackLimit());

    if(codeLimit == 0)
    {
        return ProgramResult{ RESOURCE_EXHAUSTED_STACK_BUDGET, LANDING_RESOURCE_ERROR };
    }

    if(hdr.procCount == 0)
    {
        return ProgramResult{ RESOURCE_PROGRAM_NO_PROCS, LANDING_RESOURCE_ERROR };
    }

    CodeArena::Excursion excursion(arena, codeLimit);

    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(hdr.procCount)];
    auto runtime = new(runtimeStorage) Runtime(hdr.procCount, arena);

    if(uint32_t code = runtime->loadProgram(wire))
    {
        return ProgramResult{ code, LANDING_RESOURCE_ERROR };
    }

    const uint32_t declared = runtime->slot(0).argCount();

    if(argCount != declared)
    {
        return ProgramResult{ RESOURCE_PROGRAM_ENTRY_ARG_COUNT, LANDING_RESOURCE_ERROR };
    }

    const uint32_t entrySpilled = declared > jitc::WINDOW_SIZE ? declared - jitc::WINDOW_SIZE : 0;
    if(entrySpilled > hdr.totalDepth)
    {
        return ProgramResult{ RESOURCE_PROGRAM_ENTRY_DEPTH, LANDING_RESOURCE_ERROR };
    }

    EntryArgs entryArgs;
    buildEntryArgs(&entryArgs, args, declared);

    live = runtime;
    uint64_t packed = enterDispatch(&runtime->slot(0), runtime, &entryArgs);
    live = nullptr;

    return ProgramResult{ (uint32_t)packed, (uint32_t)(packed >> 32) };
}

/* ARMv6-M/ARMv7-M exception frame, in words. An FP frame puts its own
 * registers above these, so the offsets hold there too. */
enum { FRAME_R0 = 0, FRAME_R1, FRAME_R2, FRAME_R3, FRAME_R12, FRAME_LR, FRAME_PC, FRAME_XPSR };

/* IT[1:0] at 26:25 and IT[7:2] at 15:10 — the ARMv7-M IT/ICI state of the
 * instruction this frame will never return to, which exception return would
 * otherwise apply to the trampoline's own two instructions. Reserved and zero
 * on ARMv6-M, so the mask costs nothing there. Deliberately clear of bit 24
 * (T), bit 9 (the stack-alignment adjustment exception return undoes) and
 * bits 8:0. */
static constexpr uint32_t XPSR_IT_MASK = 0x0600fc00u;

bool Executor::cancel(uint32_t exceptionFrame, uint32_t code)
{
    Runtime *runtime = live;

    if(!runtime)
    {
        return false;
    }

    /* enterDispatch writes the landing before the sp, and .Lresume clears the
     * sp again on the way out, so this one word answers both halves of "is
     * there an excursion to land on". */
    const uint32_t landingSp = runtime->savedSp();

    if(!landingSp)
    {
        return false;
    }

    uint32_t *frame = (uint32_t *)(uintptr_t)exceptionFrame;

    frame[FRAME_R0] = code;
    frame[FRAME_R1] = landingSp;
    frame[FRAME_R2] = LANDING_CANCELLED;
    frame[FRAME_R3] = runtime->sentinelLandingAddress();
    frame[FRAME_PC] = (uint32_t)(uintptr_t)asyncAbortTrampoline & ~1u;
    frame[FRAME_XPSR] &= ~XPSR_IT_MASK;

    return true;
}
