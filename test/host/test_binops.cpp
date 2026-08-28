// Expected halfwords below are cross-checked against arm-none-eabi-as, a
// tool independent of the encoding logic under test.
#include "Test.h"
#include "assembler.h"
#include "binops.h"
#include "registers.h"
#include "armv6.h"

using namespace jitc;

TEST(AddRegPlusReg)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofReg(2);
    emitBinaryOp(e, Op::ADD, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(1), ArmV6M::LoReg(2))); // ADDS r0, r1, r2
}

TEST(SubRegMinusReg)
{
    // Both operands are plain registers — SUB's simplest case, never
    // exercised by the other SUB tests here (they all have at least one
    // side be a compile-time immediate).
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofReg(3);
    emitBinaryOp(e, Op::SUB, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::subs(ArmV6M::LoReg(0), ArmV6M::LoReg(1), ArmV6M::LoReg(3))); // SUBS r0, r1, r3
}

TEST(RsubRegMinusReg)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofReg(3);
    emitBinaryOp(e, Op::RSUB, Combo::REG_ACC, acc, operand, 0); // dest = operand - acc = r3 - r1
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::subs(ArmV6M::LoReg(0), ArmV6M::LoReg(3), ArmV6M::LoReg(1))); // SUBS r0, r3, r1
}

TEST(RsubZeroImmDegeneratesToNeg)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1); // acc currently in r1
    Shape operand = Shape::ofImm(0); // RSUB #0 -> negation
    emitBinaryOp(e, Op::RSUB, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::negs(ArmV6M::LoReg(0), ArmV6M::LoReg(1))); // NEGS r0, r1
}

TEST(AddImmFitsImm3)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(5);
    emitBinaryOp(e, Op::ADD, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(1), ArmV6M::Imm<3>(5))); // ADDS r0, r1, #5
}

TEST(SubImmFitsImm8OnlyWhenDestEqualsN)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(0); // dest == n == ACC_REG
    Shape operand = Shape::ofImm(200); // doesn't fit imm3, fits imm8
    emitBinaryOp(e, Op::SUB, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::subs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(200))); // SUBS r0, #200
}

TEST(ShiftImm)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(3);
    emitBinaryOp(e, Op::SHL, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::lsls(ArmV6M::LoReg(0), ArmV6M::LoReg(1), ArmV6M::Imm<5>(3))); // LSLS r0, r1, #3

    Assembler e2(buf, 4);
    emitBinaryOp(e2, Op::SHR, Combo::IMM_ACC, acc, operand, 0);
    CHECK(buf[0] == ArmV6M::lsrs(ArmV6M::LoReg(0), ArmV6M::LoReg(1), ArmV6M::Imm<5>(3))); // LSRS r0, r1, #3

    Assembler e3(buf, 4);
    emitBinaryOp(e3, Op::ASR, Combo::IMM_ACC, acc, operand, 0);
    CHECK(buf[0] == ArmV6M::asrs(ArmV6M::LoReg(0), ArmV6M::LoReg(1), ArmV6M::Imm<5>(3))); // ASRS r0, r1, #3
}

TEST(ShiftImmMaterializesAPendingImmediateAccumulatorFirst)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofImm(5);
    Shape operand = Shape::ofImm(3);
    emitBinaryOp(e, Op::SHL, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(40))); // MOVS r2, #5   (materialize acc into SCRATCH_REG)
}

TEST(TwoOpInPlaceMaterializesAccFirstAndMovesOutIfDestDiffers)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1); // not ACC_REG — must be materialized into ACC_REG first
    Shape operand = Shape::ofReg(5);
    emitBinaryOp(e, Op::AND, Combo::REG_ACC, acc, operand, 4); // dest=4, != ACC_REG
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == ArmV6M::mov(ArmV6M::AnyReg(4), ArmV6M::AnyReg(1))); // MOV r0, r1  (materialize acc into ACC_REG)
    CHECK(buf[1] == ArmV6M::ands(ArmV6M::LoReg(4), ArmV6M::LoReg(5))); // ANDS r0, r5
}

TEST(TwoOpInPlaceSkipsMoveOutWhenDestIsAccReg)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(0); // already ACC_REG — no materialize move either
    Shape operand = Shape::ofReg(5);
    emitBinaryOp(e, Op::AND, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::ands(ArmV6M::LoReg(0), ArmV6M::LoReg(5))); // ANDS r0, r5
}

TEST(AddImmFitsImm8OnlyWhenDestEqualsN)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(0); // dest == n == ACC_REG
    Shape operand = Shape::ofImm(200); // doesn't fit imm3, fits imm8
    emitBinaryOp(e, Op::ADD, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::Imm<8>(200))); // ADDS r0, #200
}

TEST(AddImmFallsBackToMaterializeWhenDestDiffersAndImmTooLarge)
{
    // n(=1) != dest(=0), k=10 doesn't fit imm3 — the fitsImm8&&dest==n
    // fast path doesn't apply either since dest != n, so this must fall
    // all the way through to addOrSubWithImm's materialize-into-
    // SCRATCH_REG tail.
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(10);
    emitBinaryOp(e, Op::ADD, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(10))); // MOVS r2, #10
    CHECK(buf[1] == ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(1), ArmV6M::LoReg(2))); // ADDS r0, r1, r2
}

TEST(AddRegAccWithOversizedImmediateOperandSkipsTheDestEqualsNCheck)
{
    // accShape is a plain register (never SCRATCH_REG), operand is an
    // immediate too large for imm3 *or* imm8 — fitsImm8(k) itself is
    // false here, short-circuiting past the dest==n check entirely
    // (distinct from AddImmFallsBackToMaterializeWhenDestDiffersAndImmTooLarge
    // above, where fitsImm8 was true and dest!=n was what failed). 1000 =
    // 125 << 3, so materializeImm32's shift-trick synthesizes it in 2
    // halfwords.
    uint16_t buf[8];
    Assembler e(buf, 8);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(1000);
    emitBinaryOp(e, Op::ADD, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(125))); // MOVS r2, #0x7D (125)
    CHECK(buf[1] == ArmV6M::lsls(ArmV6M::LoReg(2), ArmV6M::LoReg(2), ArmV6M::Imm<5>(3))); // LSLS r2, r2, #3   (r2 = 125 << 3 = 1000)
    CHECK(buf[2] == ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(1), ArmV6M::LoReg(2))); // ADDS r0, r1, r2
}

TEST(AddPeekPeekUsesDestAsRhsForOrdinaryArithmeticToo)
{
    // The PEEK_PEEK idiom applies to every AddSubRsub op, not just the
    // TwoOpInPlace ops (AND/OR/etc) the other PEEK_PEEK tests here
    // exercise — rhs==Shape::ofReg(dest) reaches emitAddSubRsub itself,
    // not just emitBinaryOp's TwoOpInPlace branch.
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1);
    emitBinaryOp(e, Op::ADD, Combo::PEEK_PEEK, acc, Shape::ofReg(5), 5);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::adds(ArmV6M::LoReg(5), ArmV6M::LoReg(1), ArmV6M::LoReg(5))); // ADDS r5, r1, r5
}

TEST(AddAccImmRhsRegFoldsIntoOrdinaryRegPlusImm)
{
    // acc pending-imm(5), rhs reg(2): dest = rhs.reg + acc.imm — the
    // ADD/SUB row's register-plus-immediate fold, operands swapped.
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofImm(5);
    Shape operand = Shape::ofReg(2);
    emitBinaryOp(e, Op::ADD, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(2), ArmV6M::Imm<3>(5))); // ADDS r0, r2, #5
}

TEST(AddBothImmMaterializesAccIntoDestNotScratchToAvoidAliasingWithK)
{
    // Regression guard: accShape and operand both compile-time immediates
    // (e.g. CONST(3) directly followed by an IMM_ACC ADD), reachable
    // whenever acc still holds a pending CONST value. Materializing
    // accShape into SCRATCH_REG and then letting addOrSubWithImm's
    // fallback materialize k into that same register would clobber
    // accShape and compute `k op k` instead of `accShape op k` — accShape
    // must materialize into `dest` itself instead, which is never
    // SCRATCH_REG for this call shape.
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofImm(3);
    Shape operand = Shape::ofImm(4);
    emitBinaryOp(e, Op::ADD, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(7))); 
}

TEST(SubRegAccWithOversizedImmediateOperandSkipsTheDestEqualsNCheck)
{
    // SUB's mirror of AddRegAccWithOversizedImmediateOperand... above —
    // accShape is a register, operand an immediate too large for
    // imm3/imm8, so addOrSubWithImm's fitsImm8(k) check short-circuits
    // false on its own. 1000 = 125 << 3, so materializeImm32's
    // shift-trick synthesizes it in 2 halfwords.
    uint16_t buf[8];
    Assembler e(buf, 8);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(1000);
    emitBinaryOp(e, Op::SUB, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(125))); // MOVS r2, #0x7D (125)
    CHECK(buf[1] == ArmV6M::lsls(ArmV6M::LoReg(2), ArmV6M::LoReg(2), ArmV6M::Imm<5>(3))); // LSLS r2, r2, #3   (r2 = 125 << 3 = 1000)
    CHECK(buf[2] == ArmV6M::subs(ArmV6M::LoReg(0), ArmV6M::LoReg(1), ArmV6M::LoReg(2))); // SUBS r0, r1, r2
}

TEST(SubAccImmRhsRegUsesRsubImmAsLeftWithNonzeroK)
{
    // acc pending-imm(20), rhs reg(3): dest = acc.imm - rhs.reg, k != 0,
    // so emitRsubImmAsLeft's own materialize path (not the k==0/NEG
    // shortcut the other RSUB-zero test already covers).
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofImm(20);
    Shape operand = Shape::ofReg(3);
    emitBinaryOp(e, Op::SUB, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(20))); // MOVS r2, #20
    CHECK(buf[1] == ArmV6M::subs(ArmV6M::LoReg(0), ArmV6M::LoReg(2), ArmV6M::LoReg(3))); // SUBS r0, r2, r3  (= 20 - 3 = 17)
}

TEST(SubBothImmMaterializesAccIntoDest)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofImm(10);
    Shape operand = Shape::ofImm(3);
    emitBinaryOp(e, Op::SUB, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(7))); // MOVS r0, #10
}

TEST(RsubAccImmRhsRegFoldsIntoOrdinaryRegMinusImm)
{
    // RSUB is rhs - acc; acc pending-imm(4), rhs reg(3): dest = rhs.reg -
    // acc.imm, an ordinary register-minus-immediate, not the
    // immediate-as-left-operand case.
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofImm(4);
    Shape operand = Shape::ofReg(3);
    emitBinaryOp(e, Op::RSUB, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::subs(ArmV6M::LoReg(0), ArmV6M::LoReg(3), ArmV6M::Imm<3>(4))); // SUBS r0, r3, #4  (= 3 - 4 = -1)
}

TEST(RsubRhsImmAccRegUsesRsubImmAsLeftWithNonzeroK)
{
    // rhs pending-imm(7), acc reg(5) — a plausible window register, never
    // SCRATCH_REG itself (registers.h's invariant: accShape's register,
    // whenever "clean", is always ACC_REG or a window register —
    // physReg() never returns SCRATCH_REG=2): dest = rhs.imm - acc.reg,
    // k != 0.
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(5);
    Shape operand = Shape::ofImm(7);
    emitBinaryOp(e, Op::RSUB, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(7))); // MOVS r2, #7
    CHECK(buf[1] == ArmV6M::subs(ArmV6M::LoReg(0), ArmV6M::LoReg(2), ArmV6M::LoReg(5))); // SUBS r0, r2, r5  (= 7 - acc)
}

TEST(RsubBothImmMaterializesAccIntoDestNotScratchToAvoidAliasingWithK)
{
    // Same aliasing hazard as AddBothImm..., mirrored for RSUB (rhs - acc
    // instead of acc + k): dest = 2 - 9 = -7, not 0.
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofImm(9);
    Shape operand = Shape::ofImm(2);
    emitBinaryOp(e, Op::RSUB, Combo::IMM_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(6))); // MOVS r0, #11    (acc materialized into dest)
    CHECK(buf[1] == ArmV6M::mvns(ArmV6M::LoReg(0), ArmV6M::LoReg(0))); // MOVS r0, #11    (acc materialized into dest)
}

TEST(AddAccImmRhsScratchRegAvoidsClobberingReloadedOperand)
{
    // Same aliasing bug class as the BothImm tests above, but here
    // accShape is imm(100) and the *operand* register happens to be
    // SCRATCH_REG itself — exactly what happens when an out-of-window
    // local gets reloaded via ldrSp(SCRATCH_REG, ...) in
    // translate_proc.cpp. Materializing k into SCRATCH_REG right after
    // would clobber that just-reloaded value, so addOrSubWithImm
    // materializes k into ENTRY_JUMP_REG instead whenever n == SCRATCH_REG
    // — a register never live across bytecode instructions, so always
    // free to borrow, unlike dest (which can itself alias SCRATCH_REG via
    // the same reload path).
    uint16_t buf[8];
    Assembler e(buf, 8);
    Shape acc = Shape::ofImm(100);
    Shape operand = Shape::ofReg(SCRATCH_REG);
    emitBinaryOp(e, Op::ADD, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(3), ArmV6M::Imm<8>(100))); // MOVS r3, #100
    CHECK(buf[1] == ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(2), ArmV6M::LoReg(3))); // ADDS r0, r2, r3  (= reloaded value + 100)
}

TEST(SubAccImmRhsScratchRegAvoidsClobberingReloadedOperand)
{
    uint16_t buf[8];
    Assembler e(buf, 8);
    Shape acc = Shape::ofImm(100);
    Shape operand = Shape::ofReg(SCRATCH_REG);
    emitBinaryOp(e, Op::SUB, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(3), ArmV6M::Imm<8>(100))); // MOVS r3, #100
    CHECK(buf[1] == ArmV6M::subs(ArmV6M::LoReg(0), ArmV6M::LoReg(3), ArmV6M::LoReg(2))); // SUBS r0, r3, r2  (= 100 - reloaded value)
}

TEST(RsubAccImmRhsScratchRegAvoidsClobberingReloadedOperand)
{
    uint16_t buf[8];
    Assembler e(buf, 8);
    Shape acc = Shape::ofImm(50);
    Shape operand = Shape::ofReg(SCRATCH_REG);
    emitBinaryOp(e, Op::RSUB, Combo::REG_ACC, acc, operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(3), ArmV6M::Imm<8>(50))); // MOVS r3, #50
    CHECK(buf[1] == ArmV6M::subs(ArmV6M::LoReg(0), ArmV6M::LoReg(2), ArmV6M::LoReg(3))); // SUBS r0, r2, r3  (= reloaded value - 50)
}

TEST(AddAccImmRhsScratchRegDestAlsoScratchRegDoesNotAssert)
{
    // The bug this regression-tests: dest can alias SCRATCH_REG too (the
    // not-in-window REG_REG path in translate_proc.cpp stores its result
    // back through SCRATCH_REG), and when it does at the same time n ==
    // SCRATCH_REG, the old "copy n out to dest" trick had no register left
    // to hold k at all (dest == n == SCRATCH_REG). ENTRY_JUMP_REG sidesteps
    // this since it's never aliased by dest or n. k = 1000 doesn't fit the
    // imm8 increment-in-place shortcut (dest == n alone isn't enough to
    // take that path), so this reaches the materialize-a-temporary branch.
    uint16_t buf[8];
    Assembler e(buf, 8);
    Shape acc = Shape::ofImm(1000);
    Shape operand = Shape::ofReg(SCRATCH_REG);
    emitBinaryOp(e, Op::ADD, Combo::REG_ACC, acc, operand, SCRATCH_REG);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(3), ArmV6M::Imm<8>(125)));               // MOVS r3, #125
    CHECK(buf[1] == ArmV6M::lsls(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Imm<5>(3))); // LSLS r3, r3, #3      (r3 = 1000)
    CHECK(buf[2] == ArmV6M::adds(ArmV6M::LoReg(2), ArmV6M::LoReg(2), ArmV6M::LoReg(3))); // ADDS r2, r2, r3       (= reloaded value + 1000)
}

TEST(TwoOpInPlaceNativeCoversEveryOpcode)
{
    // AND is exercised by the tests above; OR/XOR/MUL and the three
    // register-count shift forms share the same TwoOpInPlace dispatch but
    // weren't exercised through the native-opcode switch yet.
    //
    // The shifts belong here — one instruction each, like the rest. They
    // briefly did not: masking the amount to five bits made each of them
    // three, until isa-core.md §4.1 stopped defining a shift by 32 or more
    // and there was nothing left to mask for (fuzzing-campaign.md
    // finding 5).
    struct
    {
        Op op;
        uint16_t expected;
    }
    cases[] = {
        {Op::OR,  ArmV6M::orrs(ArmV6M::LoReg(0), ArmV6M::LoReg(5))}, // ORR r0, r5
        {Op::XOR, ArmV6M::eors(ArmV6M::LoReg(0), ArmV6M::LoReg(5))}, // EOR r0, r5
        {Op::MUL, ArmV6M::muls(ArmV6M::LoReg(0), ArmV6M::LoReg(5))}, // MUL r0, r5
        {Op::SHL, ArmV6M::lsls(ArmV6M::LoReg(0), ArmV6M::LoReg(5))}, // LSL r0, r5
        {Op::SHR, ArmV6M::lsrs(ArmV6M::LoReg(0), ArmV6M::LoReg(5))}, // LSR r0, r5
        {Op::ASR, ArmV6M::asrs(ArmV6M::LoReg(0), ArmV6M::LoReg(5))}, // ASR r0, r5
    };
    for(auto &c : cases)
    {
        uint16_t buf[4];
        Assembler e(buf, 4);
        Shape acc = Shape::ofReg(0);
        Shape operand = Shape::ofReg(5);
        emitBinaryOp(e, c.op, Combo::REG_ACC, acc, operand, 0);
        CHECK(e.halfwordCount() == 1);
        CHECK(buf[0] == c.expected);
    }
}

TEST(TwoOpInPlacePeekPeekUsesDestAsRhs)
{
    // PEEK_PEEK's right-hand operand is dest itself (rhs == Shape::ofReg(dest))
    // — dest is a window register here (r5, mirroring window.topReg()),
    // safe to read as Rm before it's overwritten as Rdn's move-out
    // target.
    uint16_t buf[4];
    Assembler e(buf, 4);
    Shape acc = Shape::ofReg(1); // not ACC_REG — must be materialized first
    emitBinaryOp(e, Op::AND, Combo::PEEK_PEEK, acc, Shape::ofReg(5), 5);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::ands(ArmV6M::LoReg(5), ArmV6M::LoReg(1))); // ANDS r0, r5  (r5 == dest itself, PEEK_PEEK's own operand)
}
