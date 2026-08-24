// Runtime's arena bookkeeping, exercised on the host rather than only
// through the real QEMU image (compiler/qemu/compile_proc_real.cpp is its
// only other caller). What's worth testing cheaply here is the 4-byte
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

// Normally runtime_host.cpp's own address of translatorTrampoline; any
// distinct non-zero value serves as the not-resident marker here.
extern const uint32_t trampolineAddr = 0xDEADBEEFu;

namespace
{
const uint32_t ARENA_BASE = 0x20000000;
const uint32_t ARENA_SIZE = 512;

// Built by reinterpreting raw bytes exactly the way every real caller
// does — Runtime is a trivial aggregate with a flexible array member and
// is never constructed normally.
template<uint32_t procCount>
class RuntimeStorage
{
    alignas(8) uint8_t bytes[sizeof(Runtime) + (procCount + 1) * sizeof(DispatchEntry)] = {};

public:
    Runtime *operator->()
    {
        return reinterpret_cast<Runtime *>(bytes);
    }

    RuntimeStorage(uint32_t base = ARENA_BASE, uint32_t size = ARENA_SIZE)
    {
        (*this)->init(base, size, nullptr, procCount, 0, 0);
    }
};
}

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
    // compile_proc_real.cpp's own eviction loop depends on.
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
