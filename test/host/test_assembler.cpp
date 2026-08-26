// Assembler's own primitives, tested in isolation from anything
// translate_proc.cpp layers on top: 32-bit immediate synthesis cost/
// shape, the literal pool's park/flush/dedup behavior (now tracked by
// stored (site, value) pairs, never a bytecode tag or an output scan),
// and Label/bind()'s own ordering guarantee — a label's target always
// lands after any pool words a flush inserts, never on top of them. The
// branch placeholder/patch primitives themselves are covered indirectly
// through test_blocks.cpp's own LOOP/BR_TABLE cases, same as before.
#include "Test.h"
#include "assembler.h"
#include "imm_synth.h"

using namespace jitc;

// ── imm32SynthCost / isPoolingEligible ──────────────────────────────────

TEST(imm32SynthCostMatchesEachShape)
{
    CHECK(Assembler::imm32SynthCost(0) == 1);
    CHECK(Assembler::imm32SynthCost(37) == 1);
    // 0x01000001 -- bytes[1]/bytes[2] are both zero: LSLS runs for both,
    // but the intermediate ADDS is skipped both times; only the final
    // nonzero byte gets an ADDS. movs, lsls, lsls, lsls, adds == 5.
    CHECK(Assembler::imm32SynthCost(0x01000001u) == 5);
    CHECK(Assembler::imm32SynthCost(0xFFFFFFFFu) == 7); // movs + 3*(lsls;adds)
    CHECK(Assembler::imm32SynthCost(0x1234) == 3);
    CHECK(Assembler::imm32SynthCost(0x123400) == 4);
}

TEST(poolingEligibilityTracksSynthesisCost)
{
    // The threshold's own edges, keyed off the real cost model rather
    // than hardcoded values: pooling costs a fixed 6 bytes (LDR + word),
    // so it must lose at 3 halfwords of synthesis and win at 4.
    CHECK(!Assembler::isPoolingEligible(0x1234));   // cost 3
    CHECK(Assembler::isPoolingEligible(0x123400));  // cost 4

    CHECK(!Assembler::isPoolingEligible(0));          // 1 -- a bare MOVS
    CHECK(!Assembler::isPoolingEligible(0xff));       // 1
    CHECK(Assembler::isPoolingEligible(0xffffffffu)); // 7 -- the worst case
    CHECK(Assembler::isPoolingEligible(0x80000003u)); // 5 -- a TRAP sentinel

    // Every legal shift amount stays inline, which is what lets
    // translate_proc.cpp's IMM_ACC pooling leave shifts alone safely.
    for(uint32_t amount = 0; amount < 32; amount++)
    {
        CHECK(!Assembler::isPoolingEligible(amount));
    }
}

TEST(fitsImm)
{
    CHECK(fitsImm8(0) && fitsImm8(255) && !fitsImm8(256) && !fitsImm8(-1));
    CHECK(fitsImm3(0) && fitsImm3(7) && !fitsImm3(8) && !fitsImm3(-1));
}

// ── materializeImm32 ─────────────────────────────────────────────────────

TEST(materializeImm32SynthesizesWhenBelowPoolingThreshold)
{
    uint16_t buf[8];
    Assembler a(buf, 8);
    a.materializeImm32(3, 0);
    CHECK(a.halfwordCount() == 1);
    CHECK(buf[0] == 0x2300); // MOVS r3, #0
}

TEST(materializeImm32SynthesizesASingleByteValue)
{
    uint16_t buf[8];
    Assembler a(buf, 8);
    a.materializeImm32(0, 37);
    CHECK(a.halfwordCount() == 1);
    CHECK(buf[0] == 0x2025); // MOVS r0, #37
}

TEST(materializeImm32SynthesizesAThreeHalfwordValueBelowThreshold)
{
    uint16_t buf[8];
    Assembler a(buf, 8);
    a.materializeImm32(0, 0x1234); // cost 3 -- still below POOLING_MIN_LENGTH(4)
    CHECK(a.halfwordCount() == 3);
}

TEST(materializeImm32ParksAPlaceholderForAnEligibleValue)
{
    uint16_t buf[16];
    Assembler a(buf, 16);
    a.materializeImm32(0, 0x12345678u); // cost 7 -- pool-eligible
    CHECK(a.halfwordCount() == 1);      // just the placeholder LDR at the site
    CHECK(ArmV6M::isLiteralAccess(buf[0]));
    CHECK(a.poolDebt() == 4 * 1 + 4);
}

// ── pool flush ───────────────────────────────────────────────────────────

TEST(FinalizeFlushesWithNoBranchAroundAndPadsToAWordBoundary)
{
    uint16_t buf[16];
    Assembler a(buf, 16);
    a.materializeImm32(0, 0x12345678u); // site at pc=0, pc now 2 (not word-aligned)

    uint32_t total = a.finalize();
    CHECK(total == 4); // LDR(1) + pad NOP(1) + pool word(2)
    CHECK(buf[0] == 0x4800); // LDR r0,[pc,#0] -- Align(0+4,4)=4, +0
    CHECK(buf[1] == 0xBF00); // pad, to reach the word boundary
    CHECK(buf[2] == 0x5678);
    CHECK(buf[3] == 0x1234);
}

TEST(FlushPoolMidProcedureAddsABranchAround)
{
    uint16_t buf[16];
    Assembler a(buf, 16);
    a.materializeImm32(0, 0x12345678u); // site at pc=0, pc now 2
    a.flushPool();
    CHECK(a.halfwordCount() == 4); // LDR(1) + branch-around(1) + pool word(2), already word-aligned

    uint16_t rawOff;
    CHECK(ArmV6M::getBranchOffset(buf[1], rawOff));
    int32_t delta = ArmV6M::signExtend(rawOff, 11) << 1;
    CHECK((uint32_t)(2 + 4 + delta) == a.pc()); // the branch-around lands past the pool word
    CHECK(buf[2] == 0x5678);
    CHECK(buf[3] == 0x1234);
}

TEST(FlushPoolDedupsIdenticalValuesToOneSharedWord)
{
    uint16_t buf[16];
    Assembler a(buf, 16);
    a.materializeImm32(0, 0x12345678u); // site A, pc 0 -> 2
    a.materializeImm32(1, 0x12345678u); // site B, same value, pc 2 -> 4
    a.finalize();

    // Both sites must resolve to the exact same pool word.
    uint16_t offA, offB;
    CHECK(ArmV6M::getLiteralOffset(buf[0], offA));
    CHECK(ArmV6M::getLiteralOffset(buf[1], offB));
    uint32_t wordA = ((0u + 4u) & ~3u) + offA * 4u;
    uint32_t wordB = ((2u + 4u) & ~3u) + offB * 4u;
    CHECK(wordA == wordB);
    CHECK(buf[wordA / 2] == 0x5678);
    CHECK(buf[wordA / 2 + 1] == 0x1234);
}

TEST(PoolFlushesAutomaticallyOnceFull)
{
    uint16_t buf[256];
    Assembler a(buf, 256);
    for(uint32_t i = 0; i < 16; i++)
    {
        a.materializeImm32(0, 0x10000000u + i); // 16 distinct pool-eligible values
    }
    CHECK(a.poolDebt() != 0); // still open, all 16 pending

    a.materializeImm32(0, 0x20000000u); // 17th -- must flush the first 16 before parking this one
    CHECK(a.poolDebt() == 4 * 1 + 4);   // exactly the new site remains pending
}

// ── Label / bind() ───────────────────────────────────────────────────────

TEST(LabelSelfLinksOnItsFirstBranchAndBindResolvesIt)
{
    uint16_t buf[8];
    Assembler a(buf, 8);
    Label label;
    a.branchTo(label, ArmV6M::Condition::EQ); // site 0
    a.emit(ArmV6M::mvns(ArmV6M::LoReg(0), ArmV6M::LoReg(0))); // filler, site 2

    a.bind(label);
    uint16_t rawOff;
    CHECK(ArmV6M::getCondBranchOffset(buf[0], rawOff));
    int32_t delta = ArmV6M::signExtend(rawOff, 8) << 1;
    CHECK((uint32_t)(0 + 4 + delta) == a.pc());
}

TEST(LabelChainsMultipleBranchesAndBindResolvesEveryOne)
{
    uint16_t buf[8];
    Assembler a(buf, 8);
    Label label;
    a.branchTo(label);                        // site 0
    a.branchTo(label, ArmV6M::Condition::NE); // site 2, chains onto site 0

    a.bind(label);
    uint16_t rawOff;
    CHECK(ArmV6M::getBranchOffset(buf[0], rawOff));
    int32_t delta0 = ArmV6M::signExtend(rawOff, 11) << 1;
    CHECK((uint32_t)(0 + 4 + delta0) == a.pc());
    CHECK(ArmV6M::getCondBranchOffset(buf[1], rawOff));
    int32_t delta1 = ArmV6M::signExtend(rawOff, 8) << 1;
    CHECK((uint32_t)(2 + 4 + delta1) == a.pc());
}

TEST(BindFlushesAnyOpenPoolChunkBeforeResolvingTheTarget)
{
    uint16_t buf[16];
    Assembler a(buf, 16);
    a.materializeImm32(0, 0x12345678u); // pool site at pc=0, pc now 2
    Label label;
    a.branchTo(label); // site 2, pc now 4

    a.bind(label); // must flush the pool (branch-around + pad + word) *before* resolving label's own target
    uint16_t rawOff;
    CHECK(ArmV6M::getBranchOffset(buf[1], rawOff));
    int32_t delta = ArmV6M::signExtend(rawOff, 11) << 1;
    CHECK((uint32_t)(2 + 4 + delta) == a.pc());
    // label's own target lands past the flushed branch-around and pool
    // word, never on top of either.
    CHECK(a.pc() > 4 + 4);
}

// ── overflow ─────────────────────────────────────────────────────────────

TEST(EmitPastCapacityLatchesOverflowedOnADetachedAssembler)
{
    uint16_t buf[1];
    Assembler a(buf, 1);
    a.emit(0);
    CHECK(!a.overflowed());
    a.emit(0); // past capacity -- a safe no-op, not a write
    CHECK(a.overflowed());
    CHECK(a.halfwordCount() == 1);
}
