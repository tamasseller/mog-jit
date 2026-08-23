// jit-armv6m/compiler — unary op codegen (docs/design.md §10). NEG/NOT are
// single native instructions; CLZ/REVBITS have no ARMv6-M native form at
// all, so both go through the flash-resident static helper vector
// (docs/design.md §11's reserved slots 4/5 — jit-armv6m/runtime/
// runtime.S's clzHelper/revbitsHelper), reached the same way
// callHelper/returnHelper* already are.
#ifndef JIT_ARMV6M_COMPILER_UNARYOPS_H_
#define JIT_ARMV6M_COMPILER_UNARYOPS_H_

#include <cstdint>
#include "instr.h"

namespace jitc
{

class Emitter;

/** Emit one unary op. operand must already be materialized into ACC_REG
 *  (the caller's job — a unary op's native encoding never takes an
 *  immediate form, so there's nothing to fold, only something to flush
 *  first). dest is ACC_REG or a destination-fold target. */
void emitUnary(Emitter &e, Op op, uint32_t dest);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_UNARYOPS_H_
