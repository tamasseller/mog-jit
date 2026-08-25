#include "Test.h"
#include "emitter.h"
#include "imm_synth.h"

using namespace jitc;

static uint32_t lengthOf(uint32_t value)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitSynthesizeImm32(e, 0, value);
    return e.halfwordCount();
}

TEST(synthesizeImm32Zero)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitSynthesizeImm32(e, 3, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x2300); // MOVS r3, #0
    CHECK(synthesizeImm32Length(0) == 1);
}

TEST(synthesizeImm32SingleByte)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitSynthesizeImm32(e, 0, 37);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x2025); // MOVS r0, #37
    CHECK(synthesizeImm32Length(37) == 1);
}

TEST(synthesizeImm32InternalZeroByteSkipsAdds)
{
    // 0x01000001 — bytes[1] and bytes[2] are both zero: LSLS runs for both,
    // but the intermediate ADDS is skipped both times (decompose's "skip a
    // zero byte" case); only the final nonzero byte gets an ADDS.
    uint16_t buf[8];
    Emitter e(buf, 8);
    uint32_t value = 0x01000001u;
    emitSynthesizeImm32(e, 5, value);
    CHECK(e.halfwordCount() == 5); // movs, lsls, lsls, lsls, adds
    CHECK(buf[0] == 0x2501); // MOVS r5, #1
    CHECK(buf[1] == 0x022D); // LSLS r5, r5, #8
    CHECK(buf[2] == 0x022D);
    CHECK(buf[3] == 0x022D);
    CHECK(buf[4] == 0x3501); // ADDS r5, #1
    CHECK(synthesizeImm32Length(value) == e.halfwordCount());
}

TEST(synthesizeImm32AllBytesNonzero)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    uint32_t value = 0xFFFFFFFFu;
    emitSynthesizeImm32(e, 1, value);
    CHECK(e.halfwordCount() == 7); // movs + 3*(lsls;adds)
    CHECK(synthesizeImm32Length(value) == e.halfwordCount());
}

TEST(synthesizeImm32LengthMatchesEmittedCountAcrossValues)
{
    uint32_t values[] = {0, 1, 0xff, 0x100, 0x1234, 0x123456, 0x12345678, 0xffffffffu, 0x80000000u};
    for(uint32_t v : values)
    {
        CHECK(synthesizeImm32Length(v) == lengthOf(v));
    }
}

TEST(poolingEligibilityTracksSynthesisLength)
{
    // The threshold's own edges, keyed off the real cost model rather than
    // hardcoded values: pooling costs a fixed 6 bytes (LDR + word), so it
    // must lose at 3 halfwords of synthesis and win at 4.
    CHECK(synthesizeImm32Length(0x1234) == 3);
    CHECK(!isPoolingEligible(0x1234));

    CHECK(synthesizeImm32Length(0x123400) == 4);
    CHECK(isPoolingEligible(0x123400));

    CHECK(!isPoolingEligible(0));          // 1 — a bare MOVS
    CHECK(!isPoolingEligible(0xff));       // 1
    CHECK(isPoolingEligible(0xffffffffu)); // 7 — the worst case
    CHECK(isPoolingEligible(0x80000003u)); // 5 — a TRAP sentinel

    // Every legal shift amount stays inline, which is what lets
    // translate_proc.cpp's IMM_ACC pooling leave shifts alone safely.
    for(uint32_t amount = 0; amount < 32; amount++)
    {
        CHECK(!isPoolingEligible(amount));
    }
}

TEST(fitsImm)
{
    CHECK(fitsImm8(0) && fitsImm8(255) && !fitsImm8(256) && !fitsImm8(-1));
    CHECK(fitsImm3(0) && fitsImm3(7) && !fitsImm3(8) && !fitsImm3(-1));
}
