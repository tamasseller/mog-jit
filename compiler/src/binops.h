// jit-armv6m/compiler — per-opcode native codegen (docs/design.md §10.1).
// Pure instruction selection: given acc's current Shape, the combo's own
// right-hand operand, and a destination register, emit whichever native
// Thumb form fits. Nothing here reads or writes AccState.
#ifndef JIT_ARMV6M_COMPILER_BINOPS_H_
#define JIT_ARMV6M_COMPILER_BINOPS_H_

#include <cstdint>
#include "instr.h"
#include "shape.h"

namespace jitc
{

class Assembler;

void emitBinaryOp(Assembler &e, Op op, Combo combo, const Shape &accShape, const Shape &rhs, uint32_t dest);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_BINOPS_H_
