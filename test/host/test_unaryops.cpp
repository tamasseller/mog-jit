// This file checks the emitted instruction shape, not end-to-end
// correctness (covered on real QEMU, test/qemu/fixtures.cpp): NEG/NOT are
// single instructions; CLZ/REVBITS emit the same
// MOV/LDR/BLX-through-helper-vector sequence abi_strategy.cpp's own
// call/return sites use — their actual implementations are hand-written
// assembly in runtime.S, not something this level could unit-test
// directly.
#include "Test.h"
#include "unaryops.h"
#include "emitter.h"
#include "registers.h"
#include "armv6.h"

using namespace jitc;

namespace
{
using R = ArmV6M::LoReg;
}

TEST(negEmitsSingleInstruction)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    emitUnary(e, Op::NEG, ACC_REG);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::negs(ArmV6M::LoReg(ACC_REG), ArmV6M::LoReg(ACC_REG)));
}

TEST(notEmitsSingleInstructionAndMovesOutWhenDestDiffers)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    emitUnary(e, Op::NOT, 5);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::mvns(ArmV6M::LoReg(5), ArmV6M::LoReg(ACC_REG)));
}

TEST(clzEmitsHelperVectorCallSequence)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitUnary(e, Op::CLZ, ACC_REG);
    CHECK(e.halfwordCount() == 3); // MOV r3,r10 / LDR r3,[r3,#16] / BLX r3 — no trailing MOV, dest is already ACC_REG
    CHECK(buf[0] == ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    CHECK(buf[1] == ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(16))); // clzHelper, index 4
    CHECK(buf[2] == ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
}

TEST(revbitsEmitsHelperVectorCallSequenceAndMovesOutWhenDestDiffers)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    emitUnary(e, Op::REVBITS, 6);
    CHECK(e.halfwordCount() == 4); // MOV + LDR + BLX + trailing MOV out
    CHECK(buf[1] == ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(20))); // revbitsHelper, index 5
    CHECK(buf[2] == ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
    CHECK(buf[3] == ArmV6M::mov(ArmV6M::AnyReg(6), ArmV6M::AnyReg(ACC_REG)));
}
