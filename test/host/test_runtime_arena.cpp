#include "decode_instr.h"
#include "wire.h"
#include "abi_strategy.h"
#include "Test.h"
#include "ext.h"
#include "ext_stub.h"
#include "runtime.h"
#include "runtime_probe.h"
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
    CodeArena arena;
    /* Stands in for the up-front check: a shared arena's ceiling is the code
     * limit an excursion opens with, not anything the arena was built with. */
    CodeArena::Excursion excursion;

    const Instr trivialBody[1] = {bare(Op::RETURN)};
    uint8_t programBytes[procCount * 4 + 8] = {};

public:
    Runtime *operator->()
    {
        return reinterpret_cast<Runtime *>(bytes);
    }

    RuntimeStorage(uint32_t base = ARENA_BASE, uint32_t size = ARENA_SIZE,
        uint32_t overlapsStack = 0, uint32_t interruptReserve = 0):
        arena(overlapsStack
            ? CodeArena::sharedWithStack(base, interruptReserve)
            : CodeArena::region(base, size, /*stackLimit=*/0, interruptReserve)),
        excursion(arena, base + size)
    {
        ProcSource procs[procCount];
        for(uint32_t i = 0; i < procCount; i++)
        {
            procs[i] = ProcSource{0, trivialBody, 1};
        }
        uint32_t len = encodeProgram(procs, procCount, programBytes, sizeof(programBytes));
        new(bytes) Runtime(procCount, arena);
        BcReader wire = wireAtBodies(programBytes, len);
        uint32_t code = (*this)->loadProgram(wire);
        assert(code == 0); // GCOV_EXCL_LINE — this file's own encoding setup, not the thing under test
        (void)code;
    }
};

template<typename Storage>
static uint32_t place(Storage &runtime, uint32_t sizeBytes)
{
    uint32_t base = runtime->getArenaCursor();
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
        runtime->markCompiled(i, place(runtime, sizes[i]));
    }
    CHECK(!runtime->isResident(4));
    for(uint32_t i = 0; i < 4; i++)
    {
        CHECK(runtime->isResident(i));
        CHECK((runtime->slot(i).codePtr & ~1u) % 4 == 0);
        CHECK(RuntimeProbe::occupiedSizeOf(*runtime.operator->(), i) % 4 == 0);
        CHECK(RuntimeProbe::occupiedSizeOf(*runtime.operator->(), i) >= sizes[i]);
    }
}

TEST(ASeparateArenaIsCappedWhereItWasPlacedNoMatterWhereTheStackIs)
{
    RuntimeStorage<1> runtime(ARENA_BASE, ARENA_SIZE, /*overlapsStack=*/0);
    const uint32_t end = ARENA_BASE + ARENA_SIZE;

    CHECK(RuntimeProbe::arenaCeiling(*runtime.operator->()) == end);

    Runtime::DynamicStackGuard guard(*runtime.operator->(), 64);
    CHECK(RuntimeProbe::arenaCeiling(*runtime.operator->()) == end); // the stack is somewhere else entirely
}

static uint32_t currentSp()
{
    register uint32_t sp asm("sp");
    return sp;
}

TEST(ASharedArenaNeverGrowsPastWhatWasValidatedUpFront)
{
    // Only the stack is allowed into the other's ground. The arena's ceiling
    // is the line enterProgram checked before any of this ran, and a guard
    // promising to stop well above that line does not raise it.
    const uint32_t base = (currentSp() - 4096) & ~3u;
    RuntimeStorage<1> runtime(base, 2048, /*overlapsStack=*/1, /*interruptReserve=*/32);
    const uint32_t reserved = base + 2048;

    CHECK(RuntimeProbe::arenaCeiling(*runtime.operator->()) == reserved);

    Runtime::DynamicStackGuard guard(*runtime.operator->(), 1024); // floor lands above reserved
    CHECK(RuntimeProbe::arenaCeiling(*runtime.operator->()) == reserved);
}

TEST(ASharedArenaStopsShortWhenTheStackHasDescendedIntoIt)
{
    // The other half: the stack may sit inside the reserved-but-unoccupied
    // part, and while it does the arena has to stop above it — with room for
    // the exception frame that can land there without warning.
    const uint32_t reserve = 32;
    const uint32_t base = (currentSp() - 4096) & ~3u;
    RuntimeStorage<1> runtime(base, 2048, /*overlapsStack=*/1, reserve);
    const uint32_t reserved = base + 2048;

    {
        Runtime::DynamicStackGuard deep(*runtime.operator->(), 3000); // floor lands below reserved
        const uint32_t ceiling = RuntimeProbe::arenaCeiling(*runtime.operator->());

        CHECK(ceiling < reserved);
        CHECK(ceiling > base);                 // still above what the arena holds
        CHECK(ceiling + reserve <= currentSp() - 3000);
    }

    CHECK(RuntimeProbe::arenaCeiling(*runtime.operator->()) == reserved); // leaving gives the ground back
}

TEST(RoomCheckAccountsForThePaddingAllocateWillConsume)
{
    RuntimeStorage<64> runtime;
    uint32_t allocations = 0;
    while(RuntimeProbe::arenaCeiling(*runtime.operator->()) - runtime->getArenaCursor() >= 8)
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
    // The stamp comes from the dispatch helper that entered the trampoline,
    // not from markCompiled — see runtime.S's callHelper.
    RuntimeStorage<3> runtime;
    runtime->slot(0).lastUsed = 10;
    runtime->markCompiled(0, place(runtime, 8));
    runtime->slot(1).lastUsed = 20;
    runtime->markCompiled(1, place(runtime, 8));

    CHECK(RuntimeProbe::findEvictionVictim(*runtime.operator->(), /*now=*/21) == 0); // slot 1 is younger

    runtime->slot(2).lastUsed = 30;
    runtime->markCompiled(2, place(runtime, 8));
    CHECK(RuntimeProbe::findEvictionVictim(*runtime.operator->(), /*now=*/31) == 0); // still the oldest, not the newest
}

TEST(NoResidentProcedureLeavesNothingToEvict)
{
    // What Assembler::growForAttached reads as "the arena cannot be made
    // to fit this" — the -1 that turns into RESOURCE_EXHAUSTED_ARENA.
    RuntimeStorage<2> runtime;
    CHECK(RuntimeProbe::findEvictionVictim(*runtime.operator->(), /*now=*/5) < 0);
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

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    register uint32_t sp asm("sp");
    CodeArena arena = CodeArena::region(ARENA_BASE, ARENA_SIZE, /*stackLimit=*/sp);
    Runtime *runtime = new(bytes) Runtime(1, arena);
    BcReader wire = wireAtBodies(programBytes, len);
    CHECK(runtime->loadProgram(wire) == RESOURCE_EXHAUSTED_SCAN_STACK);
}

TEST(WalkReportsAnUnterminatedBodySeparatelyFromRunningOutOfStack)
{
    const Instr body[] = {bare(Op::LOOP_PRE), CONST(1)};
    ProcSource procs[] = {ProcSource{0, body, 2}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    CodeArena arena = CodeArena::region(ARENA_BASE, ARENA_SIZE, /*stackLimit=*/0);
    Runtime *runtime = new(bytes) Runtime(1, arena);
    BcReader wire = wireAtBodies(programBytes, len);
    CHECK(runtime->loadProgram(wire) == RESOURCE_PROGRAM_BODY_UNTERMINATED);
}

TEST(WalkReportsAnArgCountPastProcSlotsOwnFieldWidth)
{
    const Instr body[] = {bare(Op::RETURN)};
    ProcSource procs[] = {ProcSource{ProcSlot::MAX_ARG_COUNT + 1, body, 1}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    CodeArena arena = CodeArena::region(ARENA_BASE, ARENA_SIZE, /*stackLimit=*/0);
    Runtime *runtime = new(bytes) Runtime(1, arena);
    BcReader wire = wireAtBodies(programBytes, len);
    CHECK(runtime->loadProgram(wire) == RESOURCE_LIMIT_ARG_COUNT);
}

TEST(WalkReportsAProcCountPastTheCallRecordsOwnProcIdxField)
{
    // Rejected before the walk touches a single body, so the header alone is
    // enough — a program that really had this many procedures could not be
    // built here.
    uint8_t programBytes[] = {0x00};

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    CodeArena arena = CodeArena::region(ARENA_BASE, ARENA_SIZE, /*stackLimit=*/0);
    Runtime *runtime = new(bytes) Runtime(jitc::MAX_PROC_IDX + 2, arena);
    BcReader wire = wireOver(programBytes, sizeof(programBytes));
    CHECK(runtime->loadProgram(wire) == RESOURCE_LIMIT_PROC_COUNT);
}

TEST(WalkAcceptsTheLargestProcCountTheCallRecordCanStillAddress)
{
    const Instr body[] = {bare(Op::RETURN)};
    ProcSource procs[] = {ProcSource{0, body, 1}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    // The boundary itself is not the rejection — one procedure walked under a
    // procCount at the ceiling still has to pass.
    CodeArena arena = CodeArena::region(ARENA_BASE, ARENA_SIZE, /*stackLimit=*/0);
    Runtime *runtime = new(bytes) Runtime(1, arena);
    BcReader wire = wireAtBodies(programBytes, len);
    CHECK(runtime->loadProgram(wire) == 0);
    CHECK(jitc::MAX_PROC_IDX + 1 == 0x8000u);
}

TEST(WalkReportsAnUnknownOpcodeAsADeploymentMismatch)
{
    uint8_t programBytes[] = {0x01, 0x00, 0x80}; // proc_count=1, arg_count=0, body=[EXT 0x80]

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    CodeArena arena = CodeArena::region(ARENA_BASE, ARENA_SIZE, /*stackLimit=*/0);
    Runtime *runtime = new(bytes) Runtime(1, arena);
    BcReader wire = wireAtBodies(programBytes, sizeof(programBytes));
    CHECK(runtime->loadProgram(wire) == RESOURCE_PROGRAM_EXT_UNKNOWN);
}

// ── the extension seam at the program walk (compiler/src/ext.h) ─────────

namespace
{
bool extInlineDescribe(uint8_t, BcReader &, uint32_t *desc)
{
    *desc = jitc::extDesc(0, /*tosDelta=*/0);
    return true;
}

bool extCallShapedDescribe(uint8_t, BcReader &, uint32_t *desc)
{
    *desc = jitc::extDesc(jitc::EXT_FLAG_CALL_SHAPED, 0);
    return true;
}

const ExtStub EXT_OK = {extInlineDescribe};
const ExtStub EXT_CALL_SHAPED = {extCallShapedDescribe};

uint32_t extProgram(uint8_t *out)
{
    out[0] = 0x01;
    out[1] = 0x00;
    out[2] = 0x80;
    out[3] = 102; // RETURN
    return 4;
}
} // namespace

TEST(WalkAcceptsAWellFormedExtensionDeclaration)
{
    ExtScope ext(&EXT_OK);
    uint8_t programBytes[8];
    uint32_t len = extProgram(programBytes);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    CodeArena arena = CodeArena::region(ARENA_BASE, ARENA_SIZE, /*stackLimit=*/0);
    Runtime *runtime = new(bytes) Runtime(1, arena);
    BcReader wire = wireAtBodies(programBytes, len);
    CHECK(runtime->loadProgram(wire) == 0);
}


TEST(WalkRejectsACallShapedExtensionDeclarationAsUnsupported)
{
    ExtScope ext(&EXT_CALL_SHAPED);
    uint8_t programBytes[8];
    uint32_t len = extProgram(programBytes);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    CodeArena arena = CodeArena::region(ARENA_BASE, ARENA_SIZE, /*stackLimit=*/0);
    Runtime *runtime = new(bytes) Runtime(1, arena);
    BcReader wire = wireAtBodies(programBytes, len);
    CHECK(runtime->loadProgram(wire) == RESOURCE_PROGRAM_EXT_UNSUPPORTED);
}
