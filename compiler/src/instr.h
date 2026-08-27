// jit-armv6m/compiler — the bytecode representation this translator
// consumes. Deliberately independent of @ppl/machine's rtl.ts: same
// conceptual shapes, new types, no runtime dependency either way. A flat,
// aggregate-initializable struct (not a tagged union) so a fixture literal
// reads almost identically to its rtl.ts source.
//
// EXT is absent from Op at compile time: nothing in this JIT's own scope
// (isa-core.md §11) ever needs it, and decode_instr.h asserts on an
// extension opcode byte instead.
#ifndef JIT_ARMV6M_COMPILER_INSTR_H_
#define JIT_ARMV6M_COMPILER_INSTR_H_

#include <cstdint>

namespace jitc
{

enum class Op : uint8_t
{
    CONST, LOAD, STORE, PUSH, POP,
    RETURN, TRAP,
    CALL,
    ADD, SUB, RSUB, MUL, AND, OR, XOR, SHL, SHR, ASR,
    EQ, NE, LT_S, LE_S, GT_S, GE_S, LT_U, LE_U, GT_U, GE_U,
    NEG, NOT, CLZ, REVBITS,
    BLOCK_END, LOOP, BR_TABLE,
};

enum class Combo : uint8_t
{
    NONE, REG_ACC, REG_REG, IMM_ACC, PEEK_PEEK, POP_ACC
};

struct Instr
{
    Op op;
    Combo combo = Combo::NONE;

    union
    {
        int32_t imm = 0;      // CONST's value / IMM_ACC operand / TRAP's code / BR_TABLE's N
        uint32_t target;      // LOAD/STORE/REG_ACC/REG_REG's slot index k
        uint32_t calleeIndex; // CALL only
    };
};

constexpr Instr CONST(int32_t v)
{
    return Instr{Op::CONST, Combo::NONE, v};
}

constexpr Instr LOAD(uint32_t target)
{
    return Instr{Op::LOAD, Combo::NONE, (int32_t)target};
}

constexpr Instr STORE(uint32_t target)
{
    return Instr{Op::STORE, Combo::NONE, (int32_t)target};
}

constexpr Instr PUSH()
{
    return Instr{Op::PUSH};
}

constexpr Instr POP()
{
    return Instr{Op::POP};
}

constexpr Instr bare(Op op)
{
    return Instr{op};
}

constexpr Instr trapInstr(uint32_t code)
{
    return Instr{Op::TRAP, Combo::NONE, (int32_t)code};
}

constexpr Instr call(uint32_t calleeIndex)
{
    return Instr{Op::CALL, Combo::NONE, (int32_t)calleeIndex};
}

constexpr Instr opReg(Op op, uint32_t target)
{
    return Instr{op, Combo::REG_ACC, (int32_t)target};
}

constexpr Instr opRegWriteback(Op op, uint32_t target)
{
    return Instr{op, Combo::REG_REG, (int32_t)target};
}

constexpr Instr opImm(Op op, int32_t v)
{
    return Instr{op, Combo::IMM_ACC, v};
}

constexpr Instr opStack(Op op, Combo combo)
{
    return Instr{op, combo};
}

constexpr Instr brTable(uint32_t n)
{
    return Instr{Op::BR_TABLE, Combo::NONE, (int32_t)n};
}

/** Whether op is one of the ten comparison opcodes. blocks.h uses this to
 *  decide fusion-vs-materialize; decode_instr.h/encode_instr.h's own
 *  op-index tables depend on the EQ..GE_U range staying contiguous. */
constexpr bool isComparisonOp(Op op)
{
    return op >= Op::EQ && op <= Op::GE_U;
}

/** Whether op takes its IMM_ACC operand as a shift *amount* rather than a
 *  value — binops.h consumes it directly as an Imm<5>, so unlike every
 *  other immediate operand it must stay an immediate. */
constexpr bool isShiftOp(Op op)
{
    return op == Op::SHL || op == Op::SHR || op == Op::ASR;
}

constexpr bool isTerminator(Op op)
{
    return op == Op::BLOCK_END || op == Op::RETURN || op == Op::TRAP;
}

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_INSTR_H_
