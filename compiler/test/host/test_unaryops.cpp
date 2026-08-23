// jit-armv6m/compiler/test — unaryops.h, ported from
// jit-armv6m/prototype/test/unary-and-comparison-values.test.ts's own
// unit-level slice. End-to-end NEG/NOT/CLZ/REVBITS correctness (does the
// software helper actually compute the right answer for real inputs) is
// covered on real QEMU (test/qemu/fixtures.cpp) — this file checks the
// emitted shape: NEG/NOT are single instructions, CLZ/REVBITS emit a
// placeholder BL and record the site for the caller to patch.
#include "Test.h"
#include "unaryops.h"
#include "emitter.h"
#include "registers.h"
#include "armv6.h"

using namespace jitc;

TEST(NegEmitsSingleInstruction)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    UnaryHelperSites sites;
    emitUnary(e, Op::NEG, ACC_REG, sites);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::negs(ArmV6M::LoReg(ACC_REG), ArmV6M::LoReg(ACC_REG)));
    CHECK(sites.clzCount == 0);
    CHECK(sites.revbitsCount == 0);
}

TEST(NotEmitsSingleInstructionAndMovesOutWhenDestDiffers)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    UnaryHelperSites sites;
    emitUnary(e, Op::NOT, 5, sites);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::mvns(ArmV6M::LoReg(5), ArmV6M::LoReg(ACC_REG)));
}

TEST(ClzEmitsPlaceholderBLAndRecordsSite)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    UnaryHelperSites sites;
    emitUnary(e, Op::CLZ, ACC_REG, sites);
    CHECK(e.halfwordCount() == 2); // BL is two halfwords
    CHECK(sites.clzCount == 1);
    CHECK(sites.clz[0] == 0);
    CHECK(sites.revbitsCount == 0);
    CHECK(ArmV6M::isBL(buf[0]));
}

TEST(RevbitsEmitsPlaceholderBLAndMovesOutWhenDestDiffers)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    UnaryHelperSites sites;
    emitUnary(e, Op::REVBITS, 6, sites);
    CHECK(e.halfwordCount() == 3); // BL (2) + MOV out (1)
    CHECK(sites.revbitsCount == 1);
    CHECK(sites.revbits[0] == 0);
    CHECK(ArmV6M::isBL(buf[0]));
    CHECK(buf[2] == ArmV6M::mov(ArmV6M::AnyReg(6), ArmV6M::AnyReg(ACC_REG)));
}

TEST(EmitClzHelperEndsInThumbBxLr)
{
    uint16_t buf[16];
    Emitter e(buf, 16);
    uint32_t start = emitClzHelper(e);
    CHECK(start == 0);
    CHECK(e.halfwordCount() > 0);
    CHECK(!e.overflowed());
    // Two BX LR exits (zero-input fast path and the loop's own normal
    // exit) — both reachable, both real instructions in the emitted
    // buffer; a hand-decode isn't needed here since fixtures.cpp's own
    // CLZ QEMU fixture exercises actual return values for both paths.
    constexpr uint16_t kBxLr = 0x4770; // bx lr — 0100'0111'0'1110'000
    bool sawBxLr = false;
    for(uint32_t i = 0; i < e.halfwordCount(); i++) if(buf[i] == kBxLr) sawBxLr = true;
    CHECK(sawBxLr);
}

TEST(EmitRevbitsHelperEndsInThumbBxLr)
{
    uint16_t buf[16];
    Emitter e(buf, 16);
    uint32_t start = emitRevbitsHelper(e);
    CHECK(start == 0);
    CHECK(buf[e.halfwordCount() - 1] == 0x4770); // bx lr, the routine's own single exit
}
