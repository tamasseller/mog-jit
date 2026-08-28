#include "decode_instr.h"
#include "ext.h"

#include <cassert>

namespace jitc
{

// How an instruction's single auxiliary value is obtained. isa-core.md
// §5.2 gives every core form at most one, and only these four shapes
// occur across all 124 assigned codes.
enum AuxKind : uint8_t
{
    AUX_NONE = 0, // no auxiliary value; stays 0
    AUX_ONE = 1,  // implicit 1 (BR_TABLE#1)
    AUX_TWO = 2,  // implicit 2 (BR_TABLE#2)
    AUX_EXT = 3,  // trailing unsigned LEB128 (isa-core.md §5.4)
};

/** One row per assigned opcode below SMALL_CONST_BASE. Two bytes because
 *  Op needs 6 bits and Combo/AuxKind another 5 between them; splitting
 *  them across a byte each keeps the decode to a pair of loads with no
 *  masking of Op at all.
 *
 *  This table *is* the decode, not a cache of it: isa-core.md §5.2 states
 *  the arithmetic and comparison ranges as `code / 5` and `(code-50) / 4`,
 *  but §5.1 explicitly admits a static table as the equivalent, and on a
 *  Cortex-M0 that difference is a call into libgcc's __udivsi3 per decode
 *  (this file was the whole image's only reason to link division at all).
 *  encode_instr.cpp still derives the same numbering arithmetically; the
 *  two are held together by test_decode_encode.cpp's round trip over
 *  every opcode. */
struct Entry
{
    uint8_t op;
    uint8_t shape; // (Combo << 2) | AuxKind
};

static constexpr uint8_t shapeOf(Combo combo, AuxKind aux)
{
    return (uint8_t)(((uint8_t)combo << 2) | (uint8_t)aux);
}

static constexpr Entry row(Op op, Combo combo, AuxKind aux)
{
    return Entry{(uint8_t)op, shapeOf(combo, aux)};
}

// isa-core.md §5.2's mode ordering, per class: arithmetic is REG_ACC,
// REG_REG, PEEK_PEEK, POP_ACC, IMM_EXT; comparison is REG_ACC, POP_ACC,
// IMM_SMALL, IMM_EXT (no write-back-in-place form for a comparison).
#define JITC_ARITH_ROWS(op)                                            \
    row(op, Combo::REG_ACC, AUX_EXT), row(op, Combo::REG_REG, AUX_EXT), \
    row(op, Combo::PEEK_PEEK, AUX_NONE), row(op, Combo::POP_ACC, AUX_NONE), \
    row(op, Combo::IMM_ACC, AUX_EXT)

#define JITC_CMP_ROWS(op)                                              \
    row(op, Combo::REG_ACC, AUX_EXT), row(op, Combo::POP_ACC, AUX_NONE), \
    row(op, Combo::IMM_ACC, AUX_NONE), row(op, Combo::IMM_ACC, AUX_EXT)

constexpr Entry TABLE[] = {
    // 0-49: arithmetic (§4.1)
    JITC_ARITH_ROWS(Op::ADD), JITC_ARITH_ROWS(Op::SUB), JITC_ARITH_ROWS(Op::RSUB),
    JITC_ARITH_ROWS(Op::MUL), JITC_ARITH_ROWS(Op::AND), JITC_ARITH_ROWS(Op::OR),
    JITC_ARITH_ROWS(Op::XOR), JITC_ARITH_ROWS(Op::SHL), JITC_ARITH_ROWS(Op::SHR),
    JITC_ARITH_ROWS(Op::ASR),
    // 50-89: comparison (§4.2)
    JITC_CMP_ROWS(Op::EQ), JITC_CMP_ROWS(Op::NE), JITC_CMP_ROWS(Op::LT_S),
    JITC_CMP_ROWS(Op::LE_S), JITC_CMP_ROWS(Op::GT_S), JITC_CMP_ROWS(Op::GE_S),
    JITC_CMP_ROWS(Op::LT_U), JITC_CMP_ROWS(Op::LE_U), JITC_CMP_ROWS(Op::GT_U),
    JITC_CMP_ROWS(Op::GE_U),
    // 90-107: unary (§4.3), local/global flow control, and the move/const
    // forms that aren't a small literal — already one contiguous run in
    // §5.2, so one table rather than a range check per class.
    row(Op::NEG, Combo::NONE, AUX_NONE),       // 90
    row(Op::NOT, Combo::NONE, AUX_NONE),       // 91
    row(Op::CLZ, Combo::NONE, AUX_NONE),       // 92
    row(Op::REVBITS, Combo::NONE, AUX_NONE),   // 93
    row(Op::BLOCK_END, Combo::NONE, AUX_NONE), // 94
    row(Op::LOOP, Combo::NONE, AUX_NONE),      // 95
    row(Op::BR_TABLE, Combo::NONE, AUX_ONE),   // 96
    row(Op::BR_TABLE, Combo::NONE, AUX_TWO),   // 97
    row(Op::BR_TABLE, Combo::NONE, AUX_EXT),   // 98
    row(Op::CALL, Combo::NONE, AUX_EXT),       // 99
    row(Op::RETURN, Combo::NONE, AUX_NONE),    // 100
    row(Op::TRAP, Combo::NONE, AUX_NONE),      // 101
    row(Op::TRAP, Combo::NONE, AUX_EXT),       // 102
    row(Op::PUSH, Combo::NONE, AUX_NONE),      // 103
    row(Op::POP, Combo::NONE, AUX_NONE),       // 104
    row(Op::LOAD, Combo::NONE, AUX_EXT),       // 105
    row(Op::STORE, Combo::NONE, AUX_EXT),      // 106
    row(Op::CONST, Combo::NONE, AUX_EXT),      // 107
};

#undef JITC_ARITH_ROWS
#undef JITC_CMP_ROWS

// 108-123 are CONST#0..15, whose value is the opcode's own offset from
// this base — a range check rather than 16 more table rows.
constexpr uint32_t SMALL_CONST_BASE = 108;

static_assert(sizeof(TABLE) / sizeof(TABLE[0]) == SMALL_CONST_BASE,
    "decode table must cover exactly opcodes 0..SMALL_CONST_BASE-1 (isa-core.md §5.2)");

uint32_t decodeLeb128(const uint8_t *bytes, uint32_t offset, uint32_t &next)
{
    uint32_t value = 0, shift = 0, pos = offset;
    for(;;)
    {
        assert(pos < UINT32_MAX); // GCOV_EXCL_LINE — caller already bounds-checked offset itself
        assert(shift < 32); // GCOV_EXCL_LINE — a canonical u32 LEB128 never needs more than 5 bytes; a non-canonical, overlong one is malformed, same as any other decode failure this file asserts on rather than checks
        uint8_t byte = bytes[pos];
        value += (uint32_t)(byte & 0x7f) << shift;
        pos++;
        if((byte & 0x80) == 0)
        {
            break;
        }
        shift += 7;
    }
    next = pos;
    return value;
}

bool decodeLeb128Checked(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset,
    uint32_t &value, uint32_t &next)
{
    uint32_t v = 0, shift = 0, pos = offset;
    for(;;)
    {
        if(pos >= bytesLen || shift >= 32)
        {
            return false; // off the end still continuing, or overlong for a u32
        }
        uint8_t byte = bytes[pos];
        v += (uint32_t)(byte & 0x7f) << shift;
        pos++;
        if((byte & 0x80) == 0)
        {
            break;
        }
        shift += 7;
    }
    value = v;
    next = pos;
    return true;
}

uint32_t extDecodeLength(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t &decl,
    const ExtHooks *ext)
{
    if(ext == nullptr || ext->decode == nullptr)
    {
        return 0;
    }
    uint32_t len = ext->decode(bytes, bytesLen, offset, &decl);
    // Two things the core will not take on trust, because both would turn a
    // bad extension into a hang or an overrun rather than a diagnostic: no
    // forward progress, and a length running past the buffer.
    if(len == 0 || len > bytesLen - offset)
    {
        return 0;
    }
    return len;
}

DecodedInstr decodeInstr(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, const ExtHooks *ext)
{
    assert(offset < bytesLen); // GCOV_EXCL_LINE — ran off the end of the buffer; malformed input
    (void)bytesLen;            // only consulted by the assert above outside debug builds
    
    uint32_t code = bytes[offset];
    uint32_t pos = offset + 1;

    Instr instr{};

    if(code >= EXT_OPCODE_BASE)
    {
        // The extension range (§11) ONLY. The four codes the core reserves
        // but hasn't assigned (124-127, §5.3) are not extension space and
        // are never offered to an extension — Runtime::init's walk rejects
        // them, which is also what lets this return a well-formed Instr
        // unconditionally here.
        instr.op = Op::EXT;
        uint32_t len = extDecodeLength(bytes, bytesLen, offset, instr.extDecl, ext);
        assert(len >= 1); // GCOV_EXCL_LINE — unreachable: the walk already accepted this byte
        return {instr, offset + len};
    }

    assert(code <= LAST_CORE_OPCODE); // GCOV_EXCL_LINE — unreachable: 124-127 are core-reserved (§5.3), rejected by the walk

    if(code >= SMALL_CONST_BASE)
    {
        instr.op = Op::CONST;
        instr.imm = (int32_t)(code - SMALL_CONST_BASE);
        return {instr, pos};
    }

    const Entry &entry = TABLE[code];
    instr.op = (Op)entry.op;
    instr.combo = (Combo)(entry.shape >> 2);

    uint32_t aux = entry.shape & 3u;
    if(aux == AUX_EXT)
    {
        uint32_t next;
        // One destination for every trailing operand: which union member
        // it means is already fixed by op/combo (see Instr's own comment),
        // and they share storage.
        instr.imm = (int32_t)decodeLeb128(bytes, pos, next);
        return {instr, next};
    }
    instr.imm = (int32_t)aux; // AUX_NONE leaves 0; AUX_ONE/AUX_TWO are BR_TABLE's implicit N
    return {instr, pos};
}

} // namespace jitc
