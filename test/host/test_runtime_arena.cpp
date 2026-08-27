// Runtime's arena bookkeeping, exercised on the host rather than only
// through the real QEMU image (runtime/compile_proc.cpp, via an attached
// Assembler, is its only other caller). What's worth testing cheaply here
// is the 4-byte
// alignment invariant every procedure's PC-relative literal loads depend
// on: an off-by-one in that padding would otherwise only surface as a
// wrong value loaded on real hardware, after a compaction slide.
//
// The arena base below is a plausible-looking fake address, never real
// host memory: Runtime addresses everything as uint32_t, so on a 64-bit
// host a real buffer's address wouldn't survive the cast. That rules out
// evict(), the one method that actually dereferences the arena (memmove) —
// its slide is covered by a QEMU fixture instead. Every method used here
// only ever touches the dispatch table, which does live in real memory.
#include "Test.h"
#include "runtime_internal.h"
#include "encode_instr.h"

#include <cassert>

using namespace jitc;

// Normally dispatch_abi.cpp's own address of translatorTrampoline; any
// distinct non-zero value serves as the not-resident marker here.
extern const uint32_t trampolineAddr = 0xDEADBEEFu;

static const uint32_t ARENA_BASE = 0x20000000;
static const uint32_t ARENA_SIZE = 512;

// Built by reinterpreting raw bytes exactly the way every real caller
// does — Runtime is a trivial aggregate with a flexible array member and
// is never constructed normally.
template<uint32_t procCount>
class RuntimeStorage
{
    alignas(8) uint8_t bytes[sizeof(Runtime) + (procCount + 1) * sizeof(ProcSlot)] = {};

    // init() now walks real wire bytes to build every slot's own static
    // half (ProcSlot) — nothing this file's own tests care about, but a
    // real, valid program has to sit somewhere for it to walk. procCount
    // trivial (argCount 0, bare RETURN) procedures, encoded once, kept
    // alive as long as the Runtime itself: ProcSlot.bodyPtr points
    // straight into this buffer.
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
        bool ok = (*this)->init(programBytes, len, bodyOffset, procCount, base, size, 0, 0);
        assert(ok); // GCOV_EXCL_LINE — this file's own encoding setup, not the thing under test
    }
};

TEST(ReserveForRoundsUpToAWholeWord)
{
    CHECK(Runtime::reserveFor(0) == 0);
    CHECK(Runtime::reserveFor(2) == 4);
    CHECK(Runtime::reserveFor(4) == 4);
    CHECK(Runtime::reserveFor(6) == 8);
    CHECK(Runtime::reserveFor(14) == 16);
}

TEST(EveryAllocationStartsWordAligned)
{
    // Odd-sized allocations back to back: without the reservation
    // rounding, the second would start 2 bytes off.
    RuntimeStorage<8> runtime;
    uint32_t sizes[] = {2, 6, 14, 4, 10, 6};
    for(uint32_t size : sizes)
    {
        CHECK(runtime->allocate(size) % 4 == 0);
    }
}

TEST(UnalignedArenaBaseIsRealignedRatherThanTrusted)
{
    // enterProgramOnStack anchors the arena at stackLimit and
    // enterProgramSplit takes the base straight from its caller, so an
    // unaligned base has to be handled here rather than assumed away.
    for(uint32_t skew = 0; skew < 4; skew++)
    {
        RuntimeStorage<2> runtime(ARENA_BASE + skew);
        uint32_t dest = runtime->allocate(6);
        CHECK(dest % 4 == 0);
        CHECK(dest >= ARENA_BASE + skew); // never below the arena it was given
    }
}

TEST(OccupiedSizeIsAlwaysAWholeNumberOfWords)
{
    // This is what makes eviction safe for already-resolved literal
    // offsets: evict() slides surviving procedures down by exactly this
    // much, so a non-word multiple here would knock them off alignment.
    // Slot 4 is deliberately left uncompiled — occupiedSizeOf has to skip
    // non-resident slots while scanning for the next procedure up.
    RuntimeStorage<5> runtime;
    uint32_t sizes[] = {6, 2, 14, 10};
    for(uint32_t i = 0; i < 4; i++)
    {
        runtime->markCompiled(i, runtime->allocate(sizes[i]));
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
    // A hasRoomFor(reserveFor(need)) that passes must be followed by an
    // allocate() that stays inside the arena — the invariant
    // compiler/src/assembler.cpp's own growForAttached eviction loop
    // depends on.
    RuntimeStorage<64> runtime;
    uint32_t allocations = 0;
    while(runtime->hasRoomFor(Runtime::reserveFor(6)))
    {
        uint32_t dest = runtime->allocate(6);
        CHECK(dest % 4 == 0);
        CHECK(dest + 6 <= ARENA_BASE + ARENA_SIZE);
        allocations++;
        CHECK(allocations <= ARENA_SIZE / 8); // GCOV_EXCL_LINE — a non-advancing cursor would spin here
    }
    CHECK(allocations > 0);
}

TEST(ArenaEndIsAlignedSoAFullAllocationNeverOvershootsIt)
{
    // F7: arenaEnd used to be left as the raw codeArenaBase+codeArenaSize
    // sum, so the gap to arenaCursor (always 4-aligned) wasn't guaranteed a
    // multiple of 4. A procedure that exactly filled that gap could then
    // have allocate()'s own rounding-up push arenaCursor past arenaEnd,
    // corrupting the next Assembler's capacity computation
    // (arenaEnd - arenaCursor underflows to ~2^32 halfwords instead of
    // tripping emit()'s bounds check).
    for(uint32_t skew = 0; skew < 4; skew++)
    {
        RuntimeStorage<1> runtime(ARENA_BASE, ARENA_SIZE + skew);
        uint32_t gap = runtime->arenaEnd - runtime->arenaCursor;
        CHECK(gap % 4 == 0);
        runtime->allocate(gap); // a procedure that exactly fills the remaining capacity
        CHECK(runtime->arenaCursor == runtime->arenaEnd); // lands exactly on it, never past
    }
}

TEST(InitFailsWithoutTouchingDispatchStateWhenAProcedureCantBeScanned)
{
    // A stack floor pinned at the current sp makes scanProcBody's own live
    // check fail immediately (test_proc_scan.cpp's own
    // ScanProcBodyStackFloorReachedReportsNotOk) — init() must propagate
    // that as a plain false, the same way it would a packed-field
    // overflow, rather than asserting or leaving the caller to find out
    // only once enterDispatch is already running.
    const Instr body[] = {bare(Op::RETURN)};
    ProcSource procs[] = {ProcSource{0, body, 1}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));
    uint32_t bodyOffset;
    decodeLeb128(programBytes, 0, bodyOffset);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = reinterpret_cast<Runtime *>(bytes);

    register uint32_t sp asm("sp");
    bool ok = runtime->init(programBytes, len, bodyOffset, 1, ARENA_BASE, ARENA_SIZE, sp, 0);
    CHECK(!ok);
}
