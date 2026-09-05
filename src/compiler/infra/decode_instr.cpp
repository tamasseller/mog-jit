#include "decode_instr.h"

namespace jitc
{

enum AuxKind : uint8_t
{
    AUX_NONE = 0,   // no auxiliary value; stays 0
    AUX_ONE = 1,    // implicit 1 (BR_TABLE#1)
    AUX_EXT = 2,    // trailing unsigned LEB128 (isa-core.md §5.4)
    AUX_EXT_BR = 3, // that LEB128 plus 2 — BR_TABLE's biased case count (§5.4)
};

/** `MISC_OTHER`'s assigned sub-codes (isa-core.md §5.3). `DROP #1..#4` sit
 *  on the four codes above `SUB_DROP_EXT`, so a small count's sub-code is
 *  `n + 2`; the extended form's own operand is biased by `DROP_EXT_BIAS`. */
enum MiscOtherSub : uint32_t
{
    SUB_FALLTHROUGH = 0,
    SUB_DEFAULT = 1,
    SUB_DROP_EXT = 2,
};
static constexpr uint32_t DROP_SMALL_MAX = 4;
static constexpr uint32_t DROP_EXT_BIAS = DROP_SMALL_MAX + 1;
static constexpr uint32_t MISC_OTHER_SUB_MAX = SUB_DROP_EXT + DROP_SMALL_MAX;


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
    // 90-95: unary (§4.3)
    row(Op::NEG, Combo::NONE, AUX_NONE),       // 90
    row(Op::NOT, Combo::NONE, AUX_NONE),       // 91
    row(Op::SXTB, Combo::NONE, AUX_NONE),      // 92
    row(Op::SXTH, Combo::NONE, AUX_NONE),      // 93
    row(Op::UXTB, Combo::NONE, AUX_NONE),      // 94
    row(Op::UXTH, Combo::NONE, AUX_NONE),      // 95
    row(Op::BLOCK_END, Combo::NONE, AUX_NONE), // 96
    row(Op::LOOP_PRE, Combo::NONE, AUX_NONE),  // 97
    row(Op::LOOP_POST, Combo::NONE, AUX_NONE), // 98
    row(Op::BR_TABLE, Combo::NONE, AUX_ONE),   // 99
    row(Op::BR_TABLE, Combo::NONE, AUX_EXT_BR), // 100
    row(Op::CALL, Combo::NONE, AUX_EXT),       // 101
    row(Op::RETURN, Combo::NONE, AUX_NONE),    // 102
    row(Op::TRAP, Combo::NONE, AUX_NONE),      // 103
    row(Op::TRAP, Combo::NONE, AUX_EXT),       // 104
    row(Op::PUSH, Combo::NONE, AUX_NONE),      // 105
    row(Op::LOAD, Combo::NONE, AUX_EXT),       // 106
    row(Op::STORE, Combo::NONE, AUX_EXT),      // 107
    row(Op::CONST, Combo::NONE, AUX_EXT),      // 108
};

#undef JITC_ARITH_ROWS
#undef JITC_CMP_ROWS

static_assert(sizeof(TABLE) / sizeof(TABLE[0]) == SMALL_CONST_BASE,
    "decode table must cover exactly opcodes 0..SMALL_CONST_BASE-1 (isa-core.md §5.2)");

static_assert(TABLE[BLOCK_END_OPCODE].op == (uint8_t)Op::BLOCK_END,
    "BLOCK_END_OPCODE must name the same code the table does");

/** Each escape's assigned sub-codes, in sub-code order (isa-core.md §5.3). */
static constexpr Op MISC_UNARY_OPS[] = {Op::REVBITS, Op::CLZ};
static constexpr uint32_t MISC_UNARY_COUNT = sizeof(MISC_UNARY_OPS) / sizeof(MISC_UNARY_OPS[0]);

bool miscSubCodeAssigned(uint32_t code, uint32_t sub)
{
    /* MISC_BINARY has none yet. */
    return (code == MISC_UNARY && sub < MISC_UNARY_COUNT)
        || (code == MISC_OTHER && sub <= MISC_OTHER_SUB_MAX);
}

bool decodeLeb128(BcReader &r, uint32_t &value)
{
    uint32_t v = 0, shift = 0;
    for(;;)
    {
        if(shift >= 32 || r.atEnd())
        {
            return false; // overlong for a u32, or the body ends mid-value
        }

        const uint8_t byte = r.next();
        v += (uint32_t)(byte & 0x7f) << shift;

        if((byte & 0x80) == 0)
        {
            break;
        }
        shift += 7;
    }

    value = v;
    return true;
}

bool decodeInstr(uint8_t code, BcReader &r, Instr &out)
{
    Instr instr{};

    if(code >= EXT_OPCODE_BASE)
    {
        instr.op = Op::EXT;
        instr.extOpcode = code;
        out = instr;
        return true;
    }

    if(code >= MISC_BASE)
    {
        uint32_t sub = 0;
        if(!decodeLeb128(r, sub) || !miscSubCodeAssigned(code, sub))
        {
            return false;
        }

        if(code == MISC_UNARY)
        {
            instr.op = MISC_UNARY_OPS[sub];
            out = instr;
            return true;
        }

        if(sub == SUB_FALLTHROUGH)
        {
            instr.op = Op::FALLTHROUGH;
        }
        else if(sub == SUB_DEFAULT)
        {
            instr.op = Op::DEFAULT;
        }
        else if(sub == SUB_DROP_EXT)
        {
            uint32_t n = 0;
            if(!decodeLeb128(r, n))
            {
                return false;
            }
            instr.op = Op::DROP;
            instr.imm = (int32_t)(n + DROP_EXT_BIAS);
        }
        else
        {
            instr.op = Op::DROP;
            instr.imm = (int32_t)(sub - SUB_DROP_EXT);
        }

        out = instr;
        return true;
    }

    if(code >= SMALL_CONST_BASE)
    {
        instr.op = Op::CONST;
        instr.imm = (int32_t)(code - SMALL_CONST_BASE);
        out = instr;
        return true;
    }

    const Entry &entry = TABLE[code];
    instr.op = (Op)entry.op;
    instr.combo = (Combo)(entry.shape >> 2);

    uint32_t aux = entry.shape & 3u;
    if(aux == AUX_EXT || aux == AUX_EXT_BR)
    {
        uint32_t value = 0;
        if(!decodeLeb128(r, value))
        {
            return false;
        }
        instr.imm = (int32_t)value + (aux == AUX_EXT_BR ? 2 : 0);
    }
    else
    {
        instr.imm = (int32_t)aux; // AUX_NONE leaves 0; AUX_ONE is BR_TABLE's implicit N
    }

    out = instr;
    return true;
}

} // namespace jitc
