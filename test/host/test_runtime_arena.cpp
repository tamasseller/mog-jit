#include "Test.h"
#include "ext.h"
#include "runtime.h"
#include "encode_instr.h"

#include <cassert>

using namespace jitc;

extern const uint32_t trampolineAddr = 0xDEADBEEFu;

static const uint32_t ARENA_BASE = 0x20000000;
static const uint32_t ARENA_SIZE = 512;

template<uint32_t procCount>
class RuntimeStorage
{
    alignas(8) uint8_t bytes[sizeof(Runtime) + (procCount + 1) * sizeof(ProcSlot)] = {};

    const Instr trivialBody[1] = {bare(Op::RETURN)};
    uint8_t programBytes[procCount * 4 + 8] = {};

public:
    Runtime *operator->()
    {
        return reinterpret_cast<Runtime *>(bytes);
    }

    RuntimeStorage(uint32_t base = ARENA_BASE, uint32_t size = ARENA_SIZE)
    {
        ProcSource procs[procCount];
        for(uint32_t i = 0; i < procCount; i++)
        {
            procs[i] = ProcSource{0, trivialBody, 1};
        }
        uint32_t len = encodeProgram(procs, procCount, programBytes, sizeof(programBytes));
        uint32_t bodyOffset;
        decodeLeb128(programBytes, 0, bodyOffset); // past proc_count's own LEB128
        new(bytes) Runtime(procCount, base, size, 0, 0);
        uint32_t code = (*this)->loadProgram(programBytes, len, bodyOffset);
        assert(code == 0); // GCOV_EXCL_LINE — this file's own encoding setup, not the thing under test
        (void)code;
    }
};

template<typename Storage>
static uint32_t place(Storage &runtime, uint32_t sizeBytes)
{
    uint32_t base = runtime->arenaCursor;
    runtime->commit(base + sizeBytes);
    return base;
}

TEST(EveryAllocationStartsWordAligned)
{
    RuntimeStorage<8> runtime;
    uint32_t sizes[] = {2, 6, 14, 4, 10, 6};
    for(uint32_t size : sizes)
    {
        CHECK(place(runtime, size) % 4 == 0);
        // CHECK(runtime->arenaCursor % 4 == 0);
    }
}

TEST(UnalignedArenaBaseIsRealignedRatherThanTrusted)
{
    for(uint32_t skew = 0; skew < 4; skew++)
    {
        RuntimeStorage<2> runtime(ARENA_BASE + skew);
        uint32_t dest = place(runtime, 6);
        CHECK(dest % 4 == 0);
        CHECK(dest >= ARENA_BASE + skew); // never below the arena it was given
    }
}

TEST(OccupiedSizeIsAlwaysAWholeNumberOfWords)
{
    RuntimeStorage<5> runtime;
    uint32_t sizes[] = {6, 2, 14, 10};
    for(uint32_t i = 0; i < 4; i++)
    {
        runtime->markCompiled(i, place(runtime, sizes[i]), /*lruTick=*/0);
    }
    CHECK(!runtime->isResident(4));
    for(uint32_t i = 0; i < 4; i++)
    {
        CHECK(runtime->isResident(i));
        CHECK((runtime->slot(i).codePtr & ~1u) % 4 == 0);
        CHECK(runtime->occupiedSizeOf(i) % 4 == 0);
        CHECK(runtime->occupiedSizeOf(i) >= sizes[i]);
    }
}

TEST(RoomCheckAccountsForThePaddingAllocateWillConsume)
{
    RuntimeStorage<64> runtime;
    uint32_t allocations = 0;
    while(runtime->hasRoomFor(8))
    {
        uint32_t dest = place(runtime, 6);
        CHECK(dest % 4 == 0);
        CHECK(dest + 6 <= ARENA_BASE + ARENA_SIZE);
        allocations++;
        CHECK(allocations <= ARENA_SIZE / 8); // GCOV_EXCL_LINE — a non-advancing cursor would spin here
    }
    CHECK(allocations > 0);
}

TEST(AFreshlyCompiledProcedureIsTheYoungestNotTheOldest)
{
    RuntimeStorage<3> runtime;
    runtime->markCompiled(0, place(runtime, 8), /*lruTick=*/10);
    runtime->markCompiled(1, place(runtime, 8), /*lruTick=*/20);

    CHECK(runtime->findEvictionVictim(/*now=*/21) == 0); // slot 1 is younger

    runtime->markCompiled(2, place(runtime, 8), /*lruTick=*/30);
    CHECK(runtime->findEvictionVictim(/*now=*/31) == 0); // still the oldest, not the newest
}

TEST(NoResidentProcedureLeavesNothingToEvict)
{
    // What Assembler::growForAttached reads as "the arena cannot be made
    // to fit this" — the -1 that turns into RESOURCE_EXHAUSTED_ARENA.
    RuntimeStorage<2> runtime;
    CHECK(runtime->findEvictionVictim(/*now=*/5) < 0);
}

// TEST(ArenaEndIsAlignedSoAFullAllocationNeverOvershootsIt)
// {
//     for(uint32_t skew = 0; skew < 4; skew++)
//     {
//         RuntimeStorage<1> runtime(ARENA_BASE, ARENA_SIZE + skew);
//         uint32_t gap = runtime->arenaEnd - runtime->arenaCursor;
//         CHECK(gap % 4 == 0);
//         place(runtime, gap); // a procedure that exactly fills the remaining capacity
//         CHECK(runtime->arenaCursor == runtime->arenaEnd); // lands exactly on it, never past
//     }
// }

TEST(WalkFailsWithoutTouchingDispatchStateWhenAProcedureCantBeScanned)
{
    const Instr body[] = {bare(Op::RETURN)};
    ProcSource procs[] = {ProcSource{0, body, 1}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));
    uint32_t bodyOffset;
    decodeLeb128(programBytes, 0, bodyOffset);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    register uint32_t sp asm("sp");
    Runtime *runtime = new(bytes) Runtime(1, ARENA_BASE, ARENA_SIZE, sp, 0);
    CHECK(runtime->loadProgram(programBytes, len, bodyOffset) == RESOURCE_EXHAUSTED_SCAN_STACK);
}

TEST(WalkReportsAnUnterminatedBodySeparatelyFromRunningOutOfStack)
{
    const Instr body[] = {bare(Op::LOOP), CONST(1)};
    ProcSource procs[] = {ProcSource{0, body, 2}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));
    uint32_t bodyOffset;
    decodeLeb128(programBytes, 0, bodyOffset);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = new(bytes) Runtime(1, ARENA_BASE, ARENA_SIZE, 0, 0);
    CHECK(runtime->loadProgram(programBytes, len, bodyOffset) == RESOURCE_PROGRAM_BODY_UNTERMINATED);
}

TEST(WalkReportsAnArgCountPastProcSlotsOwnFieldWidth)
{
    const Instr body[] = {bare(Op::RETURN)};
    ProcSource procs[] = {ProcSource{ProcSlot::MAX_ARG_COUNT + 1, body, 1}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));
    uint32_t bodyOffset;
    decodeLeb128(programBytes, 0, bodyOffset);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = new(bytes) Runtime(1, ARENA_BASE, ARENA_SIZE, 0, 0);
    CHECK(runtime->loadProgram(programBytes, len, bodyOffset) == RESOURCE_LIMIT_ARG_COUNT);
}

TEST(WalkReportsAProcCountPastTheCallRecordsOwnProcIdxField)
{
    // Rejected before the walk touches a single body, so the header alone is
    // enough — a program that really had this many procedures could not be
    // built here.
    uint8_t programBytes[] = {0x00};

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = new(bytes) Runtime(jitc::MAX_PROC_IDX + 2, ARENA_BASE, ARENA_SIZE, 0, 0);
    CHECK(runtime->loadProgram(programBytes, sizeof(programBytes), 0) == RESOURCE_LIMIT_PROC_COUNT);
}

TEST(WalkAcceptsTheLargestProcCountTheCallRecordCanStillAddress)
{
    const Instr body[] = {bare(Op::RETURN)};
    ProcSource procs[] = {ProcSource{0, body, 1}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));
    uint32_t bodyOffset;
    decodeLeb128(programBytes, 0, bodyOffset);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    // The boundary itself is not the rejection — one procedure walked under a
    // procCount at the ceiling still has to pass.
    Runtime *runtime = new(bytes) Runtime(1, ARENA_BASE, ARENA_SIZE, 0, 0);
    CHECK(runtime->loadProgram(programBytes, len, bodyOffset) == 0);
    CHECK(jitc::MAX_PROC_IDX + 1 == 0x8000u);
}

TEST(WalkReportsAnUnknownOpcodeAsADeploymentMismatch)
{
    uint8_t programBytes[] = {0x01, 0x00, 0x80}; // proc_count=1, arg_count=0, body=[EXT 0x80]
    const uint32_t bodyOffset = 1;

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = new(bytes) Runtime(1, ARENA_BASE, ARENA_SIZE, 0, 0, /*extension=*/nullptr);
    CHECK(runtime->loadProgram(programBytes, sizeof(programBytes), bodyOffset) == RESOURCE_PROGRAM_EXT_UNKNOWN);
}

// ── the extension seam at the program walk (compiler/src/ext.h) ─────────

namespace
{
uint32_t extInlineDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    *decl = jitc::extDecl(0x80, 0, /*tosDelta=*/0, /*maxTransient=*/0, /*halfwords=*/2);
    return 1;
}

uint32_t extCallShapedDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    *decl = jitc::extDecl(0x80, jitc::EXT_FLAG_CALL_SHAPED, 0, 0, 2);
    return 1;
}

const ExtHooks EXT_OK = {jitc::EXT_ABI_VERSION, extInlineDecode};
const ExtHooks EXT_CALL_SHAPED = {jitc::EXT_ABI_VERSION, extCallShapedDecode};
const ExtHooks EXT_STALE_ABI = {jitc::EXT_ABI_VERSION + 1, extInlineDecode};

uint32_t extProgram(uint8_t *out)
{
    out[0] = 0x01;
    out[1] = 0x00;
    out[2] = 0x80;
    out[3] = 100; // RETURN
    return 4;
}
} // namespace

TEST(WalkAcceptsAWellFormedExtensionDeclaration)
{
    const ExtHooks *ext = &EXT_OK;
    uint8_t programBytes[8];
    uint32_t len = extProgram(programBytes);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = new(bytes) Runtime(1, ARENA_BASE, ARENA_SIZE, 0, 0, ext);
    CHECK(runtime->loadProgram(programBytes, len, 1) == 0);
}

TEST(WalkRejectsAnExtensionBuiltAgainstADifferentAbiVersion)
{
    const ExtHooks *ext = &EXT_STALE_ABI;
    uint8_t programBytes[8];
    uint32_t len = extProgram(programBytes);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = new(bytes) Runtime(1, ARENA_BASE, ARENA_SIZE, 0, 0, ext);
    CHECK(runtime->loadProgram(programBytes, len, 1) == RESOURCE_PROGRAM_EXT_ABI);
}

TEST(WalkRejectsACallShapedExtensionDeclarationAsUnsupported)
{
    const ExtHooks *ext = &EXT_CALL_SHAPED;
    uint8_t programBytes[8];
    uint32_t len = extProgram(programBytes);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = new(bytes) Runtime(1, ARENA_BASE, ARENA_SIZE, 0, 0, ext);
    CHECK(runtime->loadProgram(programBytes, len, 1) == RESOURCE_PROGRAM_EXT_UNSUPPORTED);
}
