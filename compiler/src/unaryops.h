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

class Assembler;

/** Emit one unary op. NEG/NOT read `src` directly — negs/mvns's native
 *  encoding has an independent source register field, so the caller need
 *  not flush the operand into any particular register first. CLZ/REVBITS
 *  have no such freedom: they dispatch through a fixed helper-vector
 *  subroutine (runtime.S's clzHelper/revbitsHelper) that hardcodes
 *  ACC_REG as both argument and return register, so `src` must be
 *  ACC_REG for those two (asserted). dest is ACC_REG or a
 *  destination-fold target. */
void emitUnary(Assembler &e, Op op, uint32_t dest, uint32_t src);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_UNARYOPS_H_
