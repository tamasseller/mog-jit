// Expected halfwords below are cross-checked against arm-none-eabi-as, a
// tool independent of the encoding logic under test.
#include "Test.h"
#include "emitter.h"
#include "binops.h"
#include "registers.h"

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

TEST(SubRegMinusReg)
{
    // Both operands are plain registers — SUB's simplest case, never
    // exercised by the other SUB tests here (they all have at least one
    // side be a compile-time immediate).
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofReg(3);
    emitBinaryOp(e, Op::SUB, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x1AC8); // SUBS r0, r1, r3
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

TEST(ShiftImmMaterializesAPendingImmediateAccumulatorFirst)
{
    // accShape can be a compile-time immediate here too (e.g. a CONST
    // directly followed by an immediate-shift op) — shapeToReg's
    // materialize-into-SCRATCH_REG branch, which the other ShiftImm test
    // above never reaches (it always uses a register accShape).
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofImm(5);
    Shape operand = Shape::ofImm(3);
    emitBinaryOp(e, Op::SHL, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == 0x2205); // MOVS r2, #5   (materialize acc into SCRATCH_REG)
    CHECK(buf[1] == 0x00D0); // LSLS r0, r2, #3  (= 5 << 3 = 40)
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

TEST(AddImmFitsImm8OnlyWhenDestEqualsN)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(0); // dest == n == ACC_REG
    Shape operand = Shape::ofImm(200); // doesn't fit imm3, fits imm8
    emitBinaryOp(e, Op::ADD, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x30C8); // ADDS r0, #200
}

TEST(AddImmFallsBackToMaterializeWhenDestDiffersAndImmTooLarge)
{
    // n(=1) != dest(=0), k=10 doesn't fit imm3 — the fitsImm8&&dest==n
    // fast path doesn't apply either since dest != n, so this must fall
    // all the way through to addOrSubWithImm's materialize-into-
    // SCRATCH_REG tail.
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(10);
    emitBinaryOp(e, Op::ADD, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == 0x220A); // MOVS r2, #10
    CHECK(buf[1] == 0x1888); // ADDS r0, r1, r2
}

TEST(AddRegAccWithOversizedImmediateOperandSkipsTheDestEqualsNCheck)
{
    // accShape is a plain register (never SCRATCH_REG), operand is an
    // immediate too large for imm3 *or* imm8 — fitsImm8(k) itself is
    // false here, short-circuiting past the dest==n check entirely
    // (distinct from AddImmFallsBackToMaterializeWhenDestDiffersAndImmTooLarge
    // above, where fitsImm8 was true and dest!=n was what failed).
    uint16_t buf[8];
    Emitter e(buf, 8);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(1000);
    emitBinaryOp(e, Op::ADD, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 4);
    CHECK(buf[0] == 0x2203); // MOVS r2, #3
    CHECK(buf[1] == 0x0212); // LSLS r2, r2, #8
    CHECK(buf[2] == 0x32E8); // ADDS r2, #232      (r2 = 3*256+232 = 1000)
    CHECK(buf[3] == 0x1888); // ADDS r0, r1, r2
}

TEST(AddPeekPeekUsesDestAsRhsForOrdinaryArithmeticToo)
{
    // The PEEK_PEEK idiom applies to every AddSubRsub op, not just the
    // TwoOpInPlace ops (AND/OR/etc) the other PEEK_PEEK tests here
    // exercise — operand==nullptr reaches emitAddSubRsub itself, not just
    // emitBinaryOp's TwoOpInPlace branch.
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1);
    emitBinaryOp(e, Op::ADD, Combo::PEEK_PEEK, acc, nullptr, 5);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x194D); // ADDS r5, r1, r5
}

TEST(AddAccImmRhsRegFoldsIntoOrdinaryRegPlusImm)
{
    // acc pending-imm(5), rhs reg(2): dest = rhs.reg + acc.imm — the
    // ADD/SUB row's register-plus-immediate fold, operands swapped.
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofImm(5);
    Shape operand = Shape::ofReg(2);
    emitBinaryOp(e, Op::ADD, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x1D50); // ADDS r0, r2, #5
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
    Emitter e(buf, 4);
    Shape acc = Shape::ofImm(3);
    Shape operand = Shape::ofImm(4);
    emitBinaryOp(e, Op::ADD, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == 0x2003); // MOVS r0, #3
    CHECK(buf[1] == 0x1D00); // ADDS r0, r0, #4  (= 3 + 4 = 7)
}

TEST(SubRegAccWithOversizedImmediateOperandSkipsTheDestEqualsNCheck)
{
    // SUB's mirror of AddRegAccWithOversizedImmediateOperand... above —
    // accShape is a register, operand an immediate too large for
    // imm3/imm8, so addOrSubWithImm's fitsImm8(k) check short-circuits
    // false on its own.
    uint16_t buf[8];
    Emitter e(buf, 8);
    Shape acc = Shape::ofReg(1);
    Shape operand = Shape::ofImm(1000);
    emitBinaryOp(e, Op::SUB, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 4);
    CHECK(buf[0] == 0x2203); // MOVS r2, #3
    CHECK(buf[1] == 0x0212); // LSLS r2, r2, #8
    CHECK(buf[2] == 0x32E8); // ADDS r2, #232      (r2 = 1000)
    CHECK(buf[3] == 0x1A88); // SUBS r0, r1, r2
}

TEST(SubAccImmRhsRegUsesRsubImmAsLeftWithNonzeroK)
{
    // acc pending-imm(20), rhs reg(3): dest = acc.imm - rhs.reg, k != 0,
    // so emitRsubImmAsLeft's own materialize path (not the k==0/NEG
    // shortcut the other RSUB-zero test already covers).
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofImm(20);
    Shape operand = Shape::ofReg(3);
    emitBinaryOp(e, Op::SUB, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == 0x2214); // MOVS r2, #20
    CHECK(buf[1] == 0x1AD0); // SUBS r0, r2, r3  (= 20 - 3 = 17)
}

TEST(SubBothImmMaterializesAccIntoDest)
{
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofImm(10);
    Shape operand = Shape::ofImm(3);
    emitBinaryOp(e, Op::SUB, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == 0x200A); // MOVS r0, #10
    CHECK(buf[1] == 0x1EC0); // SUBS r0, r0, #3  (= 10 - 3 = 7)
}

TEST(RsubAccImmRhsRegFoldsIntoOrdinaryRegMinusImm)
{
    // RSUB is rhs - acc; acc pending-imm(4), rhs reg(3): dest = rhs.reg -
    // acc.imm, an ordinary register-minus-immediate, not the
    // immediate-as-left-operand case.
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofImm(4);
    Shape operand = Shape::ofReg(3);
    emitBinaryOp(e, Op::RSUB, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == 0x1F18); // SUBS r0, r3, #4  (= 3 - 4 = -1)
}

TEST(RsubRhsImmAccRegUsesRsubImmAsLeftWithNonzeroK)
{
    // rhs pending-imm(7), acc reg(5) — a plausible window register, never
    // SCRATCH_REG itself (registers.h's invariant: accShape's register,
    // whenever "clean", is always ACC_REG or a window register —
    // physReg() never returns SCRATCH_REG=2): dest = rhs.imm - acc.reg,
    // k != 0.
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(5);
    Shape operand = Shape::ofImm(7);
    emitBinaryOp(e, Op::RSUB, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 2);
    CHECK(buf[0] == 0x2207); // MOVS r2, #7
    CHECK(buf[1] == 0x1B50); // SUBS r0, r2, r5  (= 7 - acc)
}

TEST(RsubBothImmMaterializesAccIntoDestNotScratchToAvoidAliasingWithK)
{
    // Same aliasing hazard as AddBothImm..., mirrored for RSUB (rhs - acc
    // instead of acc + k): dest = 2 - 9 = -7, not 0.
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofImm(9);
    Shape operand = Shape::ofImm(2);
    emitBinaryOp(e, Op::RSUB, Combo::IMM_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == 0x2009); // MOVS r0, #9    (acc materialized into dest)
    CHECK(buf[1] == 0x2202); // MOVS r2, #2    (k materialized into SCRATCH_REG)
    CHECK(buf[2] == 0x1A10); // SUBS r0, r2, r0  (= 2 - 9 = -7)
}

TEST(AddAccImmRhsScratchRegAvoidsClobberingReloadedOperand)
{
    // Same aliasing bug class as the BothImm tests above, but here
    // accShape is imm(100) and the *operand* register happens to be
    // SCRATCH_REG itself — exactly what happens when an out-of-window
    // local gets reloaded via ldrSp(SCRATCH_REG, ...) in
    // translate_proc.cpp. Materializing k into SCRATCH_REG right after
    // would clobber that just-reloaded value, so addOrSubWithImm copies n
    // out to dest first whenever n == SCRATCH_REG.
    uint16_t buf[8];
    Emitter e(buf, 8);
    Shape acc = Shape::ofImm(100);
    Shape operand = Shape::ofReg(SCRATCH_REG);
    emitBinaryOp(e, Op::ADD, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == 0x4610); // MOV r0, r2   (save the reloaded operand out of SCRATCH_REG)
    CHECK(buf[1] == 0x2264); // MOVS r2, #100
    CHECK(buf[2] == 0x1880); // ADDS r0, r0, r2  (= reloaded value + 100)
}

TEST(SubAccImmRhsScratchRegAvoidsClobberingReloadedOperand)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    Shape acc = Shape::ofImm(100);
    Shape operand = Shape::ofReg(SCRATCH_REG);
    emitBinaryOp(e, Op::SUB, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == 0x4610); // MOV r0, r2
    CHECK(buf[1] == 0x2264); // MOVS r2, #100
    CHECK(buf[2] == 0x1A10); // SUBS r0, r2, r0  (= 100 - reloaded value)
}

TEST(RsubAccImmRhsScratchRegAvoidsClobberingReloadedOperand)
{
    uint16_t buf[8];
    Emitter e(buf, 8);
    Shape acc = Shape::ofImm(50);
    Shape operand = Shape::ofReg(SCRATCH_REG);
    emitBinaryOp(e, Op::RSUB, Combo::REG_ACC, acc, &operand, 0);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == 0x4610); // MOV r0, r2
    CHECK(buf[1] == 0x2232); // MOVS r2, #50
    CHECK(buf[2] == 0x1A80); // SUBS r0, r0, r2  (= reloaded value - 50)
}

TEST(TwoOpInPlaceNativeCoversEveryOpcode)
{
    // AND is exercised by the tests above; OR/XOR/MUL and the
    // register-count shift forms share the same TwoOpInPlace dispatch
    // but weren't exercised through the native-opcode switch yet.
    struct
    {
        Op op;
        uint16_t expected;
    }
    cases[] = {
        {Op::OR,  0x4328}, {Op::XOR, 0x4068}, {Op::MUL, 0x4368},
        {Op::SHL, 0x40A8}, {Op::SHR, 0x40E8}, {Op::ASR, 0x4128},
    };
    for(auto &c : cases)
    {
        uint16_t buf[4];
        Emitter e(buf, 4);
        Shape acc = Shape::ofReg(0);
        Shape operand = Shape::ofReg(5);
        emitBinaryOp(e, c.op, Combo::REG_ACC, acc, &operand, 0);
        CHECK(e.halfwordCount() == 1);
        CHECK(buf[0] == c.expected);
    }
}

TEST(TwoOpInPlacePeekPeekUsesDestAsRhs)
{
    // PEEK_PEEK's right-hand operand is dest itself (operand == nullptr)
    // — dest is a window register here (r5, mirroring window.topReg()),
    // safe to read as Rm before it's overwritten as Rdn's move-out
    // target.
    uint16_t buf[4];
    Emitter e(buf, 4);
    Shape acc = Shape::ofReg(1); // not ACC_REG — must be materialized first
    emitBinaryOp(e, Op::AND, Combo::PEEK_PEEK, acc, nullptr, 5);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == 0x4608); // MOV r0, r1  (materialize acc into ACC_REG)
    CHECK(buf[1] == 0x4028); // ANDS r0, r5  (r5 == dest itself, PEEK_PEEK's own operand)
    CHECK(buf[2] == 0x4605); // MOV r5, r0  (move result out to dest)
}
