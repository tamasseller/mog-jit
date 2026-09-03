#ifndef JIT_ARMV6M_COMPILER_BINOPS_H_
#define JIT_ARMV6M_COMPILER_BINOPS_H_

#include <cstdint>
#include "instr.h"
#include "shape.h"
#include "armv6.h"

namespace jitc
{

class Assembler;

/* Both return whether the last instruction they emitted left N/Z set from
 * `dest`, which is what lets a following truthy branch skip its own CMP. */
bool emitBinaryOp(Assembler &e, Op op, Combo combo, const Shape &accShape, const Shape &rhs, uint32_t dest);

ArmV6M::Condition emitComparison(Assembler &a, Shape left, Op op, const Shape &operand);

bool emitUnary(Assembler &e, Op op, uint32_t dest, uint32_t src);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_BINOPS_H_
