#ifndef JIT_ARMV6M_COMPILER_BINOPS_H_
#define JIT_ARMV6M_COMPILER_BINOPS_H_

#include <cstdint>
#include "instr.h"
#include "shape.h"
#include "armv6.h"

namespace jitc
{

class Assembler;

void emitBinaryOp(Assembler &e, Op op, Combo combo, const Shape &accShape, const Shape &rhs, uint32_t dest);

ArmV6M::Condition emitComparison(Assembler &a, Shape left, Op op, const Shape &operand);

void emitUnary(Assembler &e, Op op, uint32_t dest, uint32_t src);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_BINOPS_H_
