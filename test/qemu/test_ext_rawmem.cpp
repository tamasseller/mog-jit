// The raw-memory extension, executing on the target: the first coverage of
// the extension seam that runs emitted code rather than inspecting it.

#include <cstdint>

#include "instr.h"
#include "run_program.h"
#include "encode_instr.h"
#include "ext_rawmem.h"
#include "Test.h"

using namespace jitc;

namespace
{
constexpr uint32_t CAPACITY = 256;

/* encodeJitProgram takes Instr[], which cannot express an extension op, so
 * a body with one in it is spliced a piece at a time. */
struct Body
{
    uint8_t bytes[CAPACITY];
    uint32_t len = 0;

    void add(const Instr *instrs, uint32_t count)
    {
        len += encodeBody(instrs, count, bytes + len, CAPACITY - len);
    }

    void ext(uint8_t opcode) {bytes[len++] = opcode;}

    /* Address on the stack, value left in acc — the store's own shape. */
    void store(int32_t value, int32_t offset, uint8_t opcode)
    {
        const Instr seq[] = {CONST(offset), PUSH(), CONST(value)};
        add(seq, 3);
        ext(opcode);
    }

    /* Address in acc; the load replaces it with the value, and touches the
     * operand stack not at all. */
    void load(int32_t offset, uint8_t opcode)
    {
        const Instr seq[] = {CONST(offset)};
        add(seq, 1);
        ext(opcode);
    }
};

ProgramResult runBody(const Body &body, uint32_t totalDepth)
{
    static uint8_t arena[512];

    uint8_t bytes[CAPACITY];
    uint32_t len = 0;
    encodeLeb128(0, bytes, len, CAPACITY);          // max_call_depth
    encodeLeb128(totalDepth, bytes, len, CAPACITY); // total_depth
    encodeLeb128(1, bytes, len, CAPACITY);          // proc_count
    encodeLeb128(0, bytes, len, CAPACITY);          // proc 0 arg_count

    for(uint32_t i = 0; i < body.len; i++) bytes[len++] = body.bytes[i];
    len = appendProgramFrame(bytes, len, CAPACITY);

    for(uint32_t i = 0; i < RAWMEM_BYTES; i++) g_rawMem[i] = 0;

    return Executor::split((uint32_t)(uintptr_t)arena, sizeof(arena),
            (uint32_t)(uintptr_t)&__bss_end + STACK_SLACK_ABOVE_BSS, /*interruptReserve=*/0)
        .run(bcMapped(bytes), len, nullptr, 0);
}

const Instr RETURN_ONLY[] = {bare(Op::RETURN)};
} // namespace

TEST(RawMemWordRoundTrips)
{
    Body b;
    b.store(0x12345678, 4, RAWMEM_ST32);
    b.load(4, RAWMEM_LD32);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 1);
    CHECK(!r.trapped);
    CHECK(r.value == 0x12345678u);
}

TEST(RawMemStoreLeavesAccReadableThroughACoreScratchUse)
{
    // ST32 declares no writesAcc, so the value has to still be readable
    // after it — including across a core emission that wants a scratch
    // register of its own, which an `AND` by an immediate too wide for
    // Thumb-1 is.
    Body b;
    b.store(9, 4, RAWMEM_ST32);

    const Instr tail[] = {opImm(Op::AND, 112), bare(Op::RETURN)};
    b.add(tail, 2);

    ProgramResult r = runBody(b, 1);
    CHECK(!r.trapped);
    CHECK(r.value == (9u & 112u));
}

TEST(RawMemNarrowWidthsWriteOnlyTheirOwnBytes)
{
    Body b;
    b.store((int32_t)0xffffffff, 0, RAWMEM_ST32);
    b.store(0, 0, RAWMEM_ST8);  // clears byte 0 only
    b.load(0, RAWMEM_LD32);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 1);
    CHECK(!r.trapped);
    CHECK(r.value == 0xffffff00u);
}

TEST(RawMemHalfwordReadsBackWhatItWrote)
{
    Body b;
    b.store(0xbeef, 8, RAWMEM_ST16);
    b.load(8, RAWMEM_LD16);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 1);
    CHECK(!r.trapped);
    CHECK(r.value == 0xbeefu);
}

TEST(RawMemOffsetsAreMaskedAndAligned)
{
    // 0x404 is past the buffer and lands on 4; 6 aligns down to 4 for a
    // word access. Both reach the same location as a plain 4.
    Body b;
    b.store(0x0badf00d, 0x404, RAWMEM_ST32);
    b.load(6, RAWMEM_LD32);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 1);
    CHECK(!r.trapped);
    CHECK(r.value == 0x0badf00du);
}

/* src, dstStart, dstEnd pushed in that order — dstEnd ends up on top,
 * which is the order the emitted code pops them in. */
void memmove(Body &b, int32_t src, int32_t dstStart, int32_t dstEnd)
{
    const Instr args[] = {
        CONST(src), PUSH(), CONST(dstStart), PUSH(), CONST(dstEnd), PUSH()};
    b.add(args, 6);
    b.ext(RAWMEM_MEMMOVE);
}

TEST(RawMemMemmoveCopiesThreeStackOperands)
{
    Body b;
    b.store((int32_t)0xaabbccdd, 0, RAWMEM_ST32);
    memmove(b, /*src=*/0, /*dstStart=*/16, /*dstEnd=*/20);
    b.load(16, RAWMEM_LD32);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 3);
    CHECK(!r.trapped);
    CHECK(r.value == 0xaabbccddu);
}

TEST(RawMemMemmoveOfAnEmptyRangeCopiesNothing)
{
    Body b;
    b.store(0x55555555, 16, RAWMEM_ST32);
    memmove(b, /*src=*/0, /*dstStart=*/16, /*dstEnd=*/16);
    b.load(16, RAWMEM_LD32);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 3);
    CHECK(!r.trapped);
    CHECK(r.value == 0x55555555u); // untouched — the loop must not run once
}

TEST(RawMemMemmoveOfAnInvertedRangeCopiesNothing)
{
    // end below start is the nonsensical case: it rolls over to an empty
    // walk rather than to a huge one, because the comparison is unsigned.
    Body b;
    b.store(0x55555555, 16, RAWMEM_ST32);
    memmove(b, /*src=*/0, /*dstStart=*/20, /*dstEnd=*/4);
    b.load(16, RAWMEM_LD32);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 3);
    CHECK(!r.trapped);
    CHECK(r.value == 0x55555555u);
}

/* aStart, aEnd, bStart — bStart on top, the order the emitted code pops. */
void memcmp(Body &b, int32_t aStart, int32_t aEnd, int32_t bStart)
{
    const Instr args[] = {
        CONST(aStart), PUSH(), CONST(aEnd), PUSH(), CONST(bStart), PUSH()};
    b.add(args, 6);
    b.ext(RAWMEM_MEMCMP);
}

TEST(RawMemCmpFindsEqualRangesThroughTheCHelper)
{
    Body b;
    b.store((int32_t)0xaabbccdd, 0, RAWMEM_ST32);
    memmove(b, /*src=*/0, /*dstStart=*/16, /*dstEnd=*/20);
    memcmp(b, /*aStart=*/0, /*aEnd=*/4, /*bStart=*/16);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 3);
    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

TEST(RawMemCmpReturnsTheFirstDifferingBytesDifference)
{
    Body b;
    b.store(0x10, 0, RAWMEM_ST8);
    b.store(0x03, 16, RAWMEM_ST8);
    memcmp(b, /*aStart=*/0, /*aEnd=*/1, /*bStart=*/16);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 3);
    CHECK(!r.trapped);
    CHECK(r.value == 0x10u - 0x03u);
}

TEST(RawMemCmpReturnsANegativeDifferenceAsTwosComplement)
{
    Body b;
    b.store(0x03, 0, RAWMEM_ST8);
    b.store(0x10, 16, RAWMEM_ST8);
    memcmp(b, /*aStart=*/0, /*aEnd=*/1, /*bStart=*/16);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 3);
    CHECK(!r.trapped);
    CHECK(r.value == (uint32_t)(int32_t)(0x03 - 0x10));
}

TEST(RawMemCmpOfAnEmptyRangeIsEqual)
{
    Body b;
    b.store((int32_t)0xffffffff, 0, RAWMEM_ST32);
    memcmp(b, /*aStart=*/4, /*aEnd=*/4, /*bStart=*/0);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 3);
    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

/* aStart, aEnd, bStart, bEnd — bEnd on top. The fourth is the one the
 * emitted code pushes for the helper to read through r3. */
void slicecmp(Body &b, int32_t aStart, int32_t aEnd, int32_t bStart, int32_t bEnd)
{
    const Instr args[] = {
        CONST(aStart), PUSH(), CONST(aEnd), PUSH(),
        CONST(bStart), PUSH(), CONST(bEnd), PUSH()};
    b.add(args, 8);
    b.ext(RAWMEM_SLICECMP);
}

TEST(RawMemSliceCmpReadsItsFourthOperandOffTheStack)
{
    Body b;
    b.store((int32_t)0xaabbccdd, 0, RAWMEM_ST32);
    memmove(b, /*src=*/0, /*dstStart=*/16, /*dstEnd=*/20);
    slicecmp(b, /*aStart=*/0, /*aEnd=*/4, /*bStart=*/16, /*bEnd=*/20);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 5);
    CHECK(!r.trapped);
    CHECK(r.value == 0); // equal ranges, so the pushed bEnd was read correctly
}

TEST(RawMemSliceCmpComparesLengthsWhenThePrefixMatches)
{
    // Same bytes, but the second range is one shorter — the answer depends
    // entirely on the fourth operand.
    Body b;
    b.store((int32_t)0xaabbccdd, 0, RAWMEM_ST32);
    memmove(b, /*src=*/0, /*dstStart=*/16, /*dstEnd=*/20);
    slicecmp(b, /*aStart=*/0, /*aEnd=*/4, /*bStart=*/16, /*bEnd=*/19);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 5);
    CHECK(!r.trapped);
    CHECK(r.value == 1); // lenA 4 - lenB 3
}

TEST(RawMemSliceCmpReturnsTheFirstDifferenceBeforeAnyLengthDifference)
{
    Body b;
    b.store(0x40, 0, RAWMEM_ST8);
    b.store(0x10, 16, RAWMEM_ST8);
    slicecmp(b, /*aStart=*/0, /*aEnd=*/4, /*bStart=*/16, /*bEnd=*/17);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 5);
    CHECK(!r.trapped);
    CHECK(r.value == 0x40u - 0x10u);
}

TEST(RawMemReachesTheTopOfTheBuffer)
{
    // The whole 1KB is addressable now: nothing has to reserve headroom for
    // a length, so a start offset above half the buffer is reachable. The
    // second store is the one that gives this teeth — a mask narrow enough
    // to alias the two would fold it onto the first.
    Body b;
    b.store((int32_t)0xcafebabe, RAWMEM_BYTES - 4, RAWMEM_ST32);
    b.store((int32_t)0x0000dead, RAWMEM_BYTES - 4 - 512, RAWMEM_ST32);
    b.load(RAWMEM_BYTES - 4, RAWMEM_LD32);
    b.add(RETURN_ONLY, 1);

    ProgramResult r = runBody(b, 1);
    CHECK(!r.trapped);
    CHECK(r.value == 0xcafebabeu);
}
