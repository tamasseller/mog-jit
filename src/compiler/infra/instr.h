// jit-armv6m/compiler — the bytecode representation this translator
// consumes. Deliberately independent of mog-core's rtl.ts: same
// conceptual shapes, new types, no runtime dependency either way. A flat,
// aggregate-initializable struct (not a tagged union) so a fixture literal
// reads almost identically to its rtl.ts source.
//
// Op::EXT is APPENDED, never inserted: isArithOp, isComparisonOp and the
// unary range are all `op >= X && op <= Y` tests over this enum's order.
#ifndef JIT_ARMV6M_COMPILER_INSTR_H_
#define JIT_ARMV6M_COMPILER_INSTR_H_

#include <cstdint>

namespace jitc
{

enum class Op : uint8_t
{
    CONST, LOAD, STORE, PUSH,
    RETURN, TRAP,
    CALL,
    ADD, SUB, RSUB, MUL, AND, OR, XOR, SHL, SHR, ASR,
    EQ, NE, LT_S, LE_S, GT_S, GE_S, LT_U, LE_U, GT_U, GE_U,
    NEG, NOT, SXTB, SXTH, UXTB, UXTH, // §5.2's own unary range, in its order
    CLZ, REVBITS,                     // §5.3's MISC_UNARY sub-codes
    BLOCK_END, LOOP_PRE, LOOP_POST, BR_TABLE, FALLTHROUGH,
    DEFAULT, DROP,                    // §5.3's MISC_OTHER sub-codes
    EXT, // one registered extension's opcode (isa-core.md §11); see ext.h
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
        uint32_t extOpcode;   // EXT only: the opcode byte. Not the operands — those stay
                              // on the wire (§11.3), still unread, because only the
                              // extension knows how many of them there are.
    };
};

static_assert(sizeof(Instr) == 8, "Instr must stay two words — it is returned by value from every decode");

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

constexpr Instr dropInstr(uint32_t n)
{
    return Instr{Op::DROP, Combo::NONE, (int32_t)n};
}

/** The two loop openers, which differ only in where the construct is
 *  entered (isa-core.md §4.5). */
constexpr bool isLoopOpener(Op op)
{
    return op == Op::LOOP_PRE || op == Op::LOOP_POST;
}

constexpr bool isComparisonOp(Op op)
{
    return op >= Op::EQ && op <= Op::GE_U;
}

constexpr bool isShiftOp(Op op)
{
    return op == Op::SHL || op == Op::SHR || op == Op::ASR;
}

/** Ends a block. `FALLTHROUGH` ends one by continuing into the next case
 *  and `DEFAULT` by continuing into that dispatch's own last one, rather
 *  than leaving the construct (isa-core.md §4.5). */
constexpr bool isTerminator(Op op)
{
    return op == Op::BLOCK_END || op == Op::FALLTHROUGH || op == Op::DEFAULT
        || op == Op::RETURN || op == Op::TRAP;
}

constexpr bool isProcTerminator(Op op)
{
    return op == Op::RETURN || op == Op::TRAP;
}

constexpr bool isTerminator(const Instr &instr)
{
    return isTerminator(instr.op);
}

constexpr bool isProcTerminator(const Instr &instr)
{
    return isProcTerminator(instr.op);
}

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_INSTR_H_
