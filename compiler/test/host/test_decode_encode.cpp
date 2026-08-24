// jit-armv6m/compiler/test — decode_instr.h/encode_instr.h round-trip.
// One instruction of every shape decode_instr.h claims to handle, encoded
// via encode_instr.h and decoded back — cross-checks the two against each
// other using hand-derived cases, since there's no reference
// implementation here to check against directly.
#include "Test.h"
#include "decode_instr.h"
#include "encode_instr.h"

#include <cstring>

using namespace jitc;

namespace
{

bool sameInstr(const Instr &a, const Instr &b)
{
    if(a.op != b.op || a.combo != b.combo)
    {
        return false; // GCOV_EXCL_LINE — only reached by a failing round trip
    }
    if(a.op == Op::CALL)
    {
        return a.calleeIndex == b.calleeIndex;
    }
    if(a.combo == Combo::REG_ACC || a.combo == Combo::REG_REG || a.op == Op::LOAD || a.op == Op::STORE)
    {
        return a.target == b.target;
    }
    if(a.combo == Combo::IMM_ACC || a.op == Op::CONST || a.op == Op::TRAP || a.op == Op::BR_TABLE)
    {
        return a.imm == b.imm;
    }
    return true;
}

const Instr kCases[] = {
    opReg(Op::ADD, 3), opRegWriteback(Op::SUB, 1), opStack(Op::MUL, Combo::PEEK_PEEK),
    opStack(Op::AND, Combo::POP_ACC), opImm(Op::XOR, 42),
    opReg(Op::EQ, 2), opStack(Op::LT_S, Combo::POP_ACC), opImm(Op::EQ, 0), opImm(Op::GE_U, 100),
    bare(Op::NEG), bare(Op::NOT), bare(Op::CLZ), bare(Op::REVBITS),
    bare(Op::BLOCK_END), bare(Op::LOOP), brTable(1), brTable(2), brTable(5),
    call(3), bare(Op::RETURN), trapInstr(0), trapInstr(7),
    PUSH(), POP(), LOAD(4), STORE(9), CONST(3), CONST(12345), CONST(-1),
};
constexpr uint32_t kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

} // namespace

TEST(DecodeInstrRoundTripsEveryShape)
{
    for(uint32_t i = 0; i < kCaseCount; i++)
    {
        uint8_t bytes[8];
        uint32_t len = 0;
        encodeInstr(kCases[i], bytes, len, sizeof(bytes));

        DecodedInstr d = decodeInstr(bytes, len, 0);
        CHECK(sameInstr(d.instr, kCases[i]));
        CHECK(d.next == len);
    }
}

TEST(DecodeInstrAdvancesThroughARunOfInstructions)
{
    const Instr program[] = {opImm(Op::ADD, 5), LOAD(0), bare(Op::RETURN)};
    uint8_t bytes[32];
    uint32_t len = encodeBody(program, 3, bytes, sizeof(bytes));

    uint32_t pos = 0;
    for(uint32_t i = 0; i < 3; i++)
    {
        DecodedInstr d = decodeInstr(bytes, len, pos);
        CHECK(sameInstr(d.instr, program[i]));
        pos = d.next;
    }
    CHECK(pos == len);
}

TEST(EncodeConstSmallImmFitsOneByte)
{
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeInstr(CONST(15), bytes, len, sizeof(bytes));
    CHECK(len == 1);
    CHECK(bytes[0] == 108 + 15);
}

TEST(EncodeConstLargeImmUsesLeb128Form)
{
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeInstr(CONST(16), bytes, len, sizeof(bytes));
    CHECK(len == 2);
    CHECK(bytes[0] == 107);
    CHECK(bytes[1] == 16);
}

TEST(EncodeComparisonZeroImmCollapsesToOneByte)
{
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeInstr(opImm(Op::EQ, 0), bytes, len, sizeof(bytes));
    CHECK(len == 1); // mode 2 ("IMM_ACC #0"), no operand byte
}

TEST(EncodeLeb128MultiByteRoundTrips)
{
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeLeb128(300, bytes, len, sizeof(bytes)); // 300 = 0b1_0010_1100 -> 2 LEB128 bytes
    CHECK(len == 2);

    uint32_t next;
    uint32_t value = decodeLeb128(bytes, 0, next);
    CHECK(value == 300);
    CHECK(next == 2);
}

// ── The decode table against isa-core.md §5.2 itself ────────────────────

namespace
{

// §5.2's own orderings, transcribed straight from the spec rather than
// from decode_instr.cpp, so a typo in that file's table can't hide by
// agreeing with itself.
const Op SPEC_ARITH[10] = {Op::ADD, Op::SUB, Op::RSUB, Op::MUL, Op::AND, Op::OR, Op::XOR, Op::SHL, Op::SHR, Op::ASR};
const Combo SPEC_ARITH_MODE[5] = {Combo::REG_ACC, Combo::REG_REG, Combo::PEEK_PEEK, Combo::POP_ACC, Combo::IMM_ACC};
const Op SPEC_CMP[10] = {Op::EQ, Op::NE, Op::LT_S, Op::LE_S, Op::GT_S, Op::GE_S, Op::LT_U, Op::LE_U, Op::GT_U, Op::GE_U};
const Combo SPEC_CMP_MODE[4] = {Combo::REG_ACC, Combo::POP_ACC, Combo::IMM_ACC, Combo::IMM_ACC};
const Op SPEC_MISC[18] = {
    Op::NEG, Op::NOT, Op::CLZ, Op::REVBITS,               // 90-93
    Op::BLOCK_END, Op::LOOP, Op::BR_TABLE, Op::BR_TABLE,  // 94-97
    Op::BR_TABLE, Op::CALL, Op::RETURN, Op::TRAP,         // 98-101
    Op::TRAP, Op::PUSH, Op::POP, Op::LOAD,                // 102-105
    Op::STORE, Op::CONST,                                  // 106-107
};
// Which assigned opcodes carry a trailing LEB128 (§5.4).
bool specHasTrailingOperand(uint32_t code)
{
    if(code <= 49)
    {
        uint32_t mode = code % 5;
        return mode == 0 || mode == 1 || mode == 4;
    }
    if(code <= 89)
    {
        return (code - 50) % 4 == 0 || (code - 50) % 4 == 3;
    }
    return code == 98 || code == 99 || code == 102 || code == 105 || code == 106 || code == 107;
}

} // namespace

TEST(DecodeTableAgreesWithTheSpecsOwnFormulas)
{
    // decode_instr.cpp replaced §5.2's `code / 5` and `(code-50) / 4` with
    // a static table, because on a Cortex-M0 that division is a call into
    // libgcc. Here on the host division is free, so re-derive the spec's
    // formulas and check the table reproduces them for every assigned
    // opcode — op, combo, and whether an operand trails.
    for(uint32_t code = 0; code <= 123; code++)
    {
        // A 2-byte LEB128 tail, so a decoder that wrongly expects an
        // operand (or wrongly skips one) lands on the wrong `next`.
        const uint8_t bytes[] = {(uint8_t)code, 0x81, 0x01, 0x00};
        DecodedInstr d = decodeInstr(bytes, sizeof(bytes), 0);

        Op expectOp;
        Combo expectCombo = Combo::NONE;
        if(code <= 49)
        {
            expectOp = SPEC_ARITH[code / 5];
            expectCombo = SPEC_ARITH_MODE[code % 5];
        }
        else if(code <= 89)
        {
            expectOp = SPEC_CMP[(code - 50) / 4];
            expectCombo = SPEC_CMP_MODE[(code - 50) % 4];
        }
        else if(code <= 107)
        {
            expectOp = SPEC_MISC[code - 90];
        }
        else
        {
            expectOp = Op::CONST;
        }

        CHECK(d.instr.op == expectOp);
        CHECK(d.instr.combo == expectCombo);
        CHECK(d.next == (specHasTrailingOperand(code) ? 3u : 1u));
    }
}

TEST(DecodeSuppliesTheImplicitAuxValuesTheSpecPromises)
{
    // The forms whose auxiliary value comes from the opcode itself rather
    // than a trailing operand (§5.2's IMM_SMALL, BR_TABLE#1/#2, TRAP#0,
    // CONST#0..15) — the ones a table has to carry as constants.
    const uint8_t brTable1[] = {96}, brTable2[] = {97}, trap0[] = {101};
    CHECK(decodeInstr(brTable1, 1, 0).instr.imm == 1);
    CHECK(decodeInstr(brTable2, 1, 0).instr.imm == 2);
    CHECK(decodeInstr(trap0, 1, 0).instr.imm == 0);

    for(uint32_t i = 0; i < 16; i++)
    {
        const uint8_t small[] = {(uint8_t)(108 + i)};
        DecodedInstr d = decodeInstr(small, 1, 0);
        CHECK(d.instr.op == Op::CONST);
        CHECK(d.instr.imm == (int32_t)i);
        CHECK(d.next == 1);
    }

    for(uint32_t cmp = 0; cmp < 10; cmp++)
    {
        const uint8_t immSmall[] = {(uint8_t)(50 + cmp * 4 + 2)};
        DecodedInstr d = decodeInstr(immSmall, 1, 0);
        CHECK(d.instr.combo == Combo::IMM_ACC);
        CHECK(d.instr.imm == 0);
    }
}

TEST(EveryAssignedOpcodeSurvivesAnEncodeDecodeCycle)
{
    // Byte-identity is the wrong invariant — the encoder canonicalizes
    // (TRAP#0 for a zero code, CONST#n for a small literal, IMM_SMALL for
    // a zero comparison operand), so a non-canonical input re-encodes to a
    // shorter form. What must hold is that the *instruction* is a fixed
    // point: decode, encode, decode again, and nothing has changed.
    for(uint32_t code = 0; code <= 123; code++)
    {
        const uint8_t bytes[] = {(uint8_t)code, 0x81, 0x01, 0x00};
        DecodedInstr first = decodeInstr(bytes, sizeof(bytes), 0);

        uint8_t reencoded[8];
        uint32_t len = 0;
        encodeInstr(first.instr, reencoded, len, sizeof(reencoded));
        CHECK(len > 0);

        DecodedInstr second = decodeInstr(reencoded, len, 0);
        CHECK(sameInstr(first.instr, second.instr));
        CHECK(second.next == len);
    }
}
