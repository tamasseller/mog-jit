// Emitter's literal-pool primitives. The branch placeholders/patchers are
// covered indirectly through test_blocks.cpp's own LOOP/BR_TABLE cases;
// these three have no such caller of their own that pins their exact
// encoding, since translate_proc.cpp only ever exercises them in
// combination.
#include "Test.h"
#include "emitter.h"

using namespace jitc;

TEST(PlaceholderLiteralLoadParksRawTag)
{
    uint16_t buf[4];
    Emitter e(buf, 4);

    // 150 is deliberately not a multiple of 4: the parked value is a
    // bytecode-offset delta, so ArmV6M::setLiteralOffset's Uoff<2,8>
    // would reject it — placeholderLiteralLoad must not go through it.
    uint32_t site = e.placeholderLiteralLoad(2, 150);
    CHECK(site == 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x4A96); // LDR r2,[pc,#...], low byte 150
    CHECK(ArmV6M::isLiteralAccess(buf[0]));

    uint16_t tag;
    CHECK(e.getLiteralOffsetAt(site, tag));
    CHECK(tag == 150);
}

TEST(PatchLiteralOffsetResolvesSiteAndKeepsRegister)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    uint32_t site = e.placeholderLiteralLoad(0, 7);

    e.patchLiteralOffset(site, ArmV6M::Uoff<2, 8>(12));
    CHECK(buf[0] == 0x4803); // still LDR r0, now with imm8 == 12/4

    // Still matches isLiteralAccess afterwards — which is exactly why a
    // flush's scan window must never reach back into an earlier chunk.
    uint16_t off;
    CHECK(e.getLiteralOffsetAt(site, off));
    CHECK(off == 3);
}

TEST(LiteralAccessorsAreNoOpsPastWhatWasWritten)
{
    uint16_t buf[1];
    Emitter e(buf, 1);
    e.placeholderLiteralLoad(0, 0);
    uint32_t lost = e.placeholderLiteralLoad(0, 1); // past capacity
    CHECK(e.overflowed());

    uint16_t off;
    CHECK(!e.getLiteralOffsetAt(lost, off));
    e.patchLiteralOffset(lost, ArmV6M::Uoff<2, 8>(4)); // must not write out of bounds
    CHECK(buf[0] == 0x4800);
}

TEST(NonLiteralHalfwordIsNotReportedAsLiteralSite)
{
    uint16_t buf[2];
    Emitter e(buf, 2);
    uint32_t site = e.emit(ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(1)));

    uint16_t off;
    CHECK(!e.getLiteralOffsetAt(site, off));
}
