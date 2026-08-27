// Assembler's own primitives, tested in isolation from anything
// translate_proc.cpp layers on top: 32-bit immediate synthesis cost/
// shape, the literal pool's park/flush/dedup behavior (now tracked by
// stored (site, value) pairs, never a bytecode tag or an output scan),
// and Label/bind()'s own ordering guarantee — a label's target always
// lands after any pool words a flush inserts, never on top of them. The
// branch placeholder/patch primitives themselves are covered indirectly
// through test_blocks.cpp's own LOOP/BR_TABLE cases, same as before.
#include "Test.h"
#include "Mock.h"

#include "runtime_internal.h"
#include "host_runtime_support.h"

#include "assembler.h"
#include "imm_synth.h"

using namespace jitc;

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
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(3), ArmV6M::Imm<8>(0))); // MOVS r3, #0
}

TEST(materializeImm32SynthesizesASingleByteValue)
{
    uint16_t buf[8];
    Assembler a(buf, 8);
    a.materializeImm32(0, 37);
    CHECK(a.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(37))); // MOVS r0, #37
}

TEST(materializeImm32PoolsAValueWhoseUnshiftedPatternJustMissesImm8)
{
    // 0x101 has bit8 and bit0 both set, so unshift finds shift=0 (bit0 is
    // already set) and hands back the value itself as its own pattern:
    // 257, one past fitsImm8's ceiling. Neither the direct-imm8 form, the
    // bitwise-NOT form, nor the shift form apply, so this still has to
    // fall through to pooling.
    uint16_t buf[8];
    Assembler a(buf, 8);
    a.materializeImm32(0, 0x101u);
    CHECK(a.halfwordCount() == 1); // just the placeholder LDR at the site
    CHECK(ArmV6M::isLiteralAccess(buf[0]));
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
    CHECK(buf[0] == ArmV6M::ldrPc(ArmV6M::LoReg(0), ArmV6M::Uoff<2, 8>(0))); // LDR r0,[pc,#0] -- Align(0+4,4)=4, +0
    CHECK(buf[1] == ArmV6M::nop()); // pad, to reach the word boundary
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
    // Bit 28 stays set alongside the varying low bits across the whole
    // loop (i never reaches 16, so it can't carry that far) — two set
    // bits far enough apart that unshift's own pattern always spans more
    // than 8 bits, so every one of these is genuinely pool-eligible,
    // unlike a bare 0x10000000u + i, where i=0 is a clean power of two
    // materializeImm32's shift-trick would synthesize instead of pooling.
    uint16_t buf[256];
    Assembler a(buf, 256);
    for(uint32_t i = 0; i < 16; i++)
    {
        a.materializeImm32(0, 0x10000001u + i); // 16 distinct pool-eligible values
    }
    CHECK(a.poolDebt() != 0); // still open, all 16 pending

    a.materializeImm32(0, 0x20000001u); // 17th -- must flush the first 16 before parking this one
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

TEST(UnconditionalBranchToFlushesAnyOpenPoolChunkNoGuardRightAfter)
{
    // branchTo(Label&)'s own unconditional branch is itself a point
    // nothing ever falls through -- so it flushes the still-open pool
    // right there, no-guard: no separate branch-around, the pool word
    // lands directly after the branch itself.
    uint16_t buf[16];
    Assembler a(buf, 16);
    a.materializeImm32(0, 0x12345678u); // pool site at pc=0, pc now 2
    Label label;
    a.branchTo(label); // site 2, pc now 4, flushes no-guard right after

    CHECK(a.pc() == 4 + 4); // site(2) + branch(2) + pool word(4), no branch-around
    CHECK(buf[2] == 0x5678);
    CHECK(buf[3] == 0x1234);

    a.bind(label); // pool already empty -- only resolves the branch target
    uint16_t rawOff;
    CHECK(ArmV6M::getBranchOffset(buf[1], rawOff));
    int32_t delta = ArmV6M::signExtend(rawOff, 11) << 1;
    CHECK((uint32_t)(2 + 4 + delta) == a.pc());
}

TEST(BindFlushesAnyOpenPoolChunkBeforeResolvingTheTarget)
{
    // Unlike an unconditional branchTo, a conditional one falls through
    // when not taken, so it must not auto-flush -- the pool stays open
    // until bind() itself flushes it (guarded: a real branch-around, since
    // bind()'s own target can be reached via fallthrough too).
    uint16_t buf[16];
    Assembler a(buf, 16);
    a.materializeImm32(0, 0x12345678u); // pool site at pc=0, pc now 2
    Label label;
    a.branchTo(label, ArmV6M::Condition::EQ); // site 2, pc now 4, pool still open

    a.bind(label); // must flush the pool (branch-around + pad + word) *before* resolving label's own target
    uint16_t rawOff;
    CHECK(ArmV6M::getCondBranchOffset(buf[1], rawOff));
    int32_t delta = ArmV6M::signExtend(rawOff, 8) << 1;
    CHECK((uint32_t)(2 + 4 + delta) == a.pc());
    // label's own target lands past the flushed branch-around and pool
    // word, never on top of either.
    CHECK(a.pc() > 4 + 4);
}

// ── overflow ─────────────────────────────────────────────────────────────

TEST(EmitPastCapacityBailsOnADetachedAssembler)
{
    uint16_t buf[1];
    Assembler a(buf, 1);
    a.emit(0);
    MOCK(runtime)::EXPECT(runtimeBail).withParam(RESOURCE_ERROR_CODE);
    EXPECT_RESOURCE_ERROR(a.emit(0)); // past capacity -- escapes, never writes buf[1]
}

TEST(PatchBranchBailsOnAnUnencodableUnconditionalDelta)
{
    // F5: patchBranch used to mask an out-of-range delta into whatever the
    // low bits happened to be, silently retargeting the branch, instead of
    // rejecting it.
    uint16_t buf[1];
    Assembler a(buf, 1);
    uint32_t site = a.placeholderBranch(); // site 0, pc now 2
    MOCK(runtime)::EXPECT(runtimeBail).withParam(RESOURCE_ERROR_CODE);
    EXPECT_RESOURCE_ERROR(a.patchBranch(site, 3000)); // delta 2996 > Ioff<1,11>::maxValue
}

TEST(PatchBranchBailsOnAnUnencodableConditionalDelta)
{
    uint16_t buf[1];
    Assembler a(buf, 1);
    uint32_t site = a.placeholderCondBranch(ArmV6M::Condition::EQ); // site 0, pc now 2
    MOCK(runtime)::EXPECT(runtimeBail).withParam(RESOURCE_ERROR_CODE);
    EXPECT_RESOURCE_ERROR(a.patchBranch(site, 400)); // delta 396 > Ioff<1,8>::maxValue
}

TEST(PatchBranchAcceptsTheMaxEncodableUnconditionalDelta)
{
    uint16_t buf[1];
    Assembler a(buf, 1);
    uint32_t site = a.placeholderBranch();
    a.patchBranch(site, site + 4 + 2046); // exactly Ioff<1,11>::maxValue -- must not fail()
    uint16_t rawOff;
    CHECK(ArmV6M::getBranchOffset(buf[0], rawOff));
    CHECK(ArmV6M::signExtend(rawOff, 11) << 1 == 2046);
}
