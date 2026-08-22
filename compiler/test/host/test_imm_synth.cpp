#include "Test.h"
#include "emitter.h"
#include "imm_synth.h"

using namespace jitc;

namespace {
uint32_t lengthOf(uint32_t value) {
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitSynthesizeImm32(e, 0, value);
    return e.halfwordCount();
}
}

TEST(SynthesizeImm32Zero)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitSynthesizeImm32(e, 3, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x2300); // MOVS r3, #0
    CHECK(synthesizeImm32Length(0) == 1);
}

TEST(SynthesizeImm32SingleByte)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitSynthesizeImm32(e, 0, 37);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x2025); // MOVS r0, #37
    CHECK(synthesizeImm32Length(37) == 1);
}

TEST(SynthesizeImm32InternalZeroByteSkipsAdds)
{
    // 0x01000001 — bytes[1] and bytes[2] are both zero: LSLS runs for both,
    // but the intermediate ADDS is skipped both times (decompose's "skip a
    // zero byte" case), only the final nonzero byte gets an ADDS.
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

TEST(SynthesizeImm32AllBytesNonzero)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    uint32_t value = 0xFFFFFFFFu;
    emitSynthesizeImm32(e, 1, value);
    CHECK(e.halfwordCount() == 7); // movs + 3*(lsls;adds)
    CHECK(synthesizeImm32Length(value) == e.halfwordCount());
}

TEST(SynthesizeImm32LengthMatchesEmittedCountAcrossValues)
{
    uint32_t values[] = {0, 1, 0xff, 0x100, 0x1234, 0x123456, 0x12345678, 0xffffffffu, 0x80000000u};
    for(uint32_t v : values) CHECK(synthesizeImm32Length(v) == lengthOf(v));
}

TEST(FitsImm)
{
    CHECK(fitsImm8(0) && fitsImm8(255) && !fitsImm8(256) && !fitsImm8(-1));
    CHECK(fitsImm3(0) && fitsImm3(7) && !fitsImm3(8) && !fitsImm3(-1));
}
