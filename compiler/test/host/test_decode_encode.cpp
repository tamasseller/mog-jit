// jit-armv6m/compiler/test — decode_instr.h/encode_instr.h round-trip,
// ported from jit-armv6m/prototype/test/bytecodeReader-decode.test.ts.
// One instruction of every shape decode_instr.h claims to handle, encoded
// via encode_instr.h and decoded back — cross-checks the two against each
// other the same discipline test_translate_proc.cpp's own fixtures use
// (hand-derived, not generated), since there is no @ppl/machine here to
// cross-check against directly.
#include "Test.h"
#include "decode_instr.h"
#include "encode_instr.h"

#include <cstring>

using namespace jitc;

namespace {

bool sameInstr(const Instr &a, const Instr &b) {
    if(a.op != b.op || a.combo != b.combo) return false;
    if(a.op == Op::CALL) return a.calleeIndex == b.calleeIndex;
    if(a.combo == Combo::REG_ACC || a.combo == Combo::REG_REG) return a.target == b.target;
    if(a.combo == Combo::IMM_ACC || a.op == Op::CONST || a.op == Op::TRAP || a.op == Op::BR_TABLE) return a.imm == b.imm;
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

TEST(DecodeInstrRoundTripsEveryShape) {
    for(uint32_t i = 0; i < kCaseCount; i++) {
        uint8_t bytes[8];
        uint32_t len = 0;
        encodeInstr(kCases[i], bytes, len, sizeof(bytes));

        DecodedInstr d = decodeInstr(bytes, len, 0);
        CHECK(sameInstr(d.instr, kCases[i]));
        CHECK(d.next == len);
    }
}

TEST(DecodeInstrAdvancesThroughARunOfInstructions) {
    const Instr program[] = {opImm(Op::ADD, 5), LOAD(0), bare(Op::RETURN)};
    uint8_t bytes[32];
    uint32_t len = encodeBody(program, 3, bytes, sizeof(bytes));

    uint32_t pos = 0;
    for(uint32_t i = 0; i < 3; i++) {
        DecodedInstr d = decodeInstr(bytes, len, pos);
        CHECK(sameInstr(d.instr, program[i]));
        pos = d.next;
    }
    CHECK(pos == len);
}

TEST(EncodeConstSmallImmFitsOneByte) {
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeInstr(CONST(15), bytes, len, sizeof(bytes));
    CHECK(len == 1);
    CHECK(bytes[0] == 108 + 15);
}

TEST(EncodeConstLargeImmUsesLeb128Form) {
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeInstr(CONST(16), bytes, len, sizeof(bytes));
    CHECK(len == 2);
    CHECK(bytes[0] == 107);
    CHECK(bytes[1] == 16);
}

TEST(EncodeComparisonZeroImmCollapsesToOneByte) {
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeInstr(opImm(Op::EQ, 0), bytes, len, sizeof(bytes));
    CHECK(len == 1); // mode 2 ("IMM_ACC #0"), no operand byte
}

TEST(EncodeLeb128MultiByteRoundTrips) {
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeLeb128(300, bytes, len, sizeof(bytes)); // 300 = 0b1_0010_1100 -> 2 LEB128 bytes
    CHECK(len == 2);

    uint32_t next;
    uint32_t value = decodeLeb128(bytes, 0, next);
    CHECK(value == 300);
    CHECK(next == 2);
}
