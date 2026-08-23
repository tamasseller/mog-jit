// jit-armv6m/compiler — per-opcode native codegen, ported from
// jit-armv6m/prototype/src/binops.ts. Pure instruction selection: given
// acc's current Shape, the combo's own right-hand operand, and a
// destination register, emit whichever native Thumb form fits. Nothing
// here reads or writes AccState.
#ifndef JIT_ARMV6M_COMPILER_BINOPS_H_
#define JIT_ARMV6M_COMPILER_BINOPS_H_

#include <cstdint>
#include "instr.h"
#include "shape.h"

namespace jitc {

class Emitter;

enum class BinOpKind : uint8_t { AddSubRsub, ShiftImm, TwoOpInPlace };

BinOpKind classifyBinOp(Op op, Combo combo);

/** Emit one arithmetic binary op. accShape is acc's current value;
 *  operand is combo's own right-hand side (nullptr means PEEK_PEEK,
 *  operand is dest itself — §16 item 11); dest says where the result
 *  must end up. */
void emitBinaryOp(
    Emitter &e, Op op, Combo combo,
    const Shape &accShape, const Shape *operand, uint32_t dest);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_BINOPS_H_
