// Sanity-checks ArmV6M's instruction encoders against expected 16-bit Thumb
// patterns computed independently (by hand, from the ARMv6-M encoding
// tables) rather than re-derived from the same formulas under test.
//
// A lo-lo register move has no dedicated Thumb-1 opcode — it's encoded as
// the "LSLS Rd, Rm, #0" idiom, which is why lsls() rather than a
// nonexistent movs(reg, reg) appears below.

#include "Test.h"
#include "armv6.h"

using R = ArmV6M::LoReg;

TEST(LoLoRegisterMoveIsLslsByZero)
{
    // MOVS r4, r3
    CHECK(ArmV6M::lsls(R(4), R(3), ArmV6M::Imm<5>(0)) == 0x001C);
    // MOVS r5, r3
    CHECK(ArmV6M::lsls(R(5), R(3), ArmV6M::Imm<5>(0)) == 0x001D);
}

TEST(MovImmediate)
{
    // MOVS r3, #1
    CHECK(ArmV6M::movs(R(3), ArmV6M::Imm<8>(1)) == 0x2301);
}

TEST(CmpImmediate)
{
    // CMP r3, #0x80
    CHECK(ArmV6M::cmp(R(3), ArmV6M::Imm<8>(0x80)) == 0x2B80);
}

TEST(ShiftRightImmediate)
{
    // LSRS r3, r3, #7
    CHECK(ArmV6M::lsrs(R(3), R(3), ArmV6M::Imm<5>(7)) == 0x09DB);
}

TEST(AddRegister)
{
    // ADDS r3, r3, r5
    CHECK(ArmV6M::adds(R(3), R(3), R(5)) == 0x195B);
}

TEST(ConditionalBranchZeroOffset)
{
    // BLO <next instruction> (offset field 0)
    CHECK(ArmV6M::blo(ArmV6M::Ioff<1, 8>(0)) == 0xD300);
}

TEST(UnconditionalBranchZeroOffset)
{
    // B <next instruction> (offset field 0)
    CHECK(ArmV6M::b(ArmV6M::Ioff<1, 11>(0)) == 0xE000);
}

TEST(GetBranchOffsetRejectsANonBranchInstruction)
{
    // getBranchOffset's bool return is load-bearing — emitter.h's
    // readBranchTarget branches on it — so the false path (a non-branch
    // halfword) needs coverage too.
    uint16_t rawOff;
    CHECK(!ArmV6M::getBranchOffset(ArmV6M::movs(R(3), ArmV6M::Imm<8>(1)), rawOff));
}

TEST(IsCondBranchAcceptsLeCondition)
{
    // Regression guard: isCondBranch must accept Condition::LE (0b1101),
    // the largest valid condition value — excluding it made patchBranch()
    // mis-treat an LE-conditioned branch as unconditional.
    CHECK(ArmV6M::isCondBranch(ArmV6M::condBranch(ArmV6M::Condition::LE, ArmV6M::Ioff<1, 8>(0))));
}

TEST(BranchExchangeR0)
{
    // BX r0
    CHECK(ArmV6M::bx(ArmV6M::AnyReg(0)) == 0x4700);
}
