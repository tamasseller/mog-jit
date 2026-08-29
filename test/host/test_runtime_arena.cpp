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
#include "ext.h"
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
        uint32_t code = (*this)->init(programBytes, len, bodyOffset, procCount, base, size, 0, 0);
        assert(code == 0); // GCOV_EXCL_LINE — this file's own encoding setup, not the thing under test
        (void)code;
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
        runtime->markCompiled(i, runtime->allocate(sizes[i]), /*lruTick=*/0);
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

TEST(TheExtensionScratchIsClearOfTheWordReturnHelperTailStamps)
{
    // runtime.S's returnHelperTail stamps [slotAddr, #4] unconditionally,
    // the sentinel included — that is the point, since guarding it would
    // put a branch on return-from-entry, which slots[0] exists to keep
    // free. So the extension scratch must start past that word. The
    // static_asserts in runtime_internal.h pin it to the struct; this
    // pins the consequence emitted code actually depends on, in the form
    // ext.h hands out.
    //
    // Built from the asm-visible constants rather than offsetof(Runtime,
    // slots), which is legitimately wider on this 64-bit host than on the
    // 32-bit target runtime.S is assembled for — the same reason
    // runtime_internal.h guards its own layout static_asserts.
    const uint32_t sentinel = RUNTIME_DISPATCH_TABLE_OFFSET - DISPATCH_SENTINEL_OFFSET;
    const uint32_t stamped = sentinel + offsetof(ProcSlot, lastUsed);
    for(uint32_t w = 0; w < RUNTIME_EXT_STATE_WORDS; w++)
    {
        CHECK(extStateOffset(w) > stamped);
    }
    // ...and still covers the sentinel slot's whole remaining tail, so no
    // storage was quietly lost on the way.
    CHECK(extStateOffset(0) == stamped + 4);
    CHECK(extStateOffset(RUNTIME_EXT_STATE_WORDS - 1) + 4 == sentinel + sizeof(ProcSlot));
}

TEST(AFreshlyCompiledProcedureIsTheYoungestNotTheOldest)
{
    // markCompiled stamps the live tick rather than zeroing, and that is
    // load-bearing: the dispatch that reached the translator already
    // stamped this slot in runtime.S's callHelper before tail-jumping to
    // the trampoline, so zeroing here would present a procedure that was
    // just paid for as the oldest thing in the arena — the very next
    // victim, evicted before it ever ran once. Nothing else in the host
    // suite exercises findEvictionVictim's ordering at all.
    RuntimeStorage<3> runtime;
    runtime->markCompiled(0, runtime->allocate(8), /*lruTick=*/10);
    runtime->markCompiled(1, runtime->allocate(8), /*lruTick=*/20);

    CHECK(runtime->findEvictionVictim(/*now=*/21) == 0); // slot 1 is younger

    runtime->markCompiled(2, runtime->allocate(8), /*lruTick=*/30);
    CHECK(runtime->findEvictionVictim(/*now=*/31) == 0); // still the oldest, not the newest
}

TEST(NoResidentProcedureLeavesNothingToEvict)
{
    // What Assembler::growForAttached reads as "the arena cannot be made
    // to fit this" — the -1 that turns into RESOURCE_EXHAUSTED_ARENA.
    RuntimeStorage<2> runtime;
    CHECK(runtime->findEvictionVictim(/*now=*/5) < 0);
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
    // that rather than asserting or leaving the caller to find out only
    // once enterDispatch is already running. Checked as the specific code:
    // running out of stack here and a body that was never well-formed are
    // the two halves of scanProcBody's !ok, and the whole point of
    // failCode is that they no longer arrive as the same answer.
    const Instr body[] = {bare(Op::RETURN)};
    ProcSource procs[] = {ProcSource{0, body, 1}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));
    uint32_t bodyOffset;
    decodeLeb128(programBytes, 0, bodyOffset);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = reinterpret_cast<Runtime *>(bytes);

    register uint32_t sp asm("sp");
    CHECK(runtime->init(programBytes, len, bodyOffset, 1, ARENA_BASE, ARENA_SIZE, sp, 0)
        == RESOURCE_EXHAUSTED_SCAN_STACK);
}

TEST(InitReportsAnUnterminatedBodySeparatelyFromRunningOutOfStack)
{
    // The other half of scanProcBody's !ok: a LOOP with nothing closing
    // it, so the walk runs off the blob. Same rejection point as the
    // stack-floor case above, deliberately a different answer — this one
    // is a program that was never well-formed, and no arena or stack size
    // changes that.
    const Instr body[] = {bare(Op::LOOP), CONST(1)};
    ProcSource procs[] = {ProcSource{0, body, 2}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));
    uint32_t bodyOffset;
    decodeLeb128(programBytes, 0, bodyOffset);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = reinterpret_cast<Runtime *>(bytes);

    CHECK(runtime->init(programBytes, len, bodyOffset, 1, ARENA_BASE, ARENA_SIZE, 0, 0)
        == RESOURCE_PROGRAM_BODY_UNTERMINATED);
}

TEST(InitReportsAnArgCountPastProcSlotsOwnFieldWidth)
{
    // MAX_ARG_COUNT is ProcSlot's packed staticInfo field, not an ISA
    // limit: the program is perfectly well-formed, this implementation
    // just can't record it. Hence a LIMIT code rather than a PROGRAM one.
    const Instr body[] = {bare(Op::RETURN)};
    ProcSource procs[] = {ProcSource{ProcSlot::MAX_ARG_COUNT + 1, body, 1}};
    uint8_t programBytes[16];
    uint32_t len = encodeProgram(procs, 1, programBytes, sizeof(programBytes));
    uint32_t bodyOffset;
    decodeLeb128(programBytes, 0, bodyOffset);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = reinterpret_cast<Runtime *>(bytes);

    CHECK(runtime->init(programBytes, len, bodyOffset, 1, ARENA_BASE, ARENA_SIZE, 0, 0)
        == RESOURCE_LIMIT_ARG_COUNT);
}

TEST(InitReportsAnUnknownOpcodeAsADeploymentMismatch)
{
    // A body byte in the extension range with no extension registered. Its
    // own code, not BODY_UNTERMINATED: it is plausibly the right program
    // against an image built without that extension, which is a different
    // thing from bytes that were never well-formed.
    uint8_t programBytes[] = {0x01, 0x00, 0x80}; // proc_count=1, arg_count=0, body=[EXT 0x80]
    const uint32_t bodyOffset = 1;

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = reinterpret_cast<Runtime *>(bytes);

    CHECK(runtime->init(programBytes, sizeof(programBytes), bodyOffset, 1, ARENA_BASE, ARENA_SIZE, 0, 0,
        /*extension=*/nullptr) == RESOURCE_PROGRAM_EXT_UNKNOWN);
}

// ── the extension seam at init (compiler/src/ext.h) ──────────────────────

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

// proc_count=1, arg_count=0, body=[0x80, RETURN]
uint32_t extProgram(uint8_t *out)
{
    out[0] = 0x01;
    out[1] = 0x00;
    out[2] = 0x80;
    out[3] = 100; // RETURN
    return 4;
}
} // namespace

TEST(InitAcceptsAWellFormedExtensionDeclaration)
{
    // The walk must let it through, or nothing downstream of decode is ever
    // exercised: this is what makes proc_scan's boundary and blocks' span
    // budget real rather than theoretical.
    const ExtHooks *ext = &EXT_OK;
    uint8_t programBytes[8];
    uint32_t len = extProgram(programBytes);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = reinterpret_cast<Runtime *>(bytes);

    CHECK(runtime->init(programBytes, len, 1, 1, ARENA_BASE, ARENA_SIZE, 0, 0, ext) == 0);
}

TEST(InitRejectsAnExtensionBuiltAgainstADifferentAbiVersion)
{
    // Checked before the walk can call decode() at all: an extension built
    // against a different seam must not have its declarations trusted.
    const ExtHooks *ext = &EXT_STALE_ABI;
    uint8_t programBytes[8];
    uint32_t len = extProgram(programBytes);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = reinterpret_cast<Runtime *>(bytes);

    CHECK(runtime->init(programBytes, len, 1, 1, ARENA_BASE, ARENA_SIZE, 0, 0, ext)
        == RESOURCE_PROGRAM_EXT_ABI);
}

TEST(InitRejectsACallShapedExtensionDeclarationAsUnsupported)
{
    const ExtHooks *ext = &EXT_CALL_SHAPED;
    uint8_t programBytes[8];
    uint32_t len = extProgram(programBytes);

    alignas(8) uint8_t bytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    Runtime *runtime = reinterpret_cast<Runtime *>(bytes);

    CHECK(runtime->init(programBytes, len, 1, 1, ARENA_BASE, ARENA_SIZE, 0, 0, ext)
        == RESOURCE_PROGRAM_EXT_UNSUPPORTED);
}
