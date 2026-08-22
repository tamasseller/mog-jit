// jit-armv6m/compiler — the bytecode representation this translator
// consumes. Deliberately independent of @ppl/machine's rtl.ts: same
// conceptual shapes, new types, no runtime dependency either way. A flat,
// aggregate-initializable struct (not a tagged union) so a fixture literal
// reads almost identically to its rtl.ts source.
//
// BLOCK_END/LOOP/BR_TABLE, NEG/NOT/CLZ/REVBITS, EXT, and every comparison
// opcode are deliberately absent from Op — this slice's scope boundary is
// a compile-time absence, not a runtime "not implemented" throw the way
// translateProc.ts's own gaps are.
#ifndef JIT_ARMV6M_COMPILER_INSTR_H_
#define JIT_ARMV6M_COMPILER_INSTR_H_

#include <cstdint>

namespace jitc {

enum class Op : uint8_t {
    CONST, LOAD, STORE, PUSH, POP,
    RETURN, TRAP,
    CALL,
    ADD, SUB, RSUB, MUL, AND, OR, XOR, SHL, SHR, ASR,
};

enum class Combo : uint8_t { NONE, REG_ACC, REG_REG, IMM_ACC, PEEK_PEEK, POP_ACC };

struct Instr {
    Op op;
    Combo combo = Combo::NONE;
    int32_t imm = 0;         // CONST's value / IMM_ACC operand / TRAP's code
    uint32_t target = 0;      // LOAD/STORE/REG_ACC/REG_REG's slot index k
    uint32_t calleeIndex = 0; // CALL only
};

// Builder helpers — 1:1 with packages/machine/src/rtl.ts's constructors, so
// a transcribed fixture reads like its TS source line for line.
constexpr Instr CONST(int32_t v) { return Instr{Op::CONST, Combo::NONE, v, 0, 0}; }
constexpr Instr LOAD(uint32_t target) { return Instr{Op::LOAD, Combo::NONE, 0, target, 0}; }
constexpr Instr STORE(uint32_t target) { return Instr{Op::STORE, Combo::NONE, 0, target, 0}; }
constexpr Instr PUSH() { return Instr{Op::PUSH, Combo::NONE, 0, 0, 0}; }
constexpr Instr POP() { return Instr{Op::POP, Combo::NONE, 0, 0, 0}; }
constexpr Instr bare(Op op) { return Instr{op, Combo::NONE, 0, 0, 0}; }
constexpr Instr trapInstr(uint32_t code) { return Instr{Op::TRAP, Combo::NONE, (int32_t)code, 0, 0}; }
constexpr Instr call(uint32_t calleeIndex) { return Instr{Op::CALL, Combo::NONE, 0, 0, calleeIndex}; }
constexpr Instr opReg(Op op, uint32_t target) { return Instr{op, Combo::REG_ACC, 0, target, 0}; }
constexpr Instr opRegWriteback(Op op, uint32_t target) { return Instr{op, Combo::REG_REG, 0, target, 0}; }
constexpr Instr opImm(Op op, int32_t v) { return Instr{op, Combo::IMM_ACC, v, 0, 0}; }
constexpr Instr opStack(Op op, Combo combo) { return Instr{op, combo, 0, 0, 0}; }

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_INSTR_H_
