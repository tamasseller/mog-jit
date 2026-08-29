#include <stdint.h>
#include <stddef.h>
#include <cassert>
#include "runtime_internal.h"
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
    assert(pos < size); // GCOV_EXCL_LINE — malformed/truncated program, matching decode_instr.cpp's own convention
    uint32_t maxCallDepth = jitc::decodeLeb128(bytes, pos, pos);
    assert(pos < size); // GCOV_EXCL_LINE
    uint32_t totalDepth = jitc::decodeLeb128(bytes, pos, pos);
    assert(pos < size); // GCOV_EXCL_LINE
    uint32_t procCount = jitc::decodeLeb128(bytes, pos, pos);
    return ProgramHeader{maxCallDepth, totalDepth, procCount, pos};
}

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

    uint64_t packed = enterDispatch(&entryArgs, runtime);
    return ProgramResult{ (uint32_t)packed, (uint32_t)(packed >> 32) };
}

static ProgramResult enterProgramWithHeader(
    const uint32_t *args, uint32_t argCount,
    const ExtHooks *extension,
    const uint8_t *programBytes, uint32_t programSize, const ProgramHeader &hdr,
    uint32_t codeArenaBase, uint32_t codeArenaSize,
    uint32_t stackLimit, uint32_t arenaOverlapsStack)
{
    if(hdr.procCount == 0)
    {
        return ProgramResult{ RESOURCE_PROGRAM_NO_PROCS, LANDING_RESOURCE_ERROR };
    }

    alignas(Runtime) unsigned char runtimeStorage[Runtime::storageBytesFor(hdr.procCount)];
    return enterProgramCore(args, argCount, extension, reinterpret_cast<Runtime *>(runtimeStorage),
        programBytes, programSize, hdr,
        codeArenaBase, codeArenaSize, stackLimit, arenaOverlapsStack);
}

static uint32_t requiredStackBytes(
    uint32_t procCount, uint32_t operandStackBytes, uint32_t maxCallDepth,
    uint32_t interruptReserve, const ExtHooks *extension)
{
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

static bool stackHasRoom(uint32_t needed, uint32_t stackLimit)
{
    uint32_t sp = currentSp();
    if(sp < needed)
    {
        return false; /* would wrap computing sp - needed */
    }
    return (sp - needed) >= stackLimit;
}

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
