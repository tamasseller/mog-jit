// jit-armv6m/compiler — unary op codegen (docs/design.md §10, §16 item 8),
// ported from jit-armv6m/prototype/src/unaryops.ts. NEG/NOT are single
// native instructions; CLZ/REVBITS have no ARMv6-M native form at all, so
// both go through a small, per-procedure-shared software routine reached
// by a local BL — blocks.h's emitBrTableHelper precedent, not the real,
// whole-program-linked static helper vector docs/design.md §11 envisions
// for the native target — a prototype-appropriate simplification carried
// over here rather than the final shape.
#ifndef JIT_ARMV6M_COMPILER_UNARYOPS_H_
#define JIT_ARMV6M_COMPILER_UNARYOPS_H_

#include <cstdint>
#include "instr.h"

namespace jitc {

class Emitter;

/** Placeholder BL sites collected per procedure, one list per software
 *  helper — translate_proc.cpp patches each list once its own copy of
 *  the relevant helper is emitted (mirroring blocks.h's own
 *  brTableHelperSites). Fixed capacity, no heap: this slice's own test
 *  corpus never needs more than a handful of CLZ/REVBITS sites in one
 *  procedure, and a real target bounds procedure size tightly enough that
 *  kMaxSites comfortably covers any realistic body. */
constexpr uint32_t kMaxUnaryHelperSites = 32;

struct UnaryHelperSites {
    uint32_t clz[kMaxUnaryHelperSites];
    uint32_t clzCount = 0;
    uint32_t revbits[kMaxUnaryHelperSites];
    uint32_t revbitsCount = 0;
};

/** Emit one unary op. operand must already be materialized into ACC_REG
 *  (the caller's job — a unary op's native encoding never takes an
 *  immediate form, so there's nothing to fold, only something to flush
 *  first). dest is ACC_REG or a destination-fold target. */
void emitUnary(Emitter &e, Op op, uint32_t dest, UnaryHelperSites &helperSites);

/** Count leading zeros (0..32) — a straight-line shift-and-test loop.
 *  Returns the start pc, to patch every collected CLZ helperSite against. */
uint32_t emitClzHelper(Emitter &e);

/** Reverse bit order (32-bit) — one bit per iteration. Returns the start
 *  pc, to patch every collected REVBITS helperSite against. */
uint32_t emitRevbitsHelper(Emitter &e);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_UNARYOPS_H_
