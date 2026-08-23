#include "decode_instr.h"

#include <cassert>

namespace jitc {

namespace {

// Op ordering here is the wire format's own authority (isa-core.md §5.2) —
// must match packages/machine/src/bytecode.ts's ARITH_OPS/CMP_OPS/
// UNARY_OPS exactly, cross-checked by test_decode_instr.cpp's own
// round-trip against encode_instr.h.
constexpr Op ARITH_OPS[10] = {Op::ADD, Op::SUB, Op::RSUB, Op::MUL, Op::AND, Op::OR, Op::XOR, Op::SHL, Op::SHR, Op::ASR};
constexpr Op CMP_OPS[10]   = {Op::EQ, Op::NE, Op::LT_S, Op::LE_S, Op::GT_S, Op::GE_S, Op::LT_U, Op::LE_U, Op::GT_U, Op::GE_U};
constexpr Op UNARY_OPS[4]  = {Op::NEG, Op::NOT, Op::CLZ, Op::REVBITS};

} // namespace

uint32_t decodeLeb128(const uint8_t *bytes, uint32_t offset, uint32_t &next) {
    uint32_t value = 0, shift = 0, pos = offset;
    for(;;) {
        assert(pos < UINT32_MAX); // GCOV_EXCL_LINE — caller already bounds-checked offset itself
        uint8_t byte = bytes[pos];
        value += (uint32_t)(byte & 0x7f) << shift;
        pos++;
        if((byte & 0x80) == 0) break;
        shift += 7;
    }
    next = pos;
    return value;
}

DecodedInstr decodeInstr(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset) {
    assert(offset < bytesLen); // GCOV_EXCL_LINE — ran off the end of the buffer; malformed input
    (void)bytesLen; // only consulted by the assert above outside debug builds
    uint32_t code = bytes[offset];
    uint32_t pos = offset + 1;

    assert(code < 128); // GCOV_EXCL_LINE — extension opcode, never registered by this JIT

    Instr instr{};
    uint32_t next;

    if(code <= 49) {
        instr.op = ARITH_OPS[code / 5];
        uint32_t mode = code % 5;
        if(mode == 0) { instr.combo = Combo::REG_ACC; instr.target = decodeLeb128(bytes, pos, next); return {instr, next}; }
        if(mode == 1) { instr.combo = Combo::REG_REG; instr.target = decodeLeb128(bytes, pos, next); return {instr, next}; }
        if(mode == 2) { instr.combo = Combo::PEEK_PEEK; return {instr, pos}; }
        if(mode == 3) { instr.combo = Combo::POP_ACC; return {instr, pos}; }
        instr.combo = Combo::IMM_ACC;
        instr.imm = (int32_t)decodeLeb128(bytes, pos, next);
        return {instr, next};
    }

    if(code <= 89) {
        uint32_t rel = code - 50;
        instr.op = CMP_OPS[rel / 4];
        uint32_t mode = rel % 4;
        if(mode == 0) { instr.combo = Combo::REG_ACC; instr.target = decodeLeb128(bytes, pos, next); return {instr, next}; }
        if(mode == 1) { instr.combo = Combo::POP_ACC; return {instr, pos}; }
        instr.combo = Combo::IMM_ACC;
        if(mode == 2) { instr.imm = 0; return {instr, pos}; }
        instr.imm = (int32_t)decodeLeb128(bytes, pos, next);
        return {instr, next};
    }

    if(code <= 93) { instr.op = UNARY_OPS[code - 90]; return {instr, pos}; }

    switch(code) {
        case 94: instr.op = Op::BLOCK_END; return {instr, pos};
        case 95: instr.op = Op::LOOP; return {instr, pos};
        case 96: instr.op = Op::BR_TABLE; instr.imm = 1; return {instr, pos};
        case 97: instr.op = Op::BR_TABLE; instr.imm = 2; return {instr, pos};
        case 98: instr.op = Op::BR_TABLE; instr.imm = (int32_t)decodeLeb128(bytes, pos, next); return {instr, next};
        case 99: instr.op = Op::CALL; instr.calleeIndex = decodeLeb128(bytes, pos, next); return {instr, next};
        case 100: instr.op = Op::RETURN; return {instr, pos};
        case 101: instr.op = Op::TRAP; instr.imm = 0; return {instr, pos};
        case 102: instr.op = Op::TRAP; instr.imm = (int32_t)decodeLeb128(bytes, pos, next); return {instr, next};
        case 103: instr.op = Op::PUSH; return {instr, pos};
        case 104: instr.op = Op::POP; return {instr, pos};
        case 105: instr.op = Op::LOAD; instr.target = decodeLeb128(bytes, pos, next); return {instr, next};
        case 106: instr.op = Op::STORE; instr.target = decodeLeb128(bytes, pos, next); return {instr, next};
        case 107: instr.op = Op::CONST; instr.imm = (int32_t)decodeLeb128(bytes, pos, next); return {instr, next};
    }

    if(code <= 123) { instr.op = Op::CONST; instr.imm = (int32_t)code - 108; return {instr, pos}; }

    assert(false && "decode_instr: reserved and unassigned opcode (isa-core.md §5.3)"); // GCOV_EXCL_LINE
    return {instr, pos}; // GCOV_EXCL_LINE — unreachable, keeps -Wreturn-type quiet
}

} // namespace jitc
