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
    // Comparisons (isa-core.md §4.2) — same Combo dimension as arithmetic,
    // minus REG_REG/PEEK_PEEK (isa-core.md §4.2's own table has no
    // write-back-in-place form for a comparison).
    EQ, NE, LT_S, LE_S, GT_S, GE_S, LT_U, LE_U, GT_U, GE_U,
    // Unary (isa-core.md §4.3) — no Combo of their own, always Combo::NONE.
    NEG, NOT, CLZ, REVBITS,
    // Local flow control (isa-core.md §7.1/§7.2).
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
    int32_t imm = 0;          // CONST's value / IMM_ACC operand / TRAP's code / BR_TABLE's N
    uint32_t target = 0;      // LOAD/STORE/REG_ACC/REG_REG's slot index k
    uint32_t calleeIndex = 0; // CALL only
};

// Builder helpers — 1:1 with packages/machine/src/rtl.ts's constructors, so
// a transcribed fixture reads like its TS source line for line.
constexpr Instr CONST(int32_t v)
{
    return Instr{Op::CONST, Combo::NONE, v, 0, 0};
}

constexpr Instr LOAD(uint32_t target)
{
    return Instr{Op::LOAD, Combo::NONE, 0, target, 0};
}

constexpr Instr STORE(uint32_t target)
{
    return Instr{Op::STORE, Combo::NONE, 0, target, 0};
}

constexpr Instr PUSH()
{
    return Instr{Op::PUSH, Combo::NONE, 0, 0, 0};
}

constexpr Instr POP()
{
    return Instr{Op::POP, Combo::NONE, 0, 0, 0};
}

constexpr Instr bare(Op op)
{
    return Instr{op, Combo::NONE, 0, 0, 0};
}

constexpr Instr trapInstr(uint32_t code)
{
    return Instr{Op::TRAP, Combo::NONE, (int32_t)code, 0, 0};
}

constexpr Instr call(uint32_t calleeIndex)
{
    return Instr{Op::CALL, Combo::NONE, 0, 0, calleeIndex};
}

constexpr Instr opReg(Op op, uint32_t target)
{
    return Instr{op, Combo::REG_ACC, 0, target, 0};
}

constexpr Instr opRegWriteback(Op op, uint32_t target)
{
    return Instr{op, Combo::REG_REG, 0, target, 0};
}

constexpr Instr opImm(Op op, int32_t v)
{
    return Instr{op, Combo::IMM_ACC, v, 0, 0};
}

constexpr Instr opStack(Op op, Combo combo)
{
    return Instr{op, combo, 0, 0, 0};
}

constexpr Instr brTable(uint32_t n)
{
    return Instr{Op::BR_TABLE, Combo::NONE, (int32_t)n, 0, 0};
}

/** Whether op is one of the ten comparison opcodes. blocks.h uses this to
 *  decide fusion-vs-materialize; decode_instr.h/encode_instr.h's own
 *  op-index tables depend on the EQ..GE_U range staying contiguous. */
constexpr bool isComparisonOp(Op op)
{
    return op >= Op::EQ && op <= Op::GE_U;
}

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_INSTR_H_
