#include <stdint.h>
#include <stddef.h>
#include <cassert>
#include "runtime.h"
#include "ext.h"
#include "dispatch_abi.h"
#include "entry_args.h"

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

    assert(pos < size); // GCOV_EXCL_LINE malformed/truncated program
    uint32_t maxCallDepth = jitc::decodeLeb128(bytes, pos, pos);

    assert(pos < size); // GCOV_EXCL_LINE malformed/truncated program
    uint32_t totalDepth = jitc::decodeLeb128(bytes, pos, pos);

    assert(pos < size); // GCOV_EXCL_LINE malformed/truncated program
    uint32_t procCount = jitc::decodeLeb128(bytes, pos, pos);
    
    return ProgramHeader{maxCallDepth, totalDepth, procCount, pos};
}

static uint32_t requiredStackBytes(
    uint32_t procCount, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t interruptReserve)
{
    uint32_t extHelperBytes = extHelperStackBytes();

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

static bool stackHasRoom(uint32_t needed, uint32_t stackLimit)
{
    uint32_t sp = currentSp();
    if(sp < needed)
    {
        return false; /* would wrap computing sp - needed */
    }
    return (sp - needed) >= stackLimit;
}

static ProgramResult enterProgramCore(
    uint32_t *args, uint32_t argCount,
    const uint8_t *programBytes, uint32_t programSize,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t arenaOverlapsStack, uint32_t interruptReserve)
{
    ProgramHeader hdr = parseProgramHeader(programBytes, programSize);
    uint32_t operandStackBytes = hdr.totalDepth * 4;

    uint32_t needed = requiredStackBytes(hdr.procCount, operandStackBytes, hdr.maxCallDepth, interruptReserve) + codeArenaSize;
    if(!stackHasRoom(needed, stackLimit))
    {
        return ProgramResult{ RESOURCE_EXHAUSTED_STACK_BUDGET, LANDING_RESOURCE_ERROR };
    }

    if(hdr.procCount == 0)
    {
        return ProgramResult{ RESOURCE_PROGRAM_NO_PROCS, LANDING_RESOURCE_ERROR };
    }

    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(hdr.procCount)];
    auto runtime = new(runtimeStorage) Runtime(hdr.procCount, codeArenaBase, codeArenaSize, stackLimit, arenaOverlapsStack);

    if(uint32_t code = runtime->loadProgram(programBytes, programSize, hdr.bodyOffset))
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

    uint64_t packed = enterDispatch(&runtime->slot(0), runtime, &entryArgs);
    return ProgramResult{ (uint32_t)packed, (uint32_t)(packed >> 32) };
}

extern "C" ProgramResult enterProgramOnStack(
    uint32_t *args, uint32_t argCount,
    const uint8_t *programBytes, uint32_t programSize,
    uint32_t codeArenaSize, uint32_t stackLimit, uint32_t interruptReserve)
{
    return enterProgramCore(args, argCount, programBytes, programSize, 
        stackLimit, codeArenaSize, stackLimit, /*arenaOverlapsStack=*/1, interruptReserve);
}

extern "C" ProgramResult enterProgramSplit(
    uint32_t *args, uint32_t argCount,
    const uint8_t *programBytes, uint32_t programSize,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t interruptReserve)
{
    return enterProgramCore(args, argCount, programBytes, programSize, 
        codeArenaBase, codeArenaSize, stackLimit, /*arenaOverlapsStack=*/0, interruptReserve);
}
