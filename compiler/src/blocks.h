#ifndef JIT_ARMV6M_COMPILER_BLOCKS_H_
#define JIT_ARMV6M_COMPILER_BLOCKS_H_

#include <cstdint>
#include "instr.h"
#include "ext.h"
#include "shape.h"
#include "armv6.h"
#include "assembler.h"

namespace jitc
{

class Window;
class AccState;

constexpr uint32_t ORDINARY_MAX_BYTES = 16;
constexpr uint32_t CALL_MAX_BYTES = 64;
constexpr uint32_t BR_TABLE_JUMP_OVERHEAD_BYTES = 32;

uint32_t instrMaxBytes(const Instr &instr);

struct SpanResult
{
    uint32_t bytes;
    uint32_t nextPc;
};
SpanResult maxSpanBytes(const uint8_t *bytes, uint32_t bytesLen, uint32_t from, uint32_t blockCount,
    const ExtHooks *ext = nullptr);

void emitGuardedBranch(Assembler &a, Label &label, ArmV6M::Condition condition, const uint8_t *bytes,
    uint32_t bytesLen, uint32_t from, uint32_t blockCount, const ExtHooks *ext = nullptr);

// ── Comparison → branch fusion (§10.1's "zero-destination" axis) ───────

/** Emit the CMP for a comparison whose *only* consumer is the following
 *  BR_TABLE/LOOP-condition BLOCK_END — never materializes a 0/1 result.
 *  Returns the Thumb condition that's true exactly when the comparison
 *  itself is true. */
ArmV6M::Condition emitComparison(Assembler &a, AccState &accState, Op op, const Shape &operand);

/** isa-core.md §7.1/§7.2's own leniency: BR_TABLE/a LOOP condition's
 *  BLOCK_END test whatever value acc already holds, not specifically a
 *  comparison's 0/1 result — materialize whatever's pending, test it
 *  against zero explicitly, and hand back NE. */
ArmV6M::Condition testAccNonzero(Assembler &a, AccState &accState);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_BLOCKS_H_
