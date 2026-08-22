// Expected halfwords below are cross-checked against arm-none-eabi-as (a
// tool independent of both this port and its TS original), not re-derived
// from the same formulas under test.
#include "Test.h"
#include "emitter.h"
#include "binops.h"

using namespace jitc;

TEST(ClassifyBinOp)
{
    CHECK(classifyBinOp(Op::ADD, Combo::REG_ACC) == BinOpKind::AddSubRsub);
    CHECK(classifyBinOp(Op::SUB, Combo::PEEK_PEEK) == BinOpKind::AddSubRsub);
    CHECK(classifyBinOp(Op::RSUB, Combo::IMM_ACC) == BinOpKind::AddSubRsub);
    CHECK(classifyBinOp(Op::SHL, Combo::IMM_ACC) == BinOpKind::ShiftImm);
    CHECK(classifyBinOp(Op::SHR, Combo::IMM_ACC) == BinOpKind::ShiftImm);
    CHECK(classifyBinOp(Op::ASR, Combo::IMM_ACC) == BinOpKind::ShiftImm);
    CHECK(classifyBinOp(Op::SHL, Combo::REG_ACC) == BinOpKind::TwoOpInPlace); // register-count shift
    CHECK(classifyBinOp(Op::AND, Combo::REG_ACC) == BinOpKind::TwoOpInPlace);
    CHECK(classifyBinOp(Op::MUL, Combo::REG_ACC) == BinOpKind::TwoOpInPlace);
}

TEST(AddRegPlusReg)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofReg(2);
    emitBinaryOp(e, Op::ADD, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x1888); // ADDS r0, r1, r2
}

TEST(RsubRegMinusReg)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofReg(3);
    emitBinaryOp(e, Op::RSUB, Combo::REG_ACC, acc, &operand, 0); // dest = operand - acc = r3 - r1
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x1A58); // SUBS r0, r3, r1
}

TEST(RsubZeroImmDegeneratesToNeg)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1); // acc currently in r1
    Shape operand = Shape::ofImm(0); // RSUB #0 -> negation
    emitBinaryOp(e, Op::RSUB, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x4248); // NEGS r0, r1
}

TEST(AddImmFitsImm3)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(5);
    emitBinaryOp(e, Op::ADD, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x1D48); // ADDS r0, r1, #5
}

TEST(SubImmFitsImm8OnlyWhenDestEqualsN)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(0); // dest == n == ACC_REG
    Shape operand = Shape::ofImm(200); // doesn't fit imm3, fits imm8
    emitBinaryOp(e, Op::SUB, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x38C8); // SUBS r0, #200
}

TEST(ShiftImm)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(3);
    emitBinaryOp(e, Op::SHL, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x00C8); // LSLS r0, r1, #3

    Emitter e2(buf, 4);
    emitBinaryOp(e2, Op::SHR, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(buf[0] == 0x08C8); // LSRS r0, r1, #3

    Emitter e3(buf, 4);
    emitBinaryOp(e3, Op::ASR, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(buf[0] == 0x10C8); // ASRS r0, r1, #3
}

TEST(TwoOpInPlaceMaterializesAccFirstAndMovesOutIfDestDiffers)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1); // not ACC_REG — must be materialized into ACC_REG first
    Shape operand = Shape::ofReg(5);
    emitBinaryOp(e, Op::AND, Combo::REG_ACC, acc, &operand, 4); // dest=4, != ACC_REG
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == 0x4608); // MOV r0, r1  (materialize acc into ACC_REG)
    CHECK(buf[1] == 0x4028); // ANDS r0, r5
    CHECK(buf[2] == 0x4604); // MOV r4, r0  (move result out to dest)
}

TEST(TwoOpInPlaceSkipsMoveOutWhenDestIsAccReg)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(0); // already ACC_REG — no materialize move either
    Shape operand = Shape::ofReg(5);
    emitBinaryOp(e, Op::AND, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x4028); // ANDS r0, r5
}
