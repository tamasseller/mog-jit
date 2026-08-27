// Unit tests for blocks.h's branch/table bookkeeping (openBrTable/
// closeBlockEnd etc). End-to-end LOOP/BR_TABLE/comparison-as-value
// behavior is covered on real QEMU instead (test/qemu/fixtures.cpp) —
// this file targets bookkeeping that's awkward to observe from the
// outside: a mis-patched branch is invisible in a passing *value* unless
// the test happens to exercise the exact path that would expose it.
#include "Test.h"
#include "blocks.h"
#include "assembler.h"
#include "window.h"
#include "accstate.h"
#include "encode_instr.h"
#include "registers.h"
#include "armv6.h"

using namespace jitc;
using Cond = ArmV6M::Condition;

static uint32_t encode(const Instr *body, uint32_t count, uint8_t *out, uint32_t cap)
{
    return encodeBody(body, count, out, cap);
}

TEST(EmitGuardedBranchUsesShortFormWhenSpanFitsInRange)
{
    const Instr body[] = {bare(Op::RETURN)}; // 16 bytes <= SAFE_COND_BRANCH_SPAN(240)
    uint8_t bytes[8];
    uint32_t len = encode(body, 1, bytes, sizeof(bytes));

    uint16_t buf[8];
    Assembler e(buf, 8);
    Label label;
    emitGuardedBranch(e, label, Cond::EQ, bytes, len, 0, 1);
    CHECK(e.halfwordCount() == 1); // bare conditional branch, no long form
    CHECK(label.chain == 0);
    CHECK(ArmV6M::isCondBranch(buf[0]));
}

TEST(EmitGuardedBranchUsesLongFormWhenSpanExceedsRange)
{
    // 15 ordinary NOT instructions + a RETURN closer = 16 * 16 = 256 bytes
    // > 240 — forces the invert-and-long-branch idiom.
    Instr body[16];
    for(int i = 0; i < 15; i++)
    {
        body[i] = bare(Op::NOT);
    }
    body[15] = bare(Op::RETURN);
    uint8_t bytes[32];
    uint32_t len = encode(body, 16, bytes, sizeof(bytes));

    uint16_t buf[8];
    Assembler e(buf, 8);
    Label label;
    emitGuardedBranch(e, label, Cond::EQ, bytes, len, 0, 1);
    CHECK(e.halfwordCount() == 2); // condBranch(inverse) + long b
    CHECK(ArmV6M::isCondBranch(buf[0]));
    uint16_t rawOff;
    CHECK(ArmV6M::getCondBranchOffset(buf[0], rawOff));
    CHECK(ArmV6M::getBranchCondtion(buf[0]) == Cond::NE); // inverse(EQ)
    // "not taken" (condition true) falls through to the long branch right
    // after it — a zero-distance skip, encoded as offset 0.
    CHECK(rawOff == 0);
    CHECK(label.chain == 2); // the long b's own site, right after the guard
}

TEST(MaxSpanBytesRecursesThroughANestedLoop)
{
    // The span being measured contains a whole LOOP construct (cond +
    // body sub-blocks) before reaching the closing terminator —
    // maxSpanBytes must recurse into it instead of mis-measuring past it,
    // to produce a correct bound for whatever guard is sized around this
    // whole span.
    const Instr program[] = {
        bare(Op::LOOP), bare(Op::BLOCK_END),      // empty cond
            CONST(1), bare(Op::BLOCK_END),        // body, closes normally
        bare(Op::RETURN),
    };
    uint8_t bytes[16];
    uint32_t len = encode(program, 5, bytes, sizeof(bytes));
    SpanResult r = maxSpanBytes(bytes, len, 0, 1);
    // LOOP + cond-BLOCK_END + CONST + body-BLOCK_END + RETURN, each
    // instrMaxBytes' ORDINARY_MAX_BYTES(16) — nothing here is a CALL or a
    // BR_TABLE N>2 jump table.
    CHECK(r.bytes == 5 * 16);
    CHECK(r.nextPc == len);
}

TEST(MaxSpanBytesRecursesThroughANestedBrTable)
{
    // Same shape, one level of BR_TABLE(2) instead of LOOP — blocks.cpp's
    // separate nested-BR_TABLE branch in maxSpanBytes, distinct from the
    // nested-LOOP one above.
    const Instr program[] = {
        brTable(2),
            CONST(1), bare(Op::BLOCK_END),
            CONST(2), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint8_t bytes[16];
    uint32_t len = encode(program, 6, bytes, sizeof(bytes));
    SpanResult r = maxSpanBytes(bytes, len, 0, 1);
    CHECK(r.bytes == 6 * 16);
    CHECK(r.nextPc == len);
}

TEST(MaxSpanBytesAccountsForANestedJumpTablesOwnOverhead)
{
    // The span being measured contains a nested BR_TABLE with N>2 — a
    // genuine jump table, not an if/else fusion — instrMaxBytes'
    // BR_TABLE_JUMP_OVERHEAD_BYTES branch (distinct from the "N<=2,
    // ordinary" case every other BR_TABLE-in-a-span test here exercises).
    const Instr body[] = {
        brTable(3),
            bare(Op::NOT), bare(Op::BLOCK_END),
            bare(Op::NOT), bare(Op::BLOCK_END),
            bare(Op::NOT), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint8_t bytes[32];
    uint32_t len = encode(body, sizeof(body) / sizeof(body[0]), bytes, sizeof(bytes));
    SpanResult r = maxSpanBytes(bytes, len, 0, 1);
    // BR_TABLE(3)'s own overhead (32 + (3+1)*2 = 40) + 3 cases * (NOT +
    // BLOCK_END, 16 each) + the closing RETURN.
    CHECK(r.bytes == 40 + 3 * 32 + 16);
    CHECK(r.nextPc == len);
}

TEST(EmitComparisonDirectConditionForRegRegCmp)
{
    uint16_t buf[8];
    Assembler e(buf, 8);
    AccState accState;
    accState.setClean(1); // acc already in r1, not ACC_REG
    Shape operand = Shape::ofReg(5);
    Cond c = emitComparison(e, accState, Op::LT_S, operand);
    CHECK(c == Cond::LT);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::cmp(ArmV6M::LoReg(1), ArmV6M::LoReg(5))); // CMP r1, r5
}

TEST(EmitComparisonMirroredConditionWhenAccIsAFittingImmediate)
{
    // acc PENDING(imm), operand a register — the mirror-table
    // optimization: `operand CMP #acc` with the mirrored condition,
    // avoiding materializing acc into a register first.
    uint16_t buf[8];
    Assembler e(buf, 8);
    AccState accState;
    accState.producer(Shape::ofImm(10));
    Shape operand = Shape::ofReg(3);
    Cond c = emitComparison(e, accState, Op::LT_S, operand);
    CHECK(c == Cond::GT); // MIRRORED_CONDITION[LT_S] == GT
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::cmp(ArmV6M::LoReg(3), ArmV6M::Imm<8>(10))); // CMP r3, #10
}

TEST(EmitComparisonSkipsMirroredShortcutWhenPendingAccImmTooLargeForImm8)
{
    // acc PENDING(imm) *and* the operand is a register — the
    // mirrored-condition shortcut's precondition set, except
    // fitsImm8(left.imm) is false here, unlike
    // EmitComparisonMirroredConditionWhenAccIsAFittingImmediate's small
    // pending value. Falls through to the ordinary materialize-acc path,
    // direct condition preserved (not mirrored). 1000 = 125 << 3, so
    // materializeImm32's shift-trick synthesizes it in 2 halfwords.
    uint16_t buf[8];
    Assembler e(buf, 8);
    AccState accState;
    accState.producer(Shape::ofImm(1000));
    Shape operand = Shape::ofReg(3);
    Cond c = emitComparison(e, accState, Op::LT_S, operand);
    CHECK(c == Cond::LT);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(0x7D))); // MOVS r0, #0x7D (125)
    CHECK(buf[1] == ArmV6M::lsls(ArmV6M::LoReg(0), ArmV6M::LoReg(0), ArmV6M::Imm<5>(3))); // LSLS r0, r0, #3 (r0 = 125 << 3 = 1000)
    CHECK(buf[2] == ArmV6M::cmp(ArmV6M::LoReg(0), ArmV6M::LoReg(3))); // CMP r0, r3
}

TEST(EmitComparisonMaterializesLargeImmediateOperandIntoScratch)
{
    // operand is a compile-time immediate but doesn't fit CMP's imm8 form
    // (the fallback, distinct from the imm8-fits case
    // EmitComparisonDirectConditionForRegRegCmp's sibling tests exercise)
    // — materialize it into SCRATCH_REG first, direct condition preserved
    // (acc isn't itself an immediate here, so the mirrored-condition
    // shortcut above doesn't apply). 1000 = 125 << 3, so materializeImm32's
    // shift-trick synthesizes it in 2 halfwords.
    uint16_t buf[8];
    Assembler e(buf, 8);
    AccState accState;
    accState.setClean(1);
    Shape operand = Shape::ofImm(1000);
    Cond c = emitComparison(e, accState, Op::LT_S, operand);
    CHECK(c == Cond::LT);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(0x7D))); // MOVS r2, #0x7D (125)
    CHECK(buf[1] == ArmV6M::lsls(ArmV6M::LoReg(2), ArmV6M::LoReg(2), ArmV6M::Imm<5>(3))); // LSLS r2, r2, #3 (r2 = 125 << 3 = 1000)
    CHECK(buf[2] == ArmV6M::cmp(ArmV6M::LoReg(1), ArmV6M::LoReg(2))); // CMP r1, r2
}
