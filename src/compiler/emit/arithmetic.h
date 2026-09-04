#ifndef JIT_ARMV6M_COMPILER_BINOPS_H_
#define JIT_ARMV6M_COMPILER_BINOPS_H_

#include <cstdint>
#include "instr.h"
#include "shape.h"
#include "effect.h"
#include "armv6.h"

namespace jitc
{

class Assembler;

Effect emitBinaryOp(Assembler &e, Op op, Combo combo, const Shape &accShape, const Shape &rhs, uint32_t dest);

Effect emitComparison(Assembler &a, Shape left, Op op, const Shape &operand);

Effect emitUnary(Assembler &e, Op op, uint32_t dest, uint32_t src);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_BINOPS_H_
